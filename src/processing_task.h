/*
 * processing_task.h - Data Processing Task Interface
 *
 * FreeRTOS Concept: This task is a CONSUMER of xSensorQueue and a PRODUCER
 * of xProcessedQueue. It demonstrates:
 *   - Queue-based pipeline architecture
 *   - Moving average filter using a circular buffer (stateful task)
 *   - Anomaly detection → semaphore signaling to alert task
 *   - Mutex-protected shared statistics struct
 *
 * Priority: TASK_PRIORITY_ABOVE_NORMAL (4)
 * Pattern:  xSensorQueue → [ProcessingTask] → xProcessedQueue
 *                                           → xAlertSemaphore (on anomaly)
 */

#ifndef PROCESSING_TASK_H
#define PROCESSING_TASK_H

#include "freertos_sim.h"
#include "sensor_task.h"

/* =========================================================================
 * Processed Data Structure
 * ========================================================================= */

#define FILTER_SIZE 5   /* Moving average window size */

typedef struct {
    uint32_t  timestamp_ms;
    uint32_t  sequence;
    /* Filtered (smoothed) values */
    float     temp_filtered;
    float     pressure_filtered;
    float     humidity_filtered;
    /* Raw values for comparison */
    float     temp_raw;
    float     pressure_raw;
    /* Anomaly flags */
    bool      anomaly_detected;
    bool      temp_high;      /* temp > ANOMALY_TEMP_HIGH */
    bool      pressure_low;   /* pressure < ANOMALY_PRESSURE_LOW */
    bool      pressure_high;  /* pressure > ANOMALY_PRESSURE_HIGH */
} ProcessedData_t;

/* =========================================================================
 * Anomaly Thresholds
 * ========================================================================= */
#define ANOMALY_TEMP_HIGH        35.0f   /* °C — above this is an alert */
#define ANOMALY_PRESSURE_LOW    950.0f   /* hPa — below this is alert */
#define ANOMALY_PRESSURE_HIGH  1080.0f   /* hPa — above this is alert */

/* =========================================================================
 * Shared Statistics (protected by mutex)
 * ========================================================================= */
typedef struct {
    uint32_t total_processed;
    uint32_t anomalies_detected;
    float    max_temp_seen;
    float    min_pressure_seen;
    float    max_pressure_seen;
} ProcessingStats_t;

/* =========================================================================
 * Public API
 * ========================================================================= */
extern QueueHandle_t     xProcessedQueue;
extern SemaphoreHandle_t xAlertSemaphore;
extern SemaphoreHandle_t xStatsMutex;
extern ProcessingStats_t g_processingStats;
extern volatile uint32_t g_totalProcessed;

void vProcessingTask(void* pvParameters);

#endif /* PROCESSING_TASK_H */
