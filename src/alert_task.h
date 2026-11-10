/*
 * alert_task.h - Alert Task Interface
 *
 * FreeRTOS Concept: Highest-priority REACTIVE task.
 * Waits on a binary semaphore — instantly preempts other tasks when signaled.
 * Also monitors a watchdog counter to detect unresponsive tasks.
 *
 * Priority: TASK_PRIORITY_ALERT (6) — above all sensor/processing/logging tasks.
 *
 * This demonstrates the classic ISR→Task notification pattern:
 *   Processing task (plays role of "ISR") → xSemaphoreGive → Alert task wakes
 */

#ifndef ALERT_TASK_H
#define ALERT_TASK_H

#include "freertos_sim.h"
#include "processing_task.h"

extern volatile uint32_t g_totalAlertsFired;
extern volatile uint32_t g_watchdogCounter;

void vAlertTask(void* pvParameters);

#endif /* ALERT_TASK_H */
