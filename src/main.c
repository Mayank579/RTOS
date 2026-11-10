/*
 * main.c - FreeRTOS Multi-Task Simulator Entry Point
 *
 * This program demonstrates a complete FreeRTOS-style multi-task RTOS
 * application running on Windows via a Win32 simulator layer.
 *
 * =========================================================================
 * SYSTEM ARCHITECTURE
 * =========================================================================
 *
 *  Priority │ Task              │ Role
 *  ─────────┼───────────────────┼──────────────────────────────────────────
 *     P6    │ AlertTask         │ Reactive — wakes on anomaly semaphore
 *     P5    │ SensorTask        │ Periodic — reads sensors every 500ms
 *     P4    │ ProcessingTask    │ Pipeline — filters, detects anomalies
 *     P3    │ LoggingTask       │ Output   — formats and prints log entries
 *
 *  Shared Resources (protected by mutexes):
 *    xCalibrationMutex  → protects g_calibration (sensor → main)
 *    xStatsMutex        → protects g_processingStats (processing → main)
 *    xLoggingMutex      → serializes console output (logging → console)
 *
 *  Queues (inter-task communication):
 *    xSensorQueue    (depth 10) → SensorTask     → ProcessingTask
 *    xProcessedQueue (depth 10) → ProcessingTask → LoggingTask
 *
 *  Semaphores (event signaling):
 *    xAlertSemaphore (binary) → ProcessingTask → AlertTask
 *
 * =========================================================================
 * PROGRAM FLOW
 * =========================================================================
 *
 *  Phase 1 (0–10s):  Normal sensor pipeline — all 4 tasks running
 *  Phase 2 (10–15s): Priority inversion demo (BUG then FIX)
 *  Phase 3:          Statistics summary printout
 *
 * =========================================================================
 * FreeRTOS CONCEPTS COVERED
 * =========================================================================
 *   ✓ Task creation and priorities (xTaskCreate)
 *   ✓ Periodic tasks (vTaskDelayUntil)
 *   ✓ Queues for IPC (xQueueCreate, xQueueSend, xQueueReceive)
 *   ✓ Binary semaphore for signaling (xSemaphoreCreateBinary)
 *   ✓ Mutex for mutual exclusion (xSemaphoreCreateMutex)
 *   ✓ Priority inheritance (xSemaphoreCreateMutexWithPI)
 *   ✓ Preemptive scheduling (higher priority always runs first)
 *   ✓ Blocked state (task waits without consuming CPU)
 *   ✓ Producer-consumer pipeline (sensor → process → log)
 *   ✓ Watchdog monitoring (alert task watches sensor health)
 *   ✓ Priority inversion: bug + fix with detailed timing
 */

#include "freertos_sim.h"
#include "sensor_task.h"
#include "processing_task.h"
#include "logging_task.h"
#include "alert_task.h"
#include "priority_inversion_demo.h"

#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * Forward Declarations
 * ========================================================================= */
static void print_banner(void);
static void print_statistics(void);
static void phase1_sensor_pipeline(void);
static void phase2_pi_demo(void);

/* Task handles — stored so we can delete them after each phase */
static TaskHandle_t hSensorTask    = NULL;
static TaskHandle_t hProcessTask   = NULL;
static TaskHandle_t hLogTask       = NULL;
static TaskHandle_t hAlertTask     = NULL;

/* =========================================================================
 * main()
 * ========================================================================= */

