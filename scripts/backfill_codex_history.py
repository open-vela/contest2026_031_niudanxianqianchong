#!/usr/bin/env python3
"""Backfill explicitly audited Codex transcripts into contest logs.

This tool deliberately does not discover or classify sessions.  The caller must
pass each transcript explicitly after auditing that the entire session belongs
to this contest repository.  It preserves whole conversation events rather than
filtering individual messages by path.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "1.0"
GENERATOR = "codex-history-backfill@1.0"
DEFAULT_REDACT_RULES = [
    (re.compile(r"sk-[A-Za-z0-9_-]{20,}"), "sk-***REDACTED***"),
    (re.compile(r"ghp_[A-Za-z0-9]{36}"), "ghp_***REDACTED***"),
    (re.compile(r"Bearer\s+[A-Za-z0-9._\-+/=]+"), "Bearer ***REDACTED***"),
]


def parse_json(value: Any) -> Any:
    if not isinstance(value, str):
        return value
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return value


def redact(value: Any, counter: list[int]) -> Any:
    if isinstance(value, str):
        for pattern, replacement in DEFAULT_REDACT_RULES:
            value, changed = pattern.subn(replacement, value)
            counter[0] += changed
        return value
    if isinstance(value, list):
        return [redact(item, counter) for item in value]
    if isinstance(value, dict):
        return {key: redact(item, counter) for key, item in value.items()}
    return value


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def date_from_iso(value: str) -> str:
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).date().isoformat()
    except ValueError:
        return datetime.now(timezone.utc).date().isoformat()


def transcript_events(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Read one Codex transcript and emit its non-duplicated visible events."""
    meta: dict[str, Any] = {}
    events: list[dict[str, Any]] = []
    tool_names: dict[str, str] = {}

    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            raw = json.loads(line)
        except json.JSONDecodeError:
            print(f"warning: {path}:{line_number}: invalid JSON skipped", file=sys.stderr)
            continue

        payload = raw.get("payload")
        if not isinstance(payload, dict):
            continue
        top_type = raw.get("type")
        payload_type = payload.get("type")
        timestamp = raw.get("timestamp") or payload.get("timestamp") or meta.get("timestamp")

        if top_type == "session_meta":
            meta = payload
            continue

        # Codex records response_item messages and event_msg messages in pairs.
        # event_msg is the visible canonical stream, so use it to avoid duplicates.
        if top_type == "event_msg" and payload_type == "user_message":
            text = payload.get("message")
            if isinstance(text, str) and text:
                events.append({"ts": timestamp, "role": "user", "text": text})
            continue

        if top_type == "event_msg" and payload_type == "agent_message":
            text = payload.get("message")
            if isinstance(text, str) and text:
                events.append({"ts": timestamp, "role": "assistant", "text": text})
            continue

        if top_type == "response_item" and payload_type in ("function_call", "custom_tool_call"):
            call_id = str(payload.get("call_id") or payload.get("id") or f"call-{line_number}")
            name = str(payload.get("name") or "unknown")
            tool_names[call_id] = name
            raw_input = payload.get("arguments") if payload_type == "function_call" else payload.get("input")
            events.append({
                "ts": timestamp,
                "role": "tool",
                "tool_name": name,
                "tool_call_id": call_id,
                "input": parse_json(raw_input),
                "output": None,
            })
            continue

        if top_type == "response_item" and payload_type in ("function_call_output", "custom_tool_call_output"):
            call_id = str(payload.get("call_id") or payload.get("id") or f"output-{line_number}")
            events.append({
                "ts": timestamp,
                "role": "tool",
                "tool_name": tool_names.get(call_id, "<result>"),
                "tool_call_id": call_id,
                "input": None,
                "output": parse_json(payload.get("output")),
            })
            continue

        if top_type == "event_msg" and payload_type == "patch_apply_end":
            call_id = str(payload.get("call_id") or f"patch-{line_number}")
            events.append({
                "ts": timestamp,
                "role": "tool",
                "tool_name": "apply_patch",
                "tool_call_id": call_id,
                "input": {"changes": payload.get("changes")},
                "output": {
                    "stdout": payload.get("stdout"),
                    "stderr": payload.get("stderr"),
                    "success": payload.get("success"),
                    "status": payload.get("status"),
                },
            })
            continue

        if top_type == "event_msg" and payload_type == "mcp_tool_call_end":
            call_id = str(payload.get("call_id") or f"mcp-{line_number}")
            tool_name = "/".join(
                part for part in (payload.get("app_name"), payload.get("action_name")) if part
            ) or "mcp_tool"
            events.append({
                "ts": timestamp,
                "role": "tool",
                "tool_name": tool_name,
                "tool_call_id": call_id,
                "input": payload.get("invocation"),
                "output": payload.get("result"),
            })

    if not meta:
        raise ValueError(f"{path}: session_meta not found")
    return meta, events


