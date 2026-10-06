#pragma once
/* Monotonic microsecond clock, as esp_timer_get_time() provides on device. */
#include <stdint.h>

int64_t esp_timer_get_time(void);