int main(void) {
    /* ---- Initialize the simulator ---- */
    vSimulatorInit();
    vSimulatorStart();

    print_banner();
    Sleep(500);

    /* =========================================================================
     * Create all shared IPC objects BEFORE creating tasks.
     *
     * FreeRTOS Concept: Queue/semaphore/mutex handles are global — tasks
     * communicate through these handles. The main() function acts as the
     * "startup" code that creates all RTOS objects before the scheduler starts.
     *
     * In real FreeRTOS: main() calls xTaskCreate() then vTaskStartScheduler().
     * The scheduler takes over and never returns. Here we simulate that by
     * having main() wait while tasks run.
     * ========================================================================= */

    /* --- Queues --- */
    /*
     * FreeRTOS Concept: Queue depth = maximum items that can be queued.
     * If the producer is faster than the consumer, the queue fills up.
     * A depth of 10 gives ~5 seconds of buffering at 2 reads/sec.
     */
    xSensorQueue    = xQueueCreate(10, sizeof(SensorData_t));
    xProcessedQueue = xQueueCreate(10, sizeof(ProcessedData_t));

    /* --- Semaphores --- */
    /*
     * FreeRTOS Concept: Binary semaphore created in EMPTY state.
     * AlertTask will TAKE (wait on) this.
     * ProcessingTask will GIVE (signal) this when anomaly detected.
     */
    xAlertSemaphore = xSemaphoreCreateBinary();

    /* --- Mutexes --- */
    /*
     * FreeRTOS Concept: Mutexes start in AVAILABLE state (unlike binary sem).
     * First task to call xSemaphoreTake() gets it; others block until release.
     */
    xCalibrationMutex = xSemaphoreCreateMutex();
    xStatsMutex        = xSemaphoreCreateMutex();
    xLoggingMutex      = xSemaphoreCreateMutex();

    /* Validate all objects were created */
    if (!xSensorQueue || !xProcessedQueue || !xAlertSemaphore ||
        !xCalibrationMutex || !xStatsMutex || !xLoggingMutex) {
        fprintf(stderr, "FATAL: Failed to create RTOS objects!\n");
        return 1;
    }

    sim_printf(ANSI_BLUE
        "[MAIN] All RTOS objects created successfully.\n"
        "[MAIN]   Queues: xSensorQueue, xProcessedQueue\n"
        "[MAIN]   Semaphore: xAlertSemaphore (binary)\n"
        "[MAIN]   Mutexes: xCalibrationMutex, xStatsMutex, xLoggingMutex\n"
        ANSI_RESET);

    /* =========================================================================
     * PHASE 1: Sensor Pipeline (10 seconds)
     * ========================================================================= */
    phase1_sensor_pipeline();

    /* =========================================================================
     * PHASE 2: Priority Inversion Demo
     * ========================================================================= */
    phase2_pi_demo();

    /* =========================================================================
     * PHASE 3: Print Statistics Summary
     * ========================================================================= */
    print_statistics();

    /* Cleanup */
    vQueueDelete(xSensorQueue);
    vQueueDelete(xProcessedQueue);
    vSemaphoreDelete(xAlertSemaphore);
    vSemaphoreDelete(xCalibrationMutex);
    vSemaphoreDelete(xStatsMutex);
    vSemaphoreDelete(xLoggingMutex);

    sim_printf(ANSI_BLUE "\n[MAIN] Simulator stopped. Goodbye!\n" ANSI_RESET);
    return 0;
}

/* =========================================================================
 * Phase 1: Normal Sensor Pipeline
 * ========================================================================= */

