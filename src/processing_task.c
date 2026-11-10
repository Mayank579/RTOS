/*
 * processing_task.c - Signal Processing Task Implementation
 *
 * FreeRTOS Concepts Demonstrated:
 *   1. xQueueReceive — consuming from sensor queue (blocks when empty)
 *   2. xQueueSend    — producing to processed queue
 *   3. xSemaphoreGive (binary sem) — signaling alert task on anomaly
 *   4. Mutex-protected shared statistics
 *   5. Stateful task — maintains circular buffer for moving average
 *
 * The moving average filter is a classic example of WHY tasks need private
 * state. Each task has its own stack and static locals — no sharing needed.
 */

#include "processing_task.h"
#include <string.h>
#include <float.h>

/* =========================================================================
 * Global Objects
 * ========================================================================= */

/* Queue: holds up to 10 ProcessedData_t items */
QueueHandle_t     xProcessedQueue  = NULL;

/*
 * FreeRTOS Concept: Binary Semaphore used for TASK NOTIFICATION.
 * The processing task GIVES this when an anomaly is detected.
 * The alert task TAKES this, blocking until there's something to alert on.
 *
 * Key difference vs. mutex:
 *   - Semaphore: no ownership, used for SIGNALING (like a doorbell)
 *   - Mutex: has ownership, used for MUTUAL EXCLUSION (like a bathroom key)
 */
SemaphoreHandle_t xAlertSemaphore  = NULL;

/* Mutex for shared statistics struct */
SemaphoreHandle_t xStatsMutex      = NULL;

ProcessingStats_t g_processingStats = {
    .total_processed   = 0,
    .anomalies_detected = 0,
    .max_temp_seen     = -FLT_MAX,
    .min_pressure_seen = FLT_MAX,
    .max_pressure_seen = -FLT_MAX
};

volatile uint32_t g_totalProcessed = 0;

/* =========================================================================
 * Circular Buffer Filter State (private to this task)
 *
 * FreeRTOS Concept: Static local variables in a task function persist
 * between calls (the task runs in an infinite loop — stack is preserved).
 * No other task can access these — perfect private state.
 * ========================================================================= */

typedef struct {
    float   samples[FILTER_SIZE];
    uint8_t index;
    uint8_t filled;    /* How many samples have been filled (0..FILTER_SIZE) */
} CircularFilter_t;

static void filter_init(CircularFilter_t* f) {
    memset(f->samples, 0, sizeof(f->samples));
    f->index  = 0;
    f->filled = 0;
}

static float filter_add(CircularFilter_t* f, float value) {
    f->samples[f->index] = value;
    f->index = (f->index + 1) % FILTER_SIZE;
    if (f->filled < FILTER_SIZE) f->filled++;

    /* Compute moving average over filled samples */
    float sum = 0.0f;
    for (uint8_t i = 0; i < f->filled; i++) {
        sum += f->samples[i];
    }
    return sum / (float)f->filled;
}

/* =========================================================================
 * vProcessingTask
 * ========================================================================= */

