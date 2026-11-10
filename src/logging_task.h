/*
 * logging_task.h - Logging Task Interface
 *
 * FreeRTOS Concept: Dedicated logging task at medium priority.
 * Demonstrates:
 *   - Queue consumer (xProcessedQueue)
 *   - Mutex-serialized output (logging mutex)
 *   - Lower priority than sensor/processing — log output may be delayed
 *     under high load (which is intentional: logging is best-effort)
 *
 * Priority: TASK_PRIORITY_NORMAL (3)
 */

#ifndef LOGGING_TASK_H
#define LOGGING_TASK_H

#include "freertos_sim.h"
#include "processing_task.h"

extern volatile uint32_t g_totalLogEntries;
extern SemaphoreHandle_t xLoggingMutex;

void vLoggingTask(void* pvParameters);

#endif /* LOGGING_TASK_H */