static void phase1_sensor_pipeline(void) {
    sim_printf(ANSI_BLUE ANSI_BOLD
        "\n╔═══════════════════════════════════════════════════════╗\n"
        "║  PHASE 1: Sensor Pipeline (10 seconds)                ║\n"
        "║  SensorTask(P5) → ProcessingTask(P4) → LoggingTask(P3)║\n"
        "║  AlertTask(P6) reacts to anomalies via semaphore      ║\n"
        "╚═══════════════════════════════════════════════════════╝\n"
        ANSI_RESET "\n");

    /* ---- Create the four pipeline tasks ---- */

    /*
     * FreeRTOS Concept: xTaskCreate signature:
     *   xTaskCreate(func, name, stackSize, params, priority, handle)
     *
     * Stack size (4096 words here) is ignored by our simulator but
     * in real FreeRTOS it's the task's private stack size in words.
     * Too small → stack overflow (detected by configCHECK_FOR_STACK_OVERFLOW).
     */

    /* AlertTask: HIGHEST priority — must respond instantly to anomalies */
    xTaskCreate(vAlertTask,      "AlertTask",   4096, NULL, TASK_PRIORITY_ALERT,        &hAlertTask);

    /* SensorTask: Second highest — data must be acquired reliably */
    xTaskCreate(vSensorTask,     "SensorTask",  4096, NULL, TASK_PRIORITY_HIGH,         &hSensorTask);

    /* ProcessingTask: Processes data as fast as sensor produces it */
    xTaskCreate(vProcessingTask, "ProcessTask", 4096, NULL, TASK_PRIORITY_ABOVE_NORMAL, &hProcessTask);

    /* LoggingTask: Lowest priority — logging is best-effort, can be delayed */
    xTaskCreate(vLoggingTask,    "LogTask",     4096, NULL, TASK_PRIORITY_NORMAL,       &hLogTask);

    sim_printf(ANSI_BLUE
        "[MAIN] All tasks created. Pipeline running for 10 seconds...\n"
        ANSI_RESET);

    /* Let the pipeline run for 10 seconds */
    Sleep(10000);

    /* Stop the pipeline cleanly */
    g_schedulerRunning = false;

    /* Wake any tasks waiting on semaphores or queues so they exit their loops */
    if (xAlertSemaphore) xSemaphoreGive(xAlertSemaphore);
    if (xSensorQueue)    xQueueSend(xSensorQueue, &(SensorData_t){0}, 0);
    if (xProcessedQueue) xQueueSend(xProcessedQueue, &(ProcessedData_t){0}, 0);

    /* Wait gracefully for tasks to exit without TerminateThread */
    HANDLE hThreads[4];
    int numThreads = 0;
    if (hAlertTask && hAlertTask->thread)   hThreads[numThreads++] = hAlertTask->thread;
    if (hSensorTask && hSensorTask->thread)  hThreads[numThreads++] = hSensorTask->thread;
    if (hProcessTask && hProcessTask->thread) hThreads[numThreads++] = hProcessTask->thread;
    if (hLogTask && hLogTask->thread)     hThreads[numThreads++] = hLogTask->thread;

    if (numThreads > 0) {
        WaitForMultipleObjects(numThreads, hThreads, TRUE, 2000);
    }

    /* Clean up task control blocks */
    if (hAlertTask)   { CloseHandle(hAlertTask->thread);   free(hAlertTask);   hAlertTask = NULL; }
    if (hSensorTask)  { CloseHandle(hSensorTask->thread);  free(hSensorTask);  hSensorTask = NULL; }
    if (hProcessTask) { CloseHandle(hProcessTask->thread); free(hProcessTask); hProcessTask = NULL; }
    if (hLogTask)     { CloseHandle(hLogTask->thread);     free(hLogTask);     hLogTask = NULL; }

    sim_printf(ANSI_BLUE
        "\n[MAIN] Phase 1 complete. All pipeline tasks stopped.\n"
        ANSI_RESET);
}

/* =========================================================================
 * Phase 2: Priority Inversion Demo
 * ========================================================================= */

static void phase2_pi_demo(void) {
    /* Restart scheduler flag for demo tasks */
    g_schedulerRunning = true;

    sim_printf(ANSI_BLUE ANSI_BOLD
        "\n╔══════════════════════════════════════════════════════╗\n"
        "║  PHASE 2: Priority Inversion Demo                    ║\n"
        "╚══════════════════════════════════════════════════════╝\n"
        ANSI_RESET "\n");

    run_priority_inversion_demo();

    g_schedulerRunning = false;
}

/* =========================================================================
 * Statistics Summary
 * ========================================================================= */