void vProcessingTask(void* pvParameters) {
    (void)pvParameters;

    /* Private filter state — lives on this task's stack, not shared */
    CircularFilter_t tempFilter, pressFilter, humFilter;
    filter_init(&tempFilter);
    filter_init(&pressFilter);
    filter_init(&humFilter);

    sim_printf(ANSI_CYAN_BOLD
        "[PROCESS] Task started. Priority=%u. Waiting for sensor data...\n"
        ANSI_RESET, TASK_PRIORITY_ABOVE_NORMAL);

    while (g_schedulerRunning) {
        SensorData_t raw;

        /* ----------------------------------------------------------------
         * Step 1: Block on sensor queue — wait up to 1 second for data.
         *
         * FreeRTOS Concept: xQueueReceive puts this task in BLOCKED state
         * while the queue is empty. The CPU is FREE to run other tasks
         * (SensorTask, AlertTask, etc.) while we wait.
         *
         * This is MUCH better than:
         *   while (queue_empty()) {}  ← busy-wait, wastes 100% CPU!
         * ---------------------------------------------------------------- */
        BaseType_t received = xQueueReceive(xSensorQueue, &raw, pdMS_TO_TICKS(1000));

        if (received != pdPASS) {
            /* Timeout — no data from sensor task; sensor might have crashed */
            sim_printf(ANSI_YELLOW
                "[PROCESS] WARNING: No sensor data received for 1000ms. "
                "Is SensorTask running?\n" ANSI_RESET);
            continue;
        }

        /* ----------------------------------------------------------------
         * Step 2: Apply moving average filter.
         *
         * This reduces noise by averaging the last FILTER_SIZE=5 readings.
         * No mutex needed — these circular buffers are PRIVATE to this task.
         * ---------------------------------------------------------------- */
        ProcessedData_t processed;
        processed.timestamp_ms      = raw.timestamp_ms;
        processed.sequence          = raw.sequence;
        processed.temp_raw          = raw.temperature;
        processed.pressure_raw      = raw.pressure;
        processed.temp_filtered     = filter_add(&tempFilter,  raw.temperature);
        processed.pressure_filtered = filter_add(&pressFilter, raw.pressure);
        processed.humidity_filtered = filter_add(&humFilter,   raw.humidity);

        /* ----------------------------------------------------------------
         * Step 3: Anomaly detection on FILTERED values.
         * Using filtered values reduces false positives from noise spikes.
         * ---------------------------------------------------------------- */
        processed.temp_high      = (processed.temp_filtered     > ANOMALY_TEMP_HIGH);
        processed.pressure_low   = (processed.pressure_filtered < ANOMALY_PRESSURE_LOW);
        processed.pressure_high  = (processed.pressure_filtered > ANOMALY_PRESSURE_HIGH);
        processed.anomaly_detected = processed.temp_high ||
                                     processed.pressure_low ||
                                     processed.pressure_high;

        /* Detailed processing log */
        const char* anomaly_str = processed.anomaly_detected ?
            ANSI_RED " *** ANOMALY ***" ANSI_RESET : "";

        sim_printf(ANSI_CYAN
            "[PROCESS t=%5ums] #%03u | T_filt: %5.1f°C | P_filt: %7.1f hPa | "
            "H_filt: %4.1f%%%s\n" ANSI_RESET,
            raw.timestamp_ms, raw.sequence,
            processed.temp_filtered,
            processed.pressure_filtered,
            processed.humidity_filtered,
            anomaly_str);

        /* ----------------------------------------------------------------
         * Step 4: Update shared statistics under mutex.
         *
         * FreeRTOS Concept: g_processingStats is shared — without a mutex,
         * the logging task could read a PARTIALLY UPDATED struct (e.g., see
         * total_processed incremented but anomalies_detected not yet).
         *
         * With the mutex, the entire update is ATOMIC from other tasks' view.
         * ---------------------------------------------------------------- */
        xSemaphoreTake(xStatsMutex, portMAX_DELAY);
        {
            g_processingStats.total_processed++;
            if (processed.anomaly_detected) g_processingStats.anomalies_detected++;
            if (processed.temp_filtered > g_processingStats.max_temp_seen)
                g_processingStats.max_temp_seen = processed.temp_filtered;
            if (processed.pressure_filtered < g_processingStats.min_pressure_seen)
                g_processingStats.min_pressure_seen = processed.pressure_filtered;
            if (processed.pressure_filtered > g_processingStats.max_pressure_seen)
                g_processingStats.max_pressure_seen = processed.pressure_filtered;
        }
        xSemaphoreGive(xStatsMutex);

        InterlockedIncrement((LONG*)&g_totalProcessed);

        /* ----------------------------------------------------------------
         * Step 5: Send processed data to logging queue.
         * ---------------------------------------------------------------- */
        xQueueSend(xProcessedQueue, &processed, pdMS_TO_TICKS(50));

        /* ----------------------------------------------------------------
         * Step 6: Signal alert task if anomaly detected.
         *
         * FreeRTOS Concept: xSemaphoreGive on a BINARY SEMAPHORE is used
         * for one-shot signaling — like ringing a bell.
         * The alert task (high priority) is waiting on this semaphore.
         * When we Give it, the RTOS IMMEDIATELY preempts us (processing P4)
         * and runs the alert task (P6) — because P6 > P4.
         *
         * This is called "task synchronization via semaphore" — the most
         * common pattern for ISR→task or task→task event notification.
         * ---------------------------------------------------------------- */
        if (processed.anomaly_detected) {
            sim_printf(ANSI_RED_BOLD
                "[PROCESS t=%5ums] ANOMALY: %s%s%s — posting to xAlertSemaphore\n"
                ANSI_RESET,
                raw.timestamp_ms,
                processed.temp_high     ? "TEMP_HIGH "   : "",
                processed.pressure_low  ? "PRESS_LOW "   : "",
                processed.pressure_high ? "PRESS_HIGH"   : "");

            xSemaphoreGive(xAlertSemaphore);
            /* At this point, the alert task (P6) will preempt us and run */
        }
    }

    sim_printf(ANSI_CYAN "[PROCESS] Task exiting.\n" ANSI_RESET);
    vTaskDelete(NULL);
}
