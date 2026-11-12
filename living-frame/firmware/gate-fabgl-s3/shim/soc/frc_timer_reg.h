// PROBE ONLY: stub for FabGL 1.0.9's unconditional ESP32-only include.
#pragma once
#define FRC_TIMER_LOAD_REG(i)   (0x3FF00000 + 0x00)
#define FRC_TIMER_COUNT_REG(i)  (0x3FF00000 + 0x04)
#define FRC_TIMER_CTRL_REG(i)   (0x3FF00000 + 0x08)
#define FRC_TIMER_ENABLE        (1 << 8)
