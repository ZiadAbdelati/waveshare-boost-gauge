#pragma once
/*
 * printf-backed esp_log shim. ESP_LOGx(tag, fmt, ...) keeps the real call
 * shape (tag first, then a printf format) so the driver source compiles
 * unmodified.
 */
#include <stdio.h>
#include "esp_err.h"

#define ESP_LOGE(tag, ...) do { printf("[E][%s] ", (tag)); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define ESP_LOGW(tag, ...) do { printf("[W][%s] ", (tag)); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define ESP_LOGI(tag, ...) do { printf("[I][%s] ", (tag)); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define ESP_LOGD(tag, ...) do { } while (0)
#define ESP_LOGV(tag, ...) do { } while (0)
