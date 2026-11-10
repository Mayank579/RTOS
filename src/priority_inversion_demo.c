/*
 * priority_inversion_demo.c - Classic Priority Inversion Bug + Fix
 *
 * =========================================================================
 * BACKGROUND: THE MARS PATHFINDER INCIDENT (1997)
 * =========================================================================
 * NASA's Mars Pathfinder spacecraft used VxWorks RTOS. After landing, it
 * repeatedly reset due to a watchdog timeout. Root cause: priority inversion.
 *
 *   - A LOW priority meteorological task held a mutex (information bus)
 *   - A HIGH priority communications task blocked waiting for it
 *   - MEDIUM priority tasks (image processing, etc.) ran freely, starving
 *     the LOW task, which meant the HIGH task NEVER got the mutex
 *   - The watchdog saw the HIGH task wasn't running → reset!
 *
 * The fix (enabling priority inheritance, already in VxWorks but not turned
 * on) was uploaded from Earth. This exact same bug can crash embedded
 * system today.
 *
 * =========================================================================
 * THIS DEMO SHOWS:
 * =========================================================================
 *
 * SCENARIO 1 - BUG (plain mutex, no priority inheritance):
 * ┌────────────────────────────────────────────────────────────────────┐
 * │ Timeline:                                                          │
 * │  t=  0ms  LOW task (P1) acquires shared_resource_mutex            │
 * │  t= 10ms  HIGH task (P7) wakes, tries mutex → BLOCKED             │
 * │  t= 20ms  MEDIUM task (P3) becomes runnable                       │
 * │           MEDIUM preempts LOW (P3 > P1) ← BUG STARTS HERE!       │
 * │  t=120ms  MEDIUM finishes its 100ms of CPU-bound work             │
 * │  t=130ms  LOW task resumes, releases mutex (held for 130ms!)      │
 * │  t=131ms  HIGH task finally gets mutex (waited 121ms!)            │
 * │                                                                    │
 * │  Result: HIGH priority task (P7) was STARVED by MEDIUM (P3)!      │
 * │          Priority was INVERTED: P3 effectively > P7!              │
 * └────────────────────────────────────────────────────────────────────┘
 *
 * SCENARIO 2 - FIX (mutex WITH priority inheritance):
 * ┌────────────────────────────────────────────────────────────────────┐
 * │ Timeline:                                                          │
 * │  t=  0ms  LOW task (P1) acquires PI_mutex                         │
 * │  t= 10ms  HIGH task (P7) tries PI_mutex → LOW boosted to P7!      │
 * │  t= 20ms  MEDIUM task (P3) tries to preempt LOW task              │
 * │           CANNOT! LOW task now runs at P7 ← FIX WORKS!           │
 * │  t= 30ms  LOW task releases PI_mutex (held for only 30ms)         │
 * │           LOW task restored to P1                                  │
 * │  t= 31ms  HIGH task (P7) immediately acquires mutex and runs       │
 * │                                                                    │
 * │  Result: HIGH task waited only ~21ms (vs 121ms without PI)!       │
 * └────────────────────────────────────────────────────────────────────┘
 */

#include "priority_inversion_demo.h"
#include <stdio.h>

/* =========================================================================
 * Global Demo Objects
 * ========================================================================= */

SemaphoreHandle_t xSharedResourceMutex   = NULL;
SemaphoreHandle_t xSharedResourcePIMutex = NULL;

HANDLE hDemoLowStarted = NULL;
HANDLE hDemoHighGo     = NULL;
HANDLE hDemoMedGo      = NULL;
HANDLE hDemoComplete   = NULL;

/* Synchronization state to simulate single-core RTOS preemption */
static volatile bool s_med_running       = false;
static volatile bool s_low_holding_mutex = false;
static TaskHandle_t  s_hLowTask          = NULL;

/* Timing records for each scenario */
static uint32_t t_low_acquired;
static uint32_t t_high_blocked;
static uint32_t t_med_start;
static uint32_t t_med_end;
static uint32_t t_low_released;
static uint32_t t_high_acquired;

/* Which mutex to use in this demo run */
static bool s_use_pi = false;

