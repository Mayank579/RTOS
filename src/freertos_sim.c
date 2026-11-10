/*
 * freertos_sim.c - FreeRTOS Simulator Implementation (Win32 Backend)
 *
 * Implements the FreeRTOS-compatible API defined in freertos_sim.h using
 * Win32 threads and synchronization primitives. Each FreeRTOS concept maps
 * to a Win32 equivalent:
 *
 *   FreeRTOS Task        → Win32 CreateThread() with SetThreadPriority()
 *   FreeRTOS Queue       → Ring buffer + CRITICAL_SECTION + Events
 *   FreeRTOS Binary Sem  → Win32 CreateEvent() (auto-reset)
 *   FreeRTOS Counting Sem→ Win32 CreateSemaphore()
 *   FreeRTOS Mutex       → Win32 CreateMutex() + ownership tracking
 *   Priority Inheritance → Manual priority boost/restore via SetThreadPriority()
 *   vTaskDelay           → Win32 Sleep()
 *
 * Key Design Decision: We use Win32's preemptive thread scheduler, which
 * naturally simulates FreeRTOS's preemptive scheduler. Higher Win32 priority
 * threads preempt lower ones, just like FreeRTOS.
 */

#include "freertos_sim.h"
#include <stdarg.h>
#include <time.h>

/* =========================================================================
 * Global State
 * ========================================================================= */
volatile bool   g_schedulerRunning   = false;
uint32_t        g_totalMutexContentions = 0;
CRITICAL_SECTION g_printCS;

static LARGE_INTEGER g_startTime;
static LARGE_INTEGER g_frequency;

/* Thread-local storage key to find the TaskHandle_t for the calling thread */
static DWORD g_tlsTaskIndex = TLS_OUT_OF_INDEXES;

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

/*
 * Map FreeRTOS priority (1-7) to Win32 thread priority constants.
 *
 * Key design: We use extreme Win32 priority gaps to reliably simulate
 * preemption on a single core (affinity mask = 1). Windows dynamic priority
 * boosting can blur small gaps; large gaps (IDLE vs NORMAL vs TIME_CRITICAL)
 * create crisp, observable priority preemption.
 *
 *   FreeRTOS P1 (LOW)  → THREAD_PRIORITY_IDLE         (Windows priority 1)
 *   FreeRTOS P3 (MED)  → THREAD_PRIORITY_NORMAL       (Windows priority 8)
 *   FreeRTOS P7 (HIGH) → THREAD_PRIORITY_TIME_CRITICAL(Windows priority 15)
 *
 * This maps IDLE < NORMAL < TIME_CRITICAL, creating large gaps so that
 * MED (NORMAL) genuinely starves LOW (IDLE), and HIGH (TIME_CRITICAL)
 * preempts both. Priority Inheritance must boost LOW to TIME_CRITICAL to
 * prevent MED from running.
 */
static int freertos_to_win32_priority(UBaseType_t frPriority) {
    switch (frPriority) {
        case 0:  return THREAD_PRIORITY_IDLE;
        case 1:  return THREAD_PRIORITY_IDLE;          /* LOW  — truly background */
        case 2:  return THREAD_PRIORITY_LOWEST;
        case 3:  return THREAD_PRIORITY_NORMAL;        /* MED  — normal worker */
        case 4:  return THREAD_PRIORITY_ABOVE_NORMAL;
        case 5:  return THREAD_PRIORITY_HIGHEST;
        case 6:  return THREAD_PRIORITY_HIGHEST;
        case 7:  return THREAD_PRIORITY_TIME_CRITICAL; /* HIGH — time-critical */
        default: return THREAD_PRIORITY_NORMAL;
    }
}

/* Win32 thread entry point — wraps the FreeRTOS task function */
static DWORD WINAPI thread_proc(LPVOID lpParam) {
    ThreadArgs_t* args = (ThreadArgs_t*)lpParam;
    TaskHandle_t  tcb  = args->tcb;

    /* Store TCB in thread-local storage so tasks can find themselves */
    TlsSetValue(g_tlsTaskIndex, tcb);

    /* Call the actual FreeRTOS task function */
    args->func(args->param);

    /* Task returned (should not in FreeRTOS — tasks must call vTaskDelete) */
    tcb->running = false;
    free(args);
    return 0;
}

