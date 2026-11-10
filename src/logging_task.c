/*
 * logging_task.c - Persistent Logging Task
 *
 * FreeRTOS Concepts Demonstrated:
 *   1. Dedicated logging task — a common RTOS pattern where one task owns
 *      all I/O, eliminating printf races from other tasks.
 *   2. xQueueReceive — blocks waiting for processed data entries.
 *   3. Mutex contention demo — if two tasks try to log simultaneously,
 *      one must wait (demonstrating mutual exclusion overhead).
 *   4. Priority 3 — intentionally lower priority than processing (4) and
 *      alert (6), so urgent tasks can preempt logging.
 *
 * In a real embedded system, this task would write to UART, SD card, or
 * a circular log buffer in flash. Here we use colored console output.
 */

#include "logging_task.h"
#include <string.h>

/* =========================================================================
 * Global Objects
 * ========================================================================= */

volatile uint32_t g_totalLogEntries = 0;

/*
 * FreeRTOS Concept: Logging Mutex
 * This mutex ensures only ONE task writes to the log at a time.
 * Without it, two tasks calling printf simultaneously would produce garbled
 * interleaved output — a classic RTOS race condition.
 *
 * In this simulator, the logging task is the ONLY writer, so the mutex
 * mainly demonstrates the concept and measures contention.
 * In a larger system, multiple tasks might call a log API that internally
 * takes this mutex.
 */
SemaphoreHandle_t xLoggingMutex = NULL;

/* =========================================================================
 * vLoggingTask
 * ========================================================================= */

void vLoggingTask(void* pvParameters) {
    (void)pvParameters;

    sim_printf(ANSI_MAGENTA ANSI_BOLD
        "[LOGGER] Task started. Priority=%u. Waiting for processed data...\n"
        ANSI_RESET, TASK_PRIORITY_NORMAL);

    uint32_t logSeq = 0;

    while (g_schedulerRunning) {
        ProcessedData_t pdata;

        /* ----------------------------------------------------------------
         * Block on the processed queue — wait up to 2 seconds.
         *
         * FreeRTOS Concept: This task spends most of its time BLOCKED here,
         * consuming zero CPU. The scheduler only runs it when there's data.
         *
         * Priority 3 means: even when this task is READY (has data), it won't
         * run if SensorTask(5), ProcessingTask(4), or AlertTask(6) are READY.
         * Those higher-priority tasks always go first — logging waits.
         * ---------------------------------------------------------------- */
        BaseType_t received = xQueueReceive(xProcessedQueue, &pdata, pdMS_TO_TICKS(2000));

        if (received != pdPASS) {
            /* Timeout — normal if we're between measurement cycles */
            continue;
        }

        logSeq++;

        /* ----------------------------------------------------------------
         * Acquire logging mutex before writing.
         *
         * FreeRTOS Concept: This demonstrates MUTEX CONTENTION.
         * If multiple tasks shared this mutex (e.g., a debug task also
         * uses it), one would block here, and g_totalMutexContentions
         * would increment.
         *
         * portMAX_DELAY = wait forever; logger must not drop entries.
         * ---------------------------------------------------------------- */
        xSemaphoreTake(xLoggingMutex, portMAX_DELAY);

        /* ---- CRITICAL SECTION: writing the log entry ---- */
        {
            /* Choose color based on data quality */
            const char* color   = ANSI_MAGENTA;
            const char* quality = "OK";

            if (pdata.anomaly_detected) {
                color   = ANSI_RED_BOLD;
                quality = "ANOMALY";
            } else if (pdata.temp_filtered > 32.0f || pdata.pressure_filtered < 980.0f) {
                color   = ANSI_YELLOW;
                quality = "WARN";
            }

            /* Format a detailed log line */
            printf("%s[LOG  t=%5ums #%03u/%03u] "
                   "T=%5.1f°C (raw %5.1f) | P=%7.1f hPa (raw %7.1f) | "
                   "H=%4.1f%% | %s\n" ANSI_RESET,
                   color,
                   pdata.timestamp_ms,
                   logSeq,
                   pdata.sequence,
                   pdata.temp_filtered,    pdata.temp_raw,
                   pdata.pressure_filtered, pdata.pressure_raw,
                   pdata.humidity_filtered,
                   quality);
            fflush(stdout);
        }
        /* ---- END CRITICAL SECTION ---- */

        xSemaphoreGive(xLoggingMutex);

        InterlockedIncrement((LONG*)&g_totalLogEntries);

        /*
         * FreeRTOS Concept: NO explicit vTaskDelay here.
         * The logging task naturally rate-limits itself by blocking on the
         * queue. It runs only when there's data to log — perfectly reactive.
         *
         * This is BETTER than:
         *   while(1) { log_if_data_available(); vTaskDelay(10); }
         * ...because that adds unnecessary 10ms latency to every log entry.
         */
    }

    sim_printf(ANSI_MAGENTA "[LOGGER] Task exiting.\n" ANSI_RESET);
    vTaskDelete(NULL);
}
