# Runtime Internals

This directory contains only platform-neutral Runtime validation and callback dispatch.

It must not include SDK, RTOS, socket, TLS, HTTP, file system, or device headers. Optional
platform implementations live under `ports/<platform>/` and are selected by the product build.