/* =========================================================================
 * Simulator Initialization / Control
 * ========================================================================= */

void vSimulatorInit(void) {
    /* Initialize the global print mutex */
    InitializeCriticalSection(&g_printCS);

    /* Record start time for tick counting */
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_startTime);

    /* Allocate thread-local storage slot */
    g_tlsTaskIndex = TlsAlloc();

    /*
     * SINGLE-CORE AFFINITY — Critical for realistic RTOS priority simulation.
     *
     * Real embedded MCUs (ARM Cortex-M, etc.) have exactly ONE core. When a
     * higher-priority thread runs, it truly preempts lower-priority ones — they
     * get zero CPU time until the higher-priority task blocks or finishes.
     *
     * On a modern PC (16 cores here), Windows distributes threads across all
     * cores in parallel, so a "preempted" thread still runs on another core.
     * This prevents the Priority Inversion bug from manifesting correctly.
     *
     * Pinning the entire process to Core 0 (affinity mask = 0x1) forces all
     * threads to compete on a single core, accurately simulating a real MCU.
     * We also pin the main thread and each created task thread individually
     * (SetThreadAffinityMask in xTaskCreate) for complete coverage.
     */
    SetProcessAffinityMask(GetCurrentProcess(), 1);
    SetThreadAffinityMask(GetCurrentThread(), 1);

    /*
     * Disable Windows dynamic priority boosting.
     * Windows automatically boosts starved threads to prevent starvation —
     * this is good for desktop apps but defeats our RTOS simulation, where
     * we WANT to demonstrate that a low-priority thread gets starved.
     * Disabling this means the OS scheduler strictly obeys our thread
     * priority values, accurately simulating a bare-metal RTOS.
     */
    SetProcessPriorityBoost(GetCurrentProcess(), TRUE);

    /* Enable UTF-8 encoding in Windows console */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /* Enable ANSI escape codes on Windows 10+ */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    g_schedulerRunning = false;
}

void vSimulatorStart(void) {
    g_schedulerRunning = true;
}

void vSimulatorStop(void) {
    g_schedulerRunning = false;
}

/* Returns milliseconds since simulator started — equivalent to xTaskGetTickCount() */
TickType_t xTaskGetTickCount(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (TickType_t)(((now.QuadPart - g_startTime.QuadPart) * 1000ULL)
                        / g_frequency.QuadPart);
}

uint32_t sim_get_ms(void) {
    return (uint32_t)xTaskGetTickCount();
}

/* Thread-safe printf */
void sim_printf(const char* format, ...) {
    EnterCriticalSection(&g_printCS);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    LeaveCriticalSection(&g_printCS);
}

/* =========================================================================
 * Task API Implementation
 * ========================================================================= */