def load_manifest(path: Path, team_id: str, github_login: str) -> dict[str, Any]:
    if not path.exists():
        return {
            "schema_version": SCHEMA_VERSION,
            "team_id": team_id,
            "github_login": github_login,
            "generator": GENERATOR,
            "sessions": [],
        }
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("team_id") != team_id:
        raise ValueError("manifest team_id differs from --team-id")
    if manifest.get("github_login") != github_login:
        raise ValueError("manifest github_login differs from --github-login")
    return manifest


def convert_one(path: Path, args: argparse.Namespace, manifest: dict[str, Any]) -> tuple[Path, dict[str, Any], list[dict[str, Any]]]:
    meta, events = transcript_events(path)
    session_id = str(meta.get("session_id") or meta.get("id") or path.stem)
    started_at = str(meta.get("timestamp") or (events[0].get("ts") if events else now_iso()))
    datedir = date_from_iso(started_at)
    output_path = args.dest / "logs" / args.github_login / datedir / f"codex__{session_id}.jsonl"
    if output_path.exists():
        raise ValueError(f"refusing to overwrite existing log: {output_path}")
    if not events:
        raise ValueError(f"{path}: no visible user, assistant, or tool events")

    normalized: list[dict[str, Any]] = []
    redacted_total = 0
    for seq, event in enumerate(events):
        event = dict(event)
        event["ts"] = str(event.get("ts") or started_at)
        count = [0]
        event = redact(event, count)
        redacted_total += count[0]
        out = {
            "schema_version": SCHEMA_VERSION,
            "session_id": session_id,
            "team_id": args.team_id,
            "github_login": args.github_login,
            "tool": "codex",
            "seq": seq,
            **event,
        }
        if count[0]:
            out["redacted_count"] = count[0]
        normalized.append(out)

    last_at = normalized[-1]["ts"]
    entry = {
        "session_id": session_id,
        "tool": "codex",
        "started_at": started_at,
        "last_event_at": last_at,
        "event_count": len(normalized),
        "file_path": str(output_path.relative_to(args.dest)),
        "collection_mode": "vscode_extension" if meta.get("source") == "vscode" else "cli",
        "health": "ok",
        "redacted_count_total": redacted_total,
    }
    return output_path, entry, normalized


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transcript", type=Path, action="append", required=True,
                        help="Audited Codex transcript path; repeat once per complete session")
    parser.add_argument("--dest", type=Path, required=True, help="Contest repository root")
    parser.add_argument("--team-id", required=True)
    parser.add_argument("--github-login", required=True)
    parser.add_argument("--confirm", action="store_true", help="Write logs; omit for preview")
    args = parser.parse_args()
    args.dest = args.dest.resolve()

    manifest_path = args.dest / "logs" / args.github_login / "manifest.json"
    manifest = load_manifest(manifest_path, args.team_id, args.github_login)
    known_ids = {entry.get("session_id") for entry in manifest.get("sessions", [])}
    planned = []
    for transcript in args.transcript:
        transcript = transcript.resolve()
        output_path, entry, events = convert_one(transcript, args, manifest)
        if entry["session_id"] in known_ids:
            raise ValueError(f"session already declared in manifest: {entry['session_id']}")
        known_ids.add(entry["session_id"])
        planned.append((output_path, entry, events))
        print(f"{'write' if args.confirm else 'preview'} {entry['session_id']}: {len(events)} events -> {output_path}")

    if not args.confirm:
        return 0
    for output_path, _, events in planned:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(
            "".join(json.dumps(event, ensure_ascii=False) + "\n" for event in events),
            encoding="utf-8",
        )
    manifest["sessions"].extend(entry for _, entry, _ in planned)
    manifest["schema_version"] = SCHEMA_VERSION
    manifest["team_id"] = args.team_id
    manifest["github_login"] = args.github_login
    manifest["generator"] = GENERATOR
    manifest["updated_at"] = now_iso()
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