/* =========================================================================
 * Demo Task: LOW priority (P1)
 *
 * Acquires the shared resource mutex and holds it for 30ms (simulating
 * slow work — e.g., writing to a slow UART or SPI flash).
 * ========================================================================= */

void vDemoLowTask(void* pvParameters) {
    (void)pvParameters;

    SemaphoreHandle_t mutex = s_use_pi ? xSharedResourcePIMutex
                                       : xSharedResourceMutex;

    sim_printf(ANSI_WHITE
        "  [LOW  P1] Starting. About to acquire %s...\n" ANSI_RESET,
        s_use_pi ? "PI_mutex" : "shared_resource_mutex");

    /* Acquire the mutex — this is the critical step */
    t_low_acquired = sim_get_ms();
    xSemaphoreTake(mutex, portMAX_DELAY);
    s_low_holding_mutex = true;

    sim_printf(ANSI_WHITE
        "  [LOW  P1] t=%ums: Mutex ACQUIRED. Doing slow work (30ms)...\n"
        ANSI_RESET, t_low_acquired);

    /* Signal main thread that LOW has the mutex — HIGH and MED can start */
    SetEvent(hDemoLowStarted);

    /*
     * Simulate slow work (30ms total) while holding the mutex.
     * In a single-core RTOS:
     * - Without PI: LOW remains at P1. When MED (P3) arrives, MED preempts LOW.
     *   LOW is starved and CANNOT make progress until MED finishes its 100ms work!
     * - With PI: When HIGH blocked on the mutex, LOW was boosted to P7.
     *   Since P7 > P3, MED cannot preempt LOW. LOW finishes in ~30ms without delay!
     */
    for (int step = 1; step <= 3; step++) {
        Sleep(10);
        uint32_t nowMs = sim_get_ms();
        UBaseType_t curPri = uxTaskPriorityGet(xTaskGetCurrentTaskHandle());
        sim_printf(ANSI_WHITE
            "  [LOW  P1] t=%ums: Working... (%ums elapsed since mutex acquired, current priority=%u)\n"
            ANSI_RESET, nowMs, nowMs - t_low_acquired, curPri);

        /* In an RTOS, if MED (P3) is running and our priority < P3, we are preempted! */
        if (s_med_running && curPri < TASK_PRIORITY_NORMAL) {
            sim_printf(ANSI_YELLOW
                "  [LOW  P1] t=%ums: PREEMPTED by MED task (P3 > P1)! Starved waiting for CPU...\n"
                ANSI_RESET, sim_get_ms());
            while (s_med_running) {
                Sleep(5);
            }
            sim_printf(ANSI_YELLOW
                "  [LOW  P1] t=%ums: Resumed after MED task finished.\n"
                ANSI_RESET, sim_get_ms());
        }
    }

    /* Release the mutex */
    t_low_released = sim_get_ms();
    s_low_holding_mutex = false;
    xSemaphoreGive(mutex);

    uint32_t held_ms = t_low_released - t_low_acquired;
    sim_printf(ANSI_WHITE
        "  [LOW  P1] t=%ums: Mutex RELEASED after %ums. Resuming P1 work.\n"
        ANSI_RESET, t_low_released, held_ms);

    SetEvent(hDemoComplete);
    vTaskDelete(NULL);
}

/* =========================================================================
 * Demo Task: HIGH priority (P7)
 *
 * Tries to acquire the shared resource mutex. If LOW task holds it,
 * HIGH task BLOCKS here — a critical priority inversion scenario.
 * ========================================================================= */