BaseType_t xTaskCreate(
    TaskFunction_t  pvTaskCode,
    const char*     pcName,
    uint16_t        usStackDepth,
    void*           pvParameters,
    UBaseType_t     uxPriority,
    TaskHandle_t*   pxCreatedTask)
{
    (void)usStackDepth; /* Stack depth managed by OS in this simulator */

    /* Allocate Task Control Block (TCB) — in FreeRTOS this is on the heap */
    TaskHandle_t tcb = (TaskHandle_t)calloc(1, sizeof(TaskControlBlock_t));
    if (!tcb) return pdFAIL;

    strncpy(tcb->name, pcName, sizeof(tcb->name) - 1);
    tcb->priority     = uxPriority;
    tcb->basePriority = uxPriority;
    tcb->pvParameters = pvParameters;
    tcb->running      = true;

    /* Allocate thread args wrapper */
    ThreadArgs_t* args = (ThreadArgs_t*)malloc(sizeof(ThreadArgs_t));
    if (!args) { free(tcb); return pdFAIL; }
    args->func  = pvTaskCode;
    args->param = pvParameters;
    args->tcb   = tcb;

    /*
     * FreeRTOS Concept: CreateThread is equivalent to the RTOS creating a
     * new task context with its own stack pointer and program counter.
     * The Win32 scheduler then preempts based on thread priority, mirroring
     * the FreeRTOS preemptive scheduler.
     */
    tcb->thread = CreateThread(
        NULL,                              /* Default security */
        0,                                 /* Default stack size */
        thread_proc,                       /* Thread function */
        args,                              /* Arguments */
        CREATE_SUSPENDED,                  /* Don't start yet */
        &tcb->threadId
    );

    if (!tcb->thread) {
        free(args);
        free(tcb);
        return pdFAIL;
    }

    /* Set Win32 thread priority to match FreeRTOS priority */
    SetThreadPriority(tcb->thread, freertos_to_win32_priority(uxPriority));

    /*
     * SINGLE-CORE AFFINITY PER THREAD — Forces this thread to run only on
     * Core 0 (affinity mask = 0x1). This is the per-thread equivalent of
     * SetProcessAffinityMask and does NOT require admin privileges.
     *
     * Without this, all 16 cores on this PC can run threads in parallel,
     * which prevents priority inversion from manifesting — a P1 thread and
     * a P3 thread simply run simultaneously on different cores, so the P3
     * thread never actually "preempts" the P1 thread.
     *
     * With single-core affinity, threads compete on one core, accurately
     * simulating a single-core embedded MCU (ARM Cortex-M, AVR, etc.).
     */
    SetThreadAffinityMask(tcb->thread, 1);

    /* Start the thread */
    ResumeThread(tcb->thread);

    if (pxCreatedTask) *pxCreatedTask = tcb;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t xTask) {
    TaskHandle_t tcb = xTask;
    if (!tcb) {
        /* Delete calling task */
        tcb = (TaskHandle_t)TlsGetValue(g_tlsTaskIndex);
        if (tcb) tcb->running = false;
        ExitThread(0);
        return;
    }
    tcb->running = false;
    TerminateThread(tcb->thread, 0);
    CloseHandle(tcb->thread);
    free(tcb);
}

/*
 * vTaskDelay — The calling task gives up the CPU for xTicksToDelay ms.
 *
 * FreeRTOS Concept: This is cooperative suspension. The scheduler moves the
 * task to the "Blocked" state and runs the next highest-priority ready task.
 * The task re-enters "Ready" state when the delay expires.
 *
 * Task States in FreeRTOS:
 *   Running → Blocked (vTaskDelay called)
 *   Blocked → Ready   (delay expired)
 *   Ready   → Running (scheduler selects it)
 */
void vTaskDelay(TickType_t xTicksToDelay) {
    TickType_t deadline = xTaskGetTickCount() + xTicksToDelay;
    while (xTaskGetTickCount() < deadline && g_schedulerRunning) {
        TickType_t remaining = deadline - xTaskGetTickCount();
        Sleep((remaining > 20) ? 20 : (DWORD)remaining);
    }
}

void vTaskDelayUntil(TickType_t* pxPreviousWakeTime, TickType_t xTimeIncrement) {
    TickType_t targetWakeTime = *pxPreviousWakeTime + xTimeIncrement;
    TickType_t now = xTaskGetTickCount();

    while (targetWakeTime > now && g_schedulerRunning) {
        TickType_t diff = targetWakeTime - now;
        Sleep((diff > 20) ? 20 : (DWORD)diff);
        now = xTaskGetTickCount();
    }
    *pxPreviousWakeTime = targetWakeTime;
}

UBaseType_t uxTaskPriorityGet(TaskHandle_t xTask) {
    if (!xTask) {
        TaskHandle_t self = (TaskHandle_t)TlsGetValue(g_tlsTaskIndex);
        return self ? self->priority : TASK_PRIORITY_NORMAL;
    }
    return xTask->priority;
}

void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority) {
    if (!xTask) return;
    xTask->priority = uxNewPriority;
    SetThreadPriority(xTask->thread, freertos_to_win32_priority(uxNewPriority));
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    return (TaskHandle_t)TlsGetValue(g_tlsTaskIndex);
}

const char* pcTaskGetName(TaskHandle_t xTask) {
    if (!xTask) {
        TaskHandle_t self = (TaskHandle_t)TlsGetValue(g_tlsTaskIndex);
        return self ? self->name : "UNKNOWN";
    }
    return xTask->name;
}

/* =========================================================================
 * Queue API Implementation
 *
 * FreeRTOS Concept: Queues are the PREFERRED way for tasks to communicate.
 * They decouple producers from consumers and are inherently thread-safe.
 * Data is COPIED into the queue — no shared pointers, no races.
 * ========================================================================= */

QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize) {
    Queue_t* q = (Queue_t*)calloc(1, sizeof(Queue_t));
    if (!q) return NULL;

    q->buffer    = (uint8_t*)calloc(uxQueueLength, uxItemSize);
    if (!q->buffer) { free(q); return NULL; }

    q->itemSize  = uxItemSize;
    q->maxItems  = uxQueueLength;
    q->count     = 0;
    q->head      = 0;
    q->tail      = 0;
    q->highWaterMark = 0;

    InitializeCriticalSection(&q->cs);

    /*
     * notEmpty event: auto-reset, initially not signaled.
     * Waiting tasks (xQueueReceive) block on this event.
     * When an item is added, we signal it to wake ONE waiting receiver.
     *
     * auto-reset = only ONE waiting thread is released per signal (like a
     * FreeRTOS queue where only the highest-priority receiver wakes up).
     */
    q->notEmpty = CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset */
    q->notFull  = CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset */

    return q;
}

BaseType_t xQueueSend(QueueHandle_t xQueue, const void* pvItemToQueue, TickType_t xTicksToWait) {
    Queue_t* q = (Queue_t*)xQueue;
    if (!q) return pdFAIL;

    TickType_t deadline = xTaskGetTickCount() + xTicksToWait;

    while (1) {
        EnterCriticalSection(&q->cs);

        if (q->count < q->maxItems) {
            /* Space available — copy item into queue buffer */
            uint8_t* dest = q->buffer + (q->tail * q->itemSize);
            memcpy(dest, pvItemToQueue, q->itemSize);
            q->tail = (q->tail + 1) % q->maxItems;
            q->count++;

            /* Track high-water mark */
            if (q->count > q->highWaterMark)
                q->highWaterMark = q->count;

            LeaveCriticalSection(&q->cs);

            /* Signal any waiting receiver that data is available */
            SetEvent(q->notEmpty);
            return pdPASS;
        }

        LeaveCriticalSection(&q->cs);

        /* Queue full — should we wait? */
        if (xTicksToWait == 0) return pdFAIL;
        if (xTaskGetTickCount() >= deadline && xTicksToWait != portMAX_DELAY)
            return pdFAIL;

        /*
         * FreeRTOS Concept: When a queue is full, the sending task enters
         * the "Blocked" state, waiting for space. This is equivalent to
         * blocking on the notFull event here.
         */
        DWORD waitMs = (xTicksToWait == portMAX_DELAY) ?
                       INFINITE :
                       (DWORD)(deadline - xTaskGetTickCount());
        WaitForSingleObject(q->notFull, waitMs);
    }
}

BaseType_t xQueueSendToFront(QueueHandle_t xQueue, const void* pvItemToQueue, TickType_t xTicksToWait) {
    Queue_t* q = (Queue_t*)xQueue;
    if (!q) return pdFAIL;

    EnterCriticalSection(&q->cs);
    if (q->count >= q->maxItems) {
        LeaveCriticalSection(&q->cs);
        return pdFAIL;
    }

    /* Insert at HEAD (front) of circular buffer */
    q->head = (q->head == 0) ? (q->maxItems - 1) : (q->head - 1);
    memcpy(q->buffer + (q->head * q->itemSize), pvItemToQueue, q->itemSize);
    q->count++;
    if (q->count > q->highWaterMark) q->highWaterMark = q->count;

    LeaveCriticalSection(&q->cs);
    SetEvent(q->notEmpty);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t xQueue, void* pvBuffer, TickType_t xTicksToWait) {
    Queue_t* q = (Queue_t*)xQueue;
    if (!q) return pdFAIL;

    TickType_t deadline = xTaskGetTickCount() + xTicksToWait;

    while (1) {
        EnterCriticalSection(&q->cs); 

        if (q->count > 0) {
            /* Data available — copy out of queue buffer */
            memcpy(pvBuffer, q->buffer + (q->head * q->itemSize), q->itemSize);
            q->head = (q->head + 1) % q->maxItems;
            q->count--;

            LeaveCriticalSection(&q->cs);

            /* Signal any waiting sender that space is available */
            SetEvent(q->notFull);
            return pdPASS;
        }

        LeaveCriticalSection(&q->cs);

        if (xTicksToWait == 0) return pdFAIL;
        if (xTaskGetTickCount() >= deadline && xTicksToWait != portMAX_DELAY)
            return pdFAIL;
        if (!g_schedulerRunning) return pdFAIL;

        /*
         * FreeRTOS Concept: Receiver blocks here waiting for data.
         * This is the "Blocked" state — no CPU is consumed while waiting.
         * When a sender calls xQueueSend, SetEvent() wakes us up.
         */
        DWORD waitMs = (xTicksToWait == portMAX_DELAY) ?
                       INFINITE :
                       (DWORD)(deadline - xTaskGetTickCount());
        DWORD slice = (waitMs > 100) ? 100 : waitMs;
        WaitForSingleObject(q->notEmpty, slice);
        if (!g_schedulerRunning) return pdFAIL;
    }
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t xQueue) {
    Queue_t* q = (Queue_t*)xQueue;
    if (!q) return 0;
    EnterCriticalSection(&q->cs);
    UBaseType_t count = q->count;
    LeaveCriticalSection(&q->cs);
    return count;
}

