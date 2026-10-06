#pragma once
#include "freertos/FreeRTOS.h"

void nimble_port_freertos_init(TaskFunction_t host_task_fn);
void nimble_port_freertos_deinit(void);