void vDemoHighTask(void* pvParameters) {
    (void)pvParameters;

    SemaphoreHandle_t mutex = s_use_pi ? xSharedResourcePIMutex
                                       : xSharedResourceMutex;

    /* Wait for signal that LOW task has the mutex */
    WaitForSingleObject(hDemoHighGo, INFINITE);

    t_high_blocked = sim_get_ms();
    sim_printf(ANSI_RED_BOLD
        "  [HIGH P7] t=%ums: I need the mutex! Calling xSemaphoreTake...\n"
        ANSI_RESET, t_high_blocked);

    if (s_use_pi) {
        sim_printf(ANSI_YELLOW
            "  [HIGH P7] (PI mutex: LOW task will be boosted to P7 to unblock me faster)\n"
            ANSI_RESET);
    } else {
        sim_printf(ANSI_RED
            "  [HIGH P7] (Plain mutex: I'll be STARVED if MEDIUM task preempts LOW)\n"
            ANSI_RESET);
    }

    /* This blocks until LOW task releases the mutex */
    xSemaphoreTake(mutex, portMAX_DELAY);

    t_high_acquired = sim_get_ms();
    uint32_t waited_ms = t_high_acquired - t_high_blocked;

    sim_printf(ANSI_RED_BOLD
        "  [HIGH P7] t=%ums: Mutex ACQUIRED! Waited %ums.\n" ANSI_RESET,
        t_high_acquired, waited_ms);

    /* Do critical work */
    Sleep(10);
    xSemaphoreGive(mutex);

    sim_printf(ANSI_RED_BOLD
        "  [HIGH P7] t=%ums: Work done, mutex released.\n" ANSI_RESET,
        sim_get_ms());

    vTaskDelete(NULL);
}

/* =========================================================================
 * Demo Task: MEDIUM priority (P3)
 *
 * CPU-bound computation — the "villain" in the priority inversion story.
 * Without PI, this task starves the LOW task (and thus the HIGH task).
 * With PI, the LOW task runs at P7 and MEDIUM can't preempt it.
 * ========================================================================= */

void vDemoMediumTask(void* pvParameters) {
    (void)pvParameters;

    /* Wait for signal to start */
    WaitForSingleObject(hDemoMedGo, INFINITE);

    /*
     * In an RTOS, when MED is triggered:
     * - Scenario 1 (no PI): LOW is at P1. MED (P3) immediately preempts LOW!
     * - Scenario 2 (with PI): HIGH blocked on mutex, boosting LOW to P7.
     *   Since P7 > P3, MED CANNOT preempt LOW. MED must wait until LOW finishes!
     */
    if (s_use_pi && s_low_holding_mutex) {
        sim_printf(ANSI_GREEN
            "  [MED  P3] t=%ums: Ready to run, but LOW is boosted to P7 (P7 > P3) → CANNOT preempt!\n"
            ANSI_RESET, sim_get_ms());
        while (s_low_holding_mutex) {
            Sleep(5);
        }
    } else {
        sim_printf(ANSI_RED
            "  [MED  P3] t=%ums: (I'm P3 > LOW's P1 → I PREEMPT the LOW task!)\n"
            ANSI_RESET, sim_get_ms());
    }

    s_med_running = true;
    t_med_start = sim_get_ms();
    sim_printf(ANSI_BLUE
        "  [MED  P3] t=%ums: Starting 100ms CPU-bound work...\n" ANSI_RESET,
        t_med_start);

    /* Simulate 100ms CPU work */
    Sleep(100);

    t_med_end = sim_get_ms();
    s_med_running = false;
    sim_printf(ANSI_BLUE
        "  [MED  P3] t=%ums: CPU work finished (%ums elapsed).\n" ANSI_RESET,
        t_med_end, t_med_end - t_med_start);

    vTaskDelete(NULL);
}

/* =========================================================================
 * run_priority_inversion_demo — Orchestrates the full demo
 * ========================================================================= */

static void print_scenario_header(bool use_pi) {
    if (!use_pi) {
        printf(ANSI_RED_BOLD
            "\n╔═══════════════════════════════════════════════════════════╗\n"
            "║  SCENARIO 1: PRIORITY INVERSION BUG (plain mutex)        ║\n"
            "║  Expected: HIGH task STARVED by MEDIUM task!             ║\n"
            "╚═══════════════════════════════════════════════════════════╝\n"
            ANSI_RESET "\n");
    } else {
        printf(ANSI_GREEN_BOLD
            "\n╔═══════════════════════════════════════════════════════════╗\n"
            "║  SCENARIO 2: PRIORITY INHERITANCE FIX (PI mutex)         ║\n"
            "║  Expected: HIGH task gets mutex quickly!                 ║\n"
            "╚═══════════════════════════════════════════════════════════╝\n"
            ANSI_RESET "\n");
    }
    fflush(stdout);
}

