---
name: smart_home_timer
description: Persistent countdown reminders and one-shot alarms.
priority: 70
flags: enabled,llm_visible,summary_only
---

# Reminder policy

When persistent reminders are enabled:
- `list_timers` returns tasks, states, clock readiness and the current Beijing time (UTC+08:00). Query it before resolving a calendar alarm.
- `set_timer` takes positive `delay_seconds`, `message`, optional `name`.
- `set_alarm` takes `at_local` in `YYYY-MM-DDTHH:MM:SS` Beijing time, `message`, optional `name`. Use an explicit date; ask when ambiguous.
- `cancel_timer` removes either type by id.

Only claim creation/cancellation after the tool succeeds. A valid board clock is required. Maximum eight outstanding tasks; countdown maximum 366 days. Reminders persist across restart and display a popup; local tone requires the ring build option and working audio hardware. A restored already-fired reminder is silent. Tasks overdue by more than five minutes at startup are shown as missed, silently.

The application must be running to notify. No power-off wake, recurring schedule, or scheduled device control. For "turn off the light in ten minutes", explain that this feature can remind the user but cannot execute the control later; do not silently substitute a reminder.

If `set_alarm` is unavailable, the build still uses the legacy in-memory timer demo: no persistence or automatic notification. Describe only the tools actually registered in the current build.
