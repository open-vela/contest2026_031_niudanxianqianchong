---
name: smart_home_device_control
description: Demo device control by explicit ID; physical devices require their own source tools.
priority: 85
flags: enabled,llm_visible,summary_only
---

# Smart Home Device Control Policy

Device inventory:

- The current device list is dynamic and can be changed from the Panel UI.
- Do not assume default rooms or devices. Discover current candidates with `get_device_inventory(source="demo")`.
- `get_home_status` contains only simulated local state, not all physical devices. Query the relevant source before control.
- Do not add, edit, rename, move, or delete devices through tools. Device management belongs to the Panel UI.
- `set_light` and `set_ac` require the exact `device_id` and room from discovery. If several devices match, ask the user; never select the first.

Light rules:

- If the user says to turn a light on without brightness, choose a comfortable brightness such as 60.
- If the user asks to turn a light off, set `on=false` and `brightness=0`.
- If the user says the room is too dark, use `set_light`, not AC.

AC rules:

- Control only an AC returned by discovery. Demo success does not prove any physical AC changed.
- Cool down: `set_ac` with `mode="cool"`, `fan_speed="auto"`, 24C-26C.
- Warm up: `mode="heat"`, 24C-27C.
- Dehumidify: `mode="dry"`; omit `temperature` unless explicitly requested.
- Fan only: `mode="fan"`; omit `temperature` unless explicitly requested.
- If the user only changes fan speed, omit mode and temperature so current AC state is preserved.

Environment-aware rules:

- `get_home_status.environment` contains simulated temperature, humidity, and ambient-light inputs.
- When `get_indoor_environment` is registered, use it for a user's request
  about the current indoor temperature or humidity. Its AHT30 result is a
  fresh physical measurement; do not describe the simulated environment as a
  sensor reading.
- If temperature is above 28C and the user asks for comfort or cooling, prefer bedroom AC cooling.
- If temperature is below 18C, avoid cooling unless explicitly requested.
- If ambient light is below 80lx and the user says it is dark, use `set_light`.
- If humidity is above 75%, mention high humidity and use `dry` mode only if the user asks for dehumidifying.