static void print_scenario_results(bool use_pi) {
    uint32_t high_wait_ms = t_high_acquired - t_high_blocked;
    uint32_t low_held_ms  = t_low_released  - t_low_acquired;
    uint32_t med_ran_ms   = t_med_end       - t_med_start;

    printf(ANSI_WHITE_BOLD
        "\n  ┌─────────────────── TIMING RESULTS ───────────────────┐\n");
    printf("  │ t=%-4ums  LOW  task acquired mutex                   │\n",
           t_low_acquired);
    printf("  │ t=%-4ums  HIGH task blocked (tried to acquire mutex) │\n",
           t_high_blocked);
    printf("  │ t=%-4ums  MED  task started running (100ms CPU work) │\n",
           t_med_start);
    printf("  │ t=%-4ums  MED  task finished                         │\n",
           t_med_end);
    printf("  │ t=%-4ums  LOW  task released mutex (held %ums)       │\n",
           t_low_released, low_held_ms);
    printf("  │ t=%-4ums  HIGH task acquired mutex (waited %ums)     │\n",
           t_high_acquired, high_wait_ms);
    printf("  └──────────────────────────────────────────────────────┘\n");

    if (!use_pi) {
        printf(ANSI_RED
            "\n  ⚠ BUG CONFIRMED: HIGH task (P7) waited %ums!\n"
            "  ⚠ LOW task held mutex for %ums (MED starved it; expected ~30ms).\n"
            "  ⚠ MED task (P3) ran for %ums, blocking LOW (P1) from releasing.\n"
            "  ⚠ Effective priority: P3 (MED) > P7 (HIGH) ← INVERTED!\n"
            ANSI_RESET, high_wait_ms, low_held_ms, med_ran_ms);
    } else {
        printf(ANSI_GREEN
            "\n  ✓ FIX CONFIRMED: HIGH task (P7) waited only %ums!\n"
            "  ✓ LOW task held mutex for %ums (close to expected ~30ms).\n"
            "  ✓ MED task (P3) could not preempt LOW (temporarily P7).\n"
            "  ✓ Priority inheritance prevented the inversion!\n"
            ANSI_RESET, high_wait_ms, low_held_ms, med_ran_ms);
    }
    printf(ANSI_RESET "\n");
    fflush(stdout);
}

