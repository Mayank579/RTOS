/*
 * sensor_task.c - Sensor Acquisition Task Implementation
 *
 * FreeRTOS Concepts Demonstrated:
 *   1. Periodic task using vTaskDelay (runs every 500ms)
 *   2. xQueueSend — inter-task communication via queue
 *   3. Mutex to protect shared calibration data
 *   4. Task as a producer in the producer-consumer pipeline
 *
 * Pipeline Position: [SensorTask] → xSensorQueue → ProcessingTask
 *
 *   SensorTask (P5) ──┐
 *                     ▼
 *               xSensorQueue (depth 10)
 *                     │
 *                     ▼
 *           ProcessingTask (P4)
 */

#include "sensor_task.h"
#include <stdlib.h>
#include <math.h>

/* Watchdog counter declared in alert_task.h — increment each sensor cycle */
extern volatile uint32_t g_watchdogCounter;

/* =========================================================================
 * Global Objects (defined here, declared extern in sensor_task.h)
 * ========================================================================= */

/* FreeRTOS Queue: holds up to 10 SensorData_t items */
QueueHandle_t     xSensorQueue      = NULL;

/* FreeRTOS Mutex: protects g_calibration from concurrent access */
SemaphoreHandle_t xCalibrationMutex = NULL;

/* Shared calibration data — could be updated by a calibration task */
SensorCalibration_t g_calibration = {
    .temp_offset     = 0.5f,    /* +0.5°C correction */
    .pressure_offset = -2.0f,   /* -2 hPa correction */
    .humidity_offset = 1.0f,    /* +1% RH correction */
    .last_calibrated = 0
};

volatile uint32_t g_totalSensorReadings = 0;

/* =========================================================================
 * Pseudo-random sensor simulation
 * In a real system, this would be an ADC read or I2C sensor poll.
 * ========================================================================= */

/* Simple LCG pseudo-random number generator (deterministic, no stdlib rand race) */
static uint32_t s_seed = 42;

static float sim_rand_float(float min, float max) {
    s_seed = s_seed * 1664525UL + 1013904223UL; /* Numerical Recipes LCG */
    float t = (float)(s_seed & 0xFFFF) / 65535.0f;
    return min + t * (max - min);
}

/* Simulate a slow-moving temperature with sinusoidal drift */
static float simulate_temperature(uint32_t tick_ms) {
    float base  = 28.0f;                              /* 28°C baseline */
    float drift = 8.0f * sinf((float)tick_ms / 3000.0f); /* ±8°C slow sine wave */
    float noise = sim_rand_float(-0.5f, 0.5f);        /* ±0.5°C noise */
    return base + drift + noise;
}

static float simulate_pressure(uint32_t tick_ms) {
    float base  = 1013.25f;
    float drift = 50.0f * sinf((float)tick_ms / 5000.0f + 1.0f);
    float noise = sim_rand_float(-2.0f, 2.0f);
    return base + drift + noise;
}

static float simulate_humidity(uint32_t tick_ms) {
    float base  = 55.0f;
    float drift = 20.0f * sinf((float)tick_ms / 4000.0f + 2.0f);
    float noise = sim_rand_float(-1.0f, 1.0f);
    return base + drift + noise;
}

/* =========================================================================
 * vSensorTask — Highest-priority data acquisition task
 * ========================================================================= */

