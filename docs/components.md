# Components

[English](components.md) | [简体中文](components_ZH.md)

The examples use the managed Waveshare board component
`waveshare/esp32_s3_touch_amoled_1_75c` (`^3.0.0`). The current release supports
ESP-IDF 5.5 and later; CI verifies the product examples with ESP-IDF `v5.5.5`
and `v6.0.2`.

Repository-local components have distinct ownership and are intentionally retained:

- `brookesia_app_squareline_demo` is product UI feature code used by the Brookesia example.
- `brookesia_core` is an embedded upstream framework. Preserve its source, attribution, and
  documentation unless an explicit upstream synchronization is performed.
- `bsp_extra` is board-local audio glue layered on the managed BSP and codec APIs.

Future component work should prefer managed Waveshare and Espressif components when an equivalent component is available and compatible with the selected CI matrix.

Keep repository-local glue such as board-specific demo composition, temporary compatibility code,
and example-only assets near the example that consumes it. Reusable BSP, display, touch, sensor,
audio, and bus fixes should move to the shared component source first when possible. Component names
or directory placement alone are not sufficient evidence for removal.

## Hardware Cross-Check Boundary

A read-only comparison against the repository schematic confirmed the maintained display data,
clock, and chip-select values, the touch I2C/interrupt values, and the audio clock/data values used by
the Arduino board header. Display reset/TE, QMI8658 interrupt, and USB signal constants are not all
duplicated in that header, so the schematic and managed BSP remain authoritative for those signals.
No pin definitions were changed during the CI and documentation update; runtime behavior still needs
physical-board validation after the build matrix passes.