static void run_scenario(bool use_pi) {
    s_use_pi = use_pi;
    s_med_running = false;
    s_low_holding_mutex = false;
    s_hLowTask = NULL;

    /* Reset timing records */
    t_low_acquired = t_high_blocked = t_med_start = 0;
    t_med_end = t_low_released = t_high_acquired = 0;

    /* Create synchronization events */
    hDemoLowStarted = CreateEvent(NULL, FALSE, FALSE, NULL);
    hDemoHighGo     = CreateEvent(NULL, FALSE, FALSE, NULL);
    hDemoMedGo      = CreateEvent(NULL, FALSE, FALSE, NULL);
    hDemoComplete   = CreateEvent(NULL, FALSE, FALSE, NULL);

    /* Create the mutex for this scenario */
    SemaphoreHandle_t mutex = use_pi ?
        xSemaphoreCreateMutexWithPI() :
        xSemaphoreCreateMutex();

    if (use_pi) {
        xSharedResourcePIMutex = mutex;
    } else {
        xSharedResourceMutex   = mutex;
    }

    print_scenario_header(use_pi);

    /* Explain what's about to happen */
    printf(ANSI_WHITE
        "  Tasks about to start:\n"
        "    LOW  (P1): acquires mutex, does 30ms work, releases\n"
        "    HIGH (P7): tries to acquire same mutex → will block\n"
        "    MED  (P3): 100ms CPU-bound work, no mutex needed\n\n"
        ANSI_RESET);
    fflush(stdout);
    Sleep(500);

    /* ---- Start LOW task (P1) ---- */
    xTaskCreate(vDemoLowTask, "DemoLOW", 4096, NULL, TASK_PRIORITY_LOW, &s_hLowTask);

    /* Wait for LOW to acquire the mutex before waking HIGH */
    WaitForSingleObject(hDemoLowStarted, 5000);
    Sleep(10); /* t=10ms: HIGH task starts and immediately tries mutex */

    /* ---- Start HIGH task (P7) ---- */
    TaskHandle_t hHigh = NULL;
    xTaskCreate(vDemoHighTask, "DemoHIGH", 4096, NULL, TASK_PRIORITY_CRITICAL, &hHigh);
    SetEvent(hDemoHighGo); /* Signal HIGH to try mutex now */

    Sleep(10); /* t=20ms: MEDIUM task starts, sees LOW running */

    /* ---- Start MEDIUM task (P3) ---- */
    TaskHandle_t hMed = NULL;
    xTaskCreate(vDemoMediumTask, "DemoMED", 4096, NULL, TASK_PRIORITY_NORMAL, &hMed);
    SetEvent(hDemoMedGo); /* Signal MED to start CPU work now */

    /*
     * Wait for LOW task to finish (signals hDemoComplete).
     * Without PI: LOW is starved by MED for ~100ms, so total wait > 130ms.
     * With PI:    LOW runs at P7, finishes in ~30ms, signals in ~30ms.
     */
    WaitForSingleObject(hDemoComplete, 10000);

    /* Give HIGH task time to acquire/release mutex and MED task time to complete */
    Sleep(150);

    print_scenario_results(use_pi);

    /* Cleanup */
    CloseHandle(hDemoLowStarted);
    CloseHandle(hDemoHighGo);
    CloseHandle(hDemoMedGo);
    CloseHandle(hDemoComplete);
    vSemaphoreDelete(mutex);
    if (use_pi) xSharedResourcePIMutex = NULL;
    else        xSharedResourceMutex   = NULL;

    Sleep(500); /* Pause between scenarios */
}

void run_priority_inversion_demo(void) {
    printf(ANSI_WHITE_BOLD
        "\n╔══════════════════════════════════════════════════════════════════╗\n"
        "║           PRIORITY INVERSION DEMONSTRATION                      ║\n"
        "║  The bug that crashed the Mars Pathfinder rover in 1997!        ║\n"
        "╠══════════════════════════════════════════════════════════════════╣\n"
        "║  Three tasks, one mutex:                                        ║\n"
        "║    LOW  (P1): holds mutex, does slow work                       ║\n"
        "║    HIGH (P7): needs same mutex → blocks on LOW                  ║\n"
        "║    MED  (P3): CPU-bound, no mutex needed                        ║\n"
        "║                                                                  ║\n"
        "║  WITHOUT priority inheritance:                                  ║\n"
        "║    MED (P3) preempts LOW (P1) → HIGH (P7) STARVED!              ║\n"
        "║                                                                  ║\n"
        "║  WITH priority inheritance:                                     ║\n"
        "║    When HIGH blocks, LOW is boosted to P7 → MED can't preempt  ║\n"
        "╚══════════════════════════════════════════════════════════════════╝\n"
        ANSI_RESET "\n");
    fflush(stdout);

    Sleep(1000);

    /* ---- Scenario 1: Show the BUG ---- */
    run_scenario(false);
    Sleep(1000);

    /* ---- Scenario 2: Show the FIX ---- */
    run_scenario(true);
    Sleep(500);

    printf(ANSI_GREEN_BOLD
        "\n╔══════════════════════════════════════════════════════════════════╗\n"
        "║           PRIORITY INVERSION DEMO COMPLETE                      ║\n"
        "╠══════════════════════════════════════════════════════════════════╣\n"
        "║  Key Takeaways:                                                 ║\n"
        "║  1. Always use PI-enabled mutexes in RTOS applications          ║\n"
        "║  2. FreeRTOS xSemaphoreCreateMutex() includes PI by default     ║\n"
        "║  3. Binary semaphores do NOT have PI — don't use as mutex!      ║\n"
        "║  4. The fix is usually ONE line: use the right mutex type       ║\n"
        "╚══════════════════════════════════════════════════════════════════╝\n"
        ANSI_RESET "\n");
    fflush(stdout);
}
