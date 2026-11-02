# SPDX-License-Identifier: Apache-2.0
"""Observe real TTS log output with injected HTTP bodies and monotonic time."""
import subprocess
import sys

result = subprocess.run([sys.argv[1], "--diagnostics"], capture_output=True,
                        text=True, check=True, timeout=10)
output = result.stdout
assert "secret-cookie" not in output
assert "Set-Cookie" not in output
assert "test-tts-only" not in output
for number, state, body, callbacks, last, idle in (
    (1, "short", 4, 1, 500, 30500),
    (2, "matched", 8, 2, 1000, 30000),
    (3, "transfer-encoded", 8, 2, 1000, 30000),
    (4, "unknown", 8, 2, 1000, 30000),
    (5, "invalid", 8, 2, 1000, 30000),
    (6, "unknown", 0, 0, 0, 0),
):
    case = output.split(f"CASE {number}\n", 1)[1].split("CASE ", 1)[0]
    assert "tls handshake ok test elapsed_ms=100" in case, case
    summary = next(line for line in case.splitlines() if line.startswith("[tts] http: ret="))
    for field in ("ret=-7", f"status={200 if number != 6 else 0}",
                  f"body_len={body}", "elapsed_ms=31000", f"callbacks={callbacks}",
                  f"last_body_ms={last}", f"body_idle_ms={idle}",
                  f"length_state={state}", "deadline_expired=1",
                  f"headers_seen={int(number != 6)}"):
        assert field in summary, (field, summary)
    if number != 6:
        assert "first_body_ms=500" in summary and "headers_cb_ms=500" in summary
        assert "Connection: keep-alive" in case
        assert "Content-Type: application/json" in case
    if number in (1, 2, 3):
        assert "length_known=1 expected_len=8" in summary
    print(f"PASS case {number}: {state}; original timeout preserved")
print("PASS: timing, byte counts, header whitelist, missing/overflow length and per-request reset")