void vSensorTask(void* pvParameters) {
    (void)pvParameters;

    /*
     * FreeRTOS Concept: vTaskDelayUntil for drift-free periodic execution.
     * We record when we last woke up, then delay until (lastWake + period).
     * This ensures exactly 500ms between the START of each cycle, regardless
     * of how long the sensor read takes.
     */
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(500);  /* 500ms period */

    static uint32_t sequence = 0;

    sim_printf(ANSI_GREEN_BOLD
        "[SENSOR] Task started. Priority=%u. Reading every %u ms.\n"
        ANSI_RESET,
        TASK_PRIORITY_HIGH, (unsigned)xPeriod);

    while (g_schedulerRunning) {

        uint32_t now_ms = sim_get_ms();
        sequence++;

        /* ----------------------------------------------------------------
         * Step 1: Read calibration data under mutex protection.
         *
         * FreeRTOS Concept: xSemaphoreTake (mutex) — locks the calibration
         * table so no other task can modify it while we read.
         * portMAX_DELAY means we'll wait forever if another task holds it.
         *
         * Without this mutex, if a calibration update task writes to
         * g_calibration concurrently, we could read a MIX of old and new
         * values (a "torn read" data race).
         * ---------------------------------------------------------------- */
        xSemaphoreTake(xCalibrationMutex, portMAX_DELAY);
        SensorCalibration_t cal = g_calibration;  /* Local copy under lock */
        xSemaphoreGive(xCalibrationMutex);
        /* ---- END critical section — calibration data now safely in 'cal' */

        /* Step 2: Simulate ADC reads and apply calibration offsets */
        SensorData_t data;
        data.timestamp_ms = now_ms;
        data.sequence     = sequence;
        data.temperature  = simulate_temperature(now_ms) + cal.temp_offset;
        data.pressure     = simulate_pressure(now_ms)    + cal.pressure_offset;
        data.humidity     = simulate_humidity(now_ms)    + cal.humidity_offset;

        /* Clamp to realistic ranges */
        if (data.temperature < 0.0f)    data.temperature = 0.0f;
        if (data.temperature > 85.0f)   data.temperature = 85.0f;
        if (data.pressure < 850.0f)     data.pressure    = 850.0f;
        if (data.pressure > 1150.0f)    data.pressure    = 1150.0f;
        if (data.humidity < 0.0f)       data.humidity    = 0.0f;
        if (data.humidity > 100.0f)     data.humidity    = 100.0f;

        /* ----------------------------------------------------------------
         * Step 3: Send data to processing queue.
         *
         * FreeRTOS Concept: xQueueSend COPIES the SensorData_t struct INTO
         * the queue buffer. After this call, 'data' on the stack can be
         * overwritten — the queue holds its own copy. This is SAFE.
         *
         * Timeout = pdMS_TO_TICKS(10): If the queue is full, wait 10ms.
         * If still full, drop the reading (best-effort: sensor data is fresh,
         * old data is less valuable than new data).
         * ---------------------------------------------------------------- */
        /*
         * Increment the watchdog counter every cycle.
         * The alert task monitors this — if it stops incrementing, something
         * is wrong with the sensor task (starved or crashed).
         */
        InterlockedIncrement((LONG*)&g_watchdogCounter);

        BaseType_t sent = xQueueSend(xSensorQueue, &data, pdMS_TO_TICKS(10));

        if (sent == pdPASS) {
            InterlockedIncrement((LONG*)&g_totalSensorReadings);
            sim_printf(ANSI_GREEN
                "[SENSOR t=%5ums] #%03u | Temp: %5.1f°C | Press: %7.1f hPa | "
                "Hum: %4.1f%% | Queue depth: %u\n" ANSI_RESET,
                now_ms, sequence,
                data.temperature, data.pressure, data.humidity,
                (unsigned)uxQueueMessagesWaiting(xSensorQueue));
        } else {
            /* Queue full — processing task is behind, drop reading */
            sim_printf(ANSI_YELLOW
                "[SENSOR t=%5ums] #%03u | WARNING: Queue FULL — reading DROPPED "
                "(processing task lagging!)\n" ANSI_RESET,
                now_ms, sequence);
        }

        /* ----------------------------------------------------------------
         * Step 4: Wait until next period.
         *
         * FreeRTOS Concept: vTaskDelayUntil puts us in BLOCKED state.
         * The scheduler runs other tasks (ProcessingTask, LoggingTask, etc.)
         * while we sleep. We use NO CPU while blocked — this is key to RTOS
         * efficiency vs. a bare-metal superloop.
         * ---------------------------------------------------------------- */
        vTaskDelayUntil(&xLastWakeTime, xPeriod);
    }

    sim_printf(ANSI_GREEN "[SENSOR] Task exiting.\n" ANSI_RESET);
    vTaskDelete(NULL);
}