static void print_statistics(void) {
    /* Read stats under mutex protection */
    uint32_t total_processed   = 0;
    uint32_t anomalies         = 0;
    float    max_temp          = 0.0f;
    float    min_press         = 0.0f;
    float    max_press         = 0.0f;

    if (xStatsMutex) {
        xSemaphoreTake(xStatsMutex, portMAX_DELAY);
        total_processed = g_processingStats.total_processed;
        anomalies       = g_processingStats.anomalies_detected;
        max_temp        = g_processingStats.max_temp_seen;
        min_press       = g_processingStats.min_pressure_seen;
        max_press       = g_processingStats.max_pressure_seen;
        xSemaphoreGive(xStatsMutex);
    }

    uint32_t queue_hwm = xSensorQueue ? uxQueueGetHighWaterMark(xSensorQueue) : 0;

    printf(ANSI_WHITE_BOLD
        "\n╔══════════════════════════════════════════════════════════╗\n"
        "║              RTOS SCHEDULER STATISTICS                   ║\n"
        "╠══════════════════════════════════════════════════════════╣\n"
        "║  Total sensor readings:         %-6u                     ║\n"
        "║  Total processed readings:      %-6u                     ║\n"
        "║  Anomalies detected:            %-6u                     ║\n"
        "║  Alerts fired:                  %-6u                     ║\n"
        "║  Log entries written:           %-6u                     ║\n"
        "║  Sensor queue high-water mark:  %-6u                     ║\n"
        "║  Total mutex contentions:       %-6u                     ║\n"
        "╠══════════════════════════════════════════════════════════╣\n"
        "║  Peak temperature seen:         %-5.1f °C                ║\n"
        "║  Min pressure seen:             %-7.1f hPa               ║\n"
        "║  Max pressure seen:             %-7.1f hPa               ║\n"
        "╠══════════════════════════════════════════════════════════╣\n"
        "║  RTOS Concepts Demonstrated:                             ║\n"
        "║    ✓ Preemptive task scheduling (4 priority levels)      ║\n"
        "║    ✓ Inter-task queues (SensorQ + ProcessedQ)            ║\n"
        "║    ✓ Binary semaphore signaling (alert notification)     ║\n"
        "║    ✓ Mutexes with ownership (3 mutexes used)             ║\n"
        "║    ✓ Priority inheritance (PI mutex demo)                ║\n"
        "║    ✓ Producer-consumer pipeline (3-stage)                ║\n"
        "║    ✓ Moving average filter (stateful task)               ║\n"
        "║    ✓ Watchdog monitoring (alert task watches sensor)     ║\n"
        "║    ✓ Priority inversion: bug + fix with timing           ║\n"
        "╚══════════════════════════════════════════════════════════╝\n"
        ANSI_RESET "\n",
        g_totalSensorReadings,
        total_processed,
        anomalies,
        g_totalAlertsFired,
        g_totalLogEntries,
        queue_hwm,
        g_totalMutexContentions,
        max_temp, min_press, max_press);
    fflush(stdout);
}

/* =========================================================================
 * Banner
 * ========================================================================= */

static void print_banner(void) {
    printf(ANSI_CYAN_BOLD
        "\n"
        "  ██████╗ ████████╗ ██████╗ ███████╗    ██████╗ ███████╗███╗   ███╗ ██████╗ \n"
        "  ██╔══██╗╚══██╔══╝██╔═══██╗██╔════╝    ██╔══██╗██╔════╝████╗ ████║██╔═══██╗\n"
        "  ██████╔╝   ██║   ██║   ██║███████╗    ██║  ██║█████╗  ██╔████╔██║██║   ██║\n"
        "  ██╔══██╗   ██║   ██║   ██║╚════██║    ██║  ██║██╔══╝  ██║╚██╔╝██║██║   ██║\n"
        "  ██║  ██║   ██║   ╚██████╔╝███████║    ██████╔╝███████╗██║ ╚═╝ ██║╚██████╔╝\n"
        "  ╚═╝  ╚═╝   ╚═╝    ╚═════╝ ╚══════╝    ╚═════╝ ╚══════╝╚═╝     ╚═╝ ╚═════╝ \n"
        "\n"
        ANSI_RESET);

    printf(ANSI_WHITE_BOLD
        "  FreeRTOS Multi-Task Simulator — Win32 Backend\n"
        "  Demonstrating: Tasks, Queues, Semaphores, Mutexes, Priority Inversion\n"
        "  Build: MinGW gcc -std=c11 -O2\n"
        "\n" ANSI_RESET);

    printf(ANSI_BLUE
        "  Task Priority Map:\n"
        "    P7 (Demo HIGH)     — time-critical demo task\n"
        "    P6 AlertTask       — highest normal priority, reactive\n"
        "    P5 SensorTask      — periodic data acquisition\n"
        "    P4 ProcessingTask  — signal processing + anomaly detection\n"
        "    P3 LoggingTask     — best-effort output (can be preempted)\n"
        "    P1 (Demo LOW)      — low-priority mutex holder\n"
        "\n" ANSI_RESET);
    fflush(stdout);
}