UBaseType_t uxQueueGetHighWaterMark(QueueHandle_t xQueue) {
    return xQueue ? ((Queue_t*)xQueue)->highWaterMark : 0;
}

void vQueueDelete(QueueHandle_t xQueue) {
    Queue_t* q = (Queue_t*)xQueue;
    if (!q) return;
    DeleteCriticalSection(&q->cs);
    CloseHandle(q->notEmpty);
    CloseHandle(q->notFull);
    free(q->buffer);
    free(q);
}

/* =========================================================================
 * Semaphore / Mutex Implementation
 *
 * FreeRTOS Concept: All three (binary sem, counting sem, mutex) share the
 * same SemaphoreHandle_t type. The difference is in semantics:
 *   - Binary: signaling (ISR→task, task→task)
 *   - Counting: resource pools
 *   - Mutex: mutual exclusion with ownership + priority inheritance
 * ========================================================================= */

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    Semaphore_t* s = (Semaphore_t*)calloc(1, sizeof(Semaphore_t));
    if (!s) return NULL;

    s->type = SEM_TYPE_BINARY;
    /*
     * Win32 auto-reset event maps perfectly to a FreeRTOS binary semaphore:
     *   - SetEvent  → xSemaphoreGive (post/signal)
     *   - WaitForSingleObject → xSemaphoreTake (pend/wait)
     * Auto-reset ensures only ONE task is released per Give.
     * Initial state: NOT signaled (must be Given before it can be Taken).
     */
    s->handle = CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset, initially 0 */
    s->count  = 0;
    s->maxCount = 1;
    InitializeCriticalSection(&s->cs);
    return s;
}

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount) {
    Semaphore_t* s = (Semaphore_t*)calloc(1, sizeof(Semaphore_t));
    if (!s) return NULL;

    s->type     = SEM_TYPE_COUNTING;
    s->count    = uxInitialCount;
    s->maxCount = uxMaxCount;
    /*
     * Win32 CreateSemaphore maps to FreeRTOS counting semaphore.
     * The initial count is the number of resources available immediately.
     */
    s->handle   = CreateSemaphore(NULL, (LONG)uxInitialCount, (LONG)uxMaxCount, NULL);
    InitializeCriticalSection(&s->cs);
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    Semaphore_t* s = (Semaphore_t*)calloc(1, sizeof(Semaphore_t));
    if (!s) return NULL;

    s->type     = SEM_TYPE_MUTEX;
    s->count    = 1;   /* Initially AVAILABLE (unlike binary semaphore) */
    s->maxCount = 1;
    s->owner    = NULL;
    s->priorityInheritanceEnabled = false;
    /*
     * Win32 CreateMutex with bInitialOwner=FALSE means initially unlocked.
     * FreeRTOS mutexes are also initially available.
     */
    s->handle   = CreateMutex(NULL, FALSE, NULL);
    InitializeCriticalSection(&s->cs);
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutexWithPI(void) {
    Semaphore_t* s = (Semaphore_t*)xSemaphoreCreateMutex();
    if (s) {
        s->type = SEM_TYPE_MUTEX_PI;
        s->priorityInheritanceEnabled = true;
    }
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait) {
    Semaphore_t* s = (Semaphore_t*)xSemaphore;
    if (!s) return pdFAIL;

    DWORD waitMs = (xTicksToWait == portMAX_DELAY) ?
                   INFINITE :
                   (DWORD)xTicksToWait;

    if (s->type == SEM_TYPE_BINARY) {
        DWORD result = WaitForSingleObject(s->handle, waitMs);
        return (result == WAIT_OBJECT_0) ? pdPASS : pdFAIL;
    }
    else if (s->type == SEM_TYPE_COUNTING) {
        DWORD result = WaitForSingleObject(s->handle, waitMs);
        if (result == WAIT_OBJECT_0) {
            EnterCriticalSection(&s->cs);
            if (s->count > 0) s->count--;
            LeaveCriticalSection(&s->cs);
            return pdPASS;
        }
        return pdFAIL;
    }
    else {
        /* MUTEX (with or without PI) */
        TaskHandle_t caller = (TaskHandle_t)TlsGetValue(g_tlsTaskIndex);

        /* Check if mutex is already owned — if so, increment contention counter */
        EnterCriticalSection(&s->cs);
        bool contended = (s->owner != NULL && s->owner != caller);
        if (contended) {
            s->contentionCount++;
            InterlockedIncrement((LONG*)&g_totalMutexContentions);

            /*
             * FreeRTOS Priority Inheritance:
             * If the mutex has PI enabled AND the current owner has LOWER
             * priority than us, BOOST the owner's priority to ours.
             * This prevents medium-priority tasks from starving the mutex owner.
             */
            if (s->priorityInheritanceEnabled && s->owner && caller) {
                if (s->owner->priority < caller->priority) {
                    sim_printf(ANSI_YELLOW
                        "[PI] Priority inheritance: boosting task '%s' P%u → P%u "
                        "(waiting task '%s' has P%u)\n" ANSI_RESET,
                        s->owner->name, s->owner->priority, caller->priority,
                        caller->name, caller->priority);
                    vTaskPrioritySet(s->owner, caller->priority);
                }
            }
        }
        LeaveCriticalSection(&s->cs);

        /* Block until mutex is available */
        DWORD result = WaitForSingleObject(s->handle, waitMs);
        if (result == WAIT_OBJECT_0) {
            EnterCriticalSection(&s->cs);
            s->owner = caller;
            LeaveCriticalSection(&s->cs);
            return pdPASS;
        }
        return pdFAIL;
    }
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore) {
    Semaphore_t* s = (Semaphore_t*)xSemaphore;
    if (!s) return pdFAIL;

    if (s->type == SEM_TYPE_BINARY) {
        SetEvent(s->handle);
        return pdPASS;
    }
    else if (s->type == SEM_TYPE_COUNTING) {
        EnterCriticalSection(&s->cs);
        if (s->count >= s->maxCount) {
            LeaveCriticalSection(&s->cs);
            return pdFAIL;
        }
        s->count++;
        LeaveCriticalSection(&s->cs);
        ReleaseSemaphore(s->handle, 1, NULL);
        return pdPASS;
    }
    else {
        /* MUTEX (with or without PI) */
        EnterCriticalSection(&s->cs);
        TaskHandle_t prevOwner = s->owner;
        s->owner = NULL;
        LeaveCriticalSection(&s->cs);

        /*
         * FreeRTOS Priority Inheritance — Restore:
         * When releasing a PI mutex, the owner's priority is restored to its
         * ORIGINAL (base) priority. This undoes any boost that was applied
         * when a higher-priority task started waiting.
         */
        if (s->priorityInheritanceEnabled && prevOwner) {
            if (prevOwner->priority != prevOwner->basePriority) {
                sim_printf(ANSI_YELLOW
                    "[PI] Priority inheritance restored: task '%s' P%u → P%u\n"
                    ANSI_RESET,
                    prevOwner->name, prevOwner->priority, prevOwner->basePriority);
                vTaskPrioritySet(prevOwner, prevOwner->basePriority);
            }
        }

        ReleaseMutex(s->handle);
        return pdPASS;
    }
}

void vSemaphoreDelete(SemaphoreHandle_t xSemaphore) {
    Semaphore_t* s = (Semaphore_t*)xSemaphore;
    if (!s) return;
    DeleteCriticalSection(&s->cs);
    CloseHandle(s->handle);
    free(s);
}
