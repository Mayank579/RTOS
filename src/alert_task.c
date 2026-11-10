/*
 * alert_task.c - High-Priority Alert Handler Task
 *
 * FreeRTOS Concepts Demonstrated:
 *   1. High-priority REACTIVE task — always preempts lower-priority tasks
 *      the moment it has work (semaphore is Given).
 *   2. Binary semaphore as notification mechanism (ISR-to-task pattern).
 *   3. Watchdog monitoring via shared volatile counter + mutex.
 *   4. ASCII animation — demonstrates that high-priority tasks get CPU
 *      immediately, even interrupting the logging or processing tasks.
 *
 * The alert task has Priority 6, which means:
 *   - As soon as xAlertSemaphore is Given by the processing task,
 *     the RTOS preempts ProcessingTask(P4) and runs AlertTask(P6).
 *   - AlertTask will NOT be preempted by SensorTask(P5), ProcessingTask(P4),
 *     or LoggingTask(P3) while it's running.
 *   - Only a task with Priority 7 could preempt it.
 */

#include "alert_task.h"
#include <string.h>

/* =========================================================================
 * Global Objects
 * ========================================================================= */

volatile uint32_t g_totalAlertsFired = 0;
volatile uint32_t g_watchdogCounter  = 0;  /* Sensor task increments this */

/* =========================================================================
 * Alert Animation
 * ========================================================================= */

static const char* ALERT_FRAMES[] = {
    "  [!!!]  ALERT  [!!!]  ",
    "  (!!!)  ALERT  (!!!)  ",
    "  {!!!}  ALERT  {!!!}  ",
    "  <!!!)  ALERT  (!!!>  ",
};
#define NUM_ALERT_FRAMES 4

static void play_alert_animation(uint32_t timestamp_ms, uint32_t alertNum) {
    /*
     * This animation demonstrates that the high-priority task "owns" the CPU
     * during this window. No lower-priority task will execute these frames.
     * (In this simulation, Windows threads aren't perfectly preemptive, but
     *  the concept is the same — higher priority = CPU first.)
     */
    printf(ANSI_RED_BOLD "\n");
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║         SENSOR ANOMALY DETECTED!         ║\n");
    printf("  ║   Alert #%-3u  |  Time: %5u ms           ║\n", alertNum, timestamp_ms);
    printf("  ╚══════════════════════════════════════════╝\n");

    for (int frame = 0; frame < 3; frame++) {
        const char* f = ALERT_FRAMES[frame % NUM_ALERT_FRAMES];
        printf("  ★★★ %s ★★★\n", f);
        fflush(stdout);
        Sleep(80); /* Short sleep between animation frames */
    }
    printf(ANSI_RESET "\n");
    fflush(stdout);
}

/* =========================================================================
 * vAlertTask
 * ========================================================================= */

void vAlertTask(void* pvParameters) {
    (void)pvParameters;

    sim_printf(ANSI_RED_BOLD
        "[ALERT ] Task started. Priority=%u. Waiting on xAlertSemaphore...\n"
        ANSI_RESET, TASK_PRIORITY_ALERT);

    uint32_t lastWatchdogValue = 0;
    TickType_t lastWatchdogCheck = xTaskGetTickCount();

    while (g_schedulerRunning) {

        /* ----------------------------------------------------------------
         * Block on binary semaphore — wait up to 500ms.
         *
         * FreeRTOS Concept: xSemaphoreTake puts us in BLOCKED state.
         * We consume ZERO CPU while waiting. The processing task will
         * call xSemaphoreGive when it detects an anomaly.
         *
         * When Given:
         *   1. AlertTask moves from BLOCKED → READY
         *   2. Since AlertTask(P6) > ProcessingTask(P4), RTOS IMMEDIATELY
         *      context-switches: ProcessingTask is preempted mid-execution!
         *   3. AlertTask runs the animation, then blocks again.
         *   4. ProcessingTask resumes where it left off.
         *
         * This is the REAL POWER of an RTOS: priority-driven preemption
         * ensures critical events are handled with bounded latency.
         * ---------------------------------------------------------------- */
        BaseType_t taken = xSemaphoreTake(xAlertSemaphore, pdMS_TO_TICKS(500));

        if (taken == pdPASS) {
            /* Semaphore was Given — anomaly detected! */
            uint32_t now_ms = sim_get_ms();
            uint32_t alertNum = InterlockedIncrement((LONG*)&g_totalAlertsFired);

            /* Take the global print mutex so our animation isn't garbled */
            EnterCriticalSection(&g_printCS);
            play_alert_animation(now_ms, alertNum);
            LeaveCriticalSection(&g_printCS);

            sim_printf(ANSI_RED
                "[ALERT  t=%5ums] Alert #%u processed. "
                "Task preempted processing task to handle this immediately.\n"
                ANSI_RESET,
                now_ms, alertNum);
        }

        /* ----------------------------------------------------------------
         * Watchdog check — runs even when no alert (uses timeout window).
         *
         * FreeRTOS Concept: Watchdog monitoring. The sensor task should be
         * incrementing g_watchdogCounter every 500ms. If it stops (task
         * crashed or starved), the counter stops changing.
         *
         * This is a simplified software watchdog. Real FreeRTOS systems use
         * the hardware watchdog timer (WDT) for this.
         * ---------------------------------------------------------------- */
        TickType_t now = xTaskGetTickCount();
        if (now - lastWatchdogCheck >= 1000) {
            uint32_t currentWatchdog = g_watchdogCounter;
            if (currentWatchdog == lastWatchdogValue && g_schedulerRunning) {
                sim_printf(ANSI_YELLOW
                    "[ALERT  WATCHDOG] WARNING: g_watchdogCounter has not "
                    "changed in 1000ms! SensorTask may be STARVED or CRASHED.\n"
                    ANSI_RESET);
            }
            lastWatchdogValue = currentWatchdog;
            lastWatchdogCheck = now;
        }
    }

    sim_printf(ANSI_RED "[ALERT ] Task exiting.\n" ANSI_RESET);
    vTaskDelete(NULL);
}
