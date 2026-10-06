#pragma once
/*
 * pthread-backed FreeRTOS shim shared by freertos/{FreeRTOS,queue,task,semphr}.h.
 * 1 tick == 1 ms so pdMS_TO_TICKS() is the identity on the host.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;

#define pdTRUE          1
#define pdFALSE         0
#define pdPASS          1
#define pdFAIL          0
#define portMAX_DELAY   ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configTICK_RATE_HZ 1000

typedef void (*TaskFunction_t)(void *);
typedef struct fake_queue *QueueHandle_t;
typedef struct fake_task  *TaskHandle_t;
typedef struct fake_sem   *SemaphoreHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize);
BaseType_t    xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t    xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks);

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_depth,
                       void *arg, UBaseType_t priority, TaskHandle_t *out);

void vTaskDelay(TickType_t ticks);

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);
