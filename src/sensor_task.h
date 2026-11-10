/*
 * sensor_task.h - Sensor Task Interface
 *
 * FreeRTOS Concept: This is the highest-priority data-acquisition task.
 * In embedded systems, sensor tasks are often triggered by hardware interrupts
 * or run periodically. Here we simulate periodic sensor reads every 500ms.
 *
 * Priority: TASK_PRIORITY_HIGH (5) — must run reliably, can't be starved
 * Pattern: Periodic producer → posts to xSensorQueue
 */

#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include "freertos_sim.h"
#include <stdint.h>

/* =========================================================================
 * Shared Data Structures
 * These are COPIED into queues — no shared pointers needed.
 * ========================================================================= */

/*
 * SensorData_t — Raw sensor reading packet.
 * Sized to fit in a queue message; timestamp allows receivers to detect lag.
 */
typedef struct {
    uint32_t timestamp_ms;    /* When the reading was taken */
    float    temperature;     /* Celsius: simulated 20–40°C */
    float    pressure;        /* hPa:     simulated 900–1100 */
    float    humidity;        /* %RH:     simulated 30–80% */
    uint32_t sequence;        /* Monotonically increasing packet ID */
} SensorData_t;

/*
 * SensorCalibration_t — Shared calibration table.
 *
 * FreeRTOS Concept: This struct is SHARED MEMORY — multiple tasks could
 * read it. We protect it with a mutex (xCalibrationMutex) to prevent
 * torn reads/writes (a race condition where you read half-updated data).
 */
typedef struct {
    float temp_offset;        /* Calibration offset for temperature */
    float pressure_offset;    /* Calibration offset for pressure */
    float humidity_offset;    /* Calibration offset for humidity */
    uint32_t last_calibrated; /* Timestamp of last calibration */
} SensorCalibration_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/* Global queue handle — defined in main.c, used by sensor + processing tasks */
extern QueueHandle_t    xSensorQueue;

/* Global calibration mutex — shared between sensor_task and other potential readers */
extern SemaphoreHandle_t xCalibrationMutex;

/* Global calibration data — protected by xCalibrationMutex */
extern SensorCalibration_t g_calibration;

/* Global sensor statistics */
extern volatile uint32_t g_totalSensorReadings;

/*
 * vSensorTask — The sensor acquisition task function.
 * Pass NULL as pvParameters.
 */
void vSensorTask(void* pvParameters);

#endif /* SENSOR_TASK_H */
