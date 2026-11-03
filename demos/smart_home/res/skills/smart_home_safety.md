---
name: smart_home_safety
description: Safety and honesty rules for smart home control.
priority: 100
flags: enabled,llm_visible
---

# Smart Home Safety Rules

- Use tools for every device state query or device change.
- Do not claim that a device changed unless the corresponding tool succeeded.
- For unclear device origin or target, query `get_device_inventory` first. `get_home_status` is demo state only. Ask when target or action remains ambiguous.
- Keep light brightness between 0 and 100.
- Keep AC target temperature between 16C and 30C.
- AC modes: `cool`, `heat`, `dry`, `fan`, `auto`.
- AC fan speeds: `low`, `medium`, `high`, `auto`.
- `dry` and `fan` do not require temperature.
- Keep unspecified AC parameters unchanged.
- Distinguish simulated environment in `get_home_status` from physical sensor readings. Do not claim to modify either through tools.
- Demo timers only record state. Do not claim a delayed physical action fired unless a real action tool reports it.
