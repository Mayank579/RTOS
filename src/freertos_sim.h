/*
 * freertos_sim.h - FreeRTOS-Compatible Simulator API
 *
 * This header defines a FreeRTOS-compatible API layer implemented on top of
 * Win32 threads and synchronization primitives. The goal is to demonstrate
 * real RTOS concepts (tasks, queues, semaphores, mutexes, priority inversion)
 * using an API surface identical to FreeRTOS, running on Windows via MinGW.
 *
 * FreeRTOS Concepts Demonstrated:
 *   - Tasks (xTaskCreate, vTaskDelete, vTaskDelay)
 *   - Queues (xQueueCreate, xQueueSend, xQueueReceive)
 *   - Binary Semaphores (xSemaphoreCreateBinary, xSemaphoreGive, xSemaphoreTake)
 *   - Counting Semaphores (xSemaphoreCreateCounting)
 *   - Mutexes (xSemaphoreCreateMutex) with optional Priority Inheritance
 *   - Task Priorities (1=lowest ... 7=highest)
 *   - Preemptive Scheduling (simulated via Windows thread priorities)
 */

#ifndef FREERTOS_SIM_H
#define FREERTOS_SIM_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * FreeRTOS Type Definitions
 * In real FreeRTOS these map to architecture-specific types.
 * Here we define them using Win32 primitives.
 * ========================================================================= */

typedef uint32_t  TickType_t;       /* FreeRTOS tick count type */
typedef int32_t   BaseType_t;       /* FreeRTOS base integer type */
typedef uint32_t  UBaseType_t;      /* FreeRTOS unsigned base type */

#define pdTRUE    ((BaseType_t) 1)
#define pdFALSE   ((BaseType_t) 0)
#define pdPASS    pdTRUE
#define pdFAIL    pdFALSE

/* portMAX_DELAY - wait forever (like FreeRTOS's portMAX_DELAY) */
#define portMAX_DELAY  ((TickType_t) 0xFFFFFFFF)

/* Convert ms to FreeRTOS ticks (1 tick = 1ms in this simulator) */
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))

/* =========================================================================
 * Task Priority Levels
 * FreeRTOS priority: higher number = higher priority
 * Mapped to Win32 thread priorities for preemption simulation.
 * ========================================================================= */
#define TASK_PRIORITY_IDLE       0
#define TASK_PRIORITY_LOW        1
#define TASK_PRIORITY_BELOW_NORMAL 2
#define TASK_PRIORITY_NORMAL     3
#define TASK_PRIORITY_ABOVE_NORMAL 4
#define TASK_PRIORITY_HIGH       5
#define TASK_PRIORITY_ALERT      6
#define TASK_PRIORITY_CRITICAL   7

/* =========================================================================
 * Task Handle
 * In FreeRTOS, TaskHandle_t is an opaque pointer to a Task Control Block (TCB).
 * Here we wrap a Win32 HANDLE with metadata.
 * ========================================================================= */
typedef struct TaskControlBlock {
    HANDLE          thread;          /* Win32 thread handle */
    DWORD           threadId;        /* Win32 thread ID */
    char            name[32];        /* Task name (for debugging) */
    UBaseType_t     priority;        /* FreeRTOS priority (1-7) */
    UBaseType_t     basePriority;    /* Original priority (for PI restore) */
    bool            running;         /* Is task running? */
    void*           pvParameters;    /* Task parameters */
} TaskControlBlock_t;

typedef TaskControlBlock_t* TaskHandle_t;

/* Task function signature — identical to FreeRTOS */
typedef void (*TaskFunction_t)(void* pvParameters);

/* Internal thread proc wrapper */
typedef struct {
    TaskFunction_t  func;
    void*           param;
    TaskHandle_t    tcb;
} ThreadArgs_t;

/* =========================================================================
 * Queue
 * FreeRTOS Concept: Queues are the primary IPC mechanism.
 * xQueueSend() copies data INTO the queue (by value, not pointer).
 * xQueueReceive() copies data OUT of the queue.
 * This prevents shared memory races — tasks never share the data buffer.
 * ========================================================================= */
#define MAX_QUEUE_ITEMS 32

typedef struct {
    uint8_t*        buffer;          /* Circular buffer storage */
    size_t          itemSize;        /* Size of each item in bytes */
    UBaseType_t     maxItems;        /* Maximum queue depth */
    UBaseType_t     count;           /* Current item count */
    UBaseType_t     head;            /* Read index */
    UBaseType_t     tail;            /* Write index */
    CRITICAL_SECTION cs;            /* Win32 critical section (like a spinlock) */
    HANDLE          notEmpty;        /* Signaled when item added */
    HANDLE          notFull;         /* Signaled when item removed */
    UBaseType_t     highWaterMark;   /* Peak fill level (for statistics) */
} Queue_t;

typedef Queue_t* QueueHandle_t;

/* =========================================================================
 * Semaphore / Mutex
 * FreeRTOS uses the SAME type for binary semaphores, counting semaphores,
 * and mutexes — they're all "semaphores" internally.
 *
 * Binary Semaphore: signaling between tasks (ISR → task)
 * Counting Semaphore: resource counting
 * Mutex: mutual exclusion with optional priority inheritance
 * ========================================================================= */
typedef enum {
    SEM_TYPE_BINARY,
    SEM_TYPE_COUNTING,
    SEM_TYPE_MUTEX,
    SEM_TYPE_MUTEX_PI   /* Mutex WITH priority inheritance */
} SemType_t;

typedef struct Semaphore {
    SemType_t       type;
    HANDLE          handle;          /* Win32 semaphore or event handle */
    CRITICAL_SECTION cs;            /* Protects owner/priority fields */
    TaskHandle_t    owner;           /* Current mutex owner (NULL if free) */
    UBaseType_t     count;           /* For counting semaphores */
    UBaseType_t     maxCount;        /* Max count for counting semaphores */
    bool            priorityInheritanceEnabled; /* PI mutex flag */
    /* Statistics */
    uint32_t        contentionCount; /* How many times tasks had to wait */
} Semaphore_t;

typedef Semaphore_t* SemaphoreHandle_t;

/* In FreeRTOS, MutexHandle_t is just SemaphoreHandle_t */
typedef SemaphoreHandle_t MutexHandle_t;

/* =========================================================================
 * Global Simulator State
 * ========================================================================= */
extern volatile bool g_schedulerRunning;
extern uint32_t      g_totalMutexContentions;
extern CRITICAL_SECTION g_printCS;    /* Serializes all console output */

/* =========================================================================
 * Simulator Initialization / Control
 * ========================================================================= */
void vSimulatorInit(void);
void vSimulatorStart(void);
void vSimulatorStop(void);
TickType_t xTaskGetTickCount(void);  /* Returns ms since simulator start */

/* =========================================================================
 * Task API — mirrors FreeRTOS task.h
 * ========================================================================= */

/*
 * xTaskCreate — Creates a new task (like spawning a thread with priority).
 *
 * FreeRTOS Concept: Each task is an independent thread of execution with its
 * own stack. The scheduler decides which task runs based on priority.
 *
 * Parameters:
 *   pvTaskCode    - Pointer to task function
 *   pcName        - Human-readable name (for debugging)
 *   usStackDepth  - Stack size in words (ignored in sim, OS manages it)
 *   pvParameters  - Passed to task function as pvParameters
 *   uxPriority    - FreeRTOS priority level (1=lowest, 7=highest)
 *   pxCreatedTask - Output: handle to created task
 */
BaseType_t xTaskCreate(
    TaskFunction_t  pvTaskCode,
    const char*     pcName,
    uint16_t        usStackDepth,
    void*           pvParameters,
    UBaseType_t     uxPriority,
    TaskHandle_t*   pxCreatedTask
);

/*
 * vTaskDelete — Terminates a task.
 * Pass NULL to delete the calling task.
 */
void vTaskDelete(TaskHandle_t xTask);

/*
 * vTaskDelay — Suspends the calling task for a given number of ticks.
 *
 * FreeRTOS Concept: This is a BLOCKING call. The scheduler runs other tasks
 * while this task sleeps. It does NOT busy-wait (that would waste CPU).
 *
 * xTicksToDelay — Use pdMS_TO_TICKS() to convert milliseconds to ticks.
 */
void vTaskDelay(TickType_t xTicksToDelay);

/*
 * vTaskDelayUntil — Delays until an absolute tick count.
 * Used for precise periodic tasks (avoids drift from vTaskDelay).
 */
void vTaskDelayUntil(TickType_t* pxPreviousWakeTime, TickType_t xTimeIncrement);

/* Get/Set task priority */
UBaseType_t uxTaskPriorityGet(TaskHandle_t xTask);
void        vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority);

/* Get calling task's handle */
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char*  pcTaskGetName(TaskHandle_t xTask);

/* =========================================================================
 * Queue API — mirrors FreeRTOS queue.h
 * ========================================================================= */

/*
 * xQueueCreate — Creates a queue.
 *
 * FreeRTOS Concept: Queues have FIXED item size and depth.
 * They store copies of data, not pointers — this is key for thread safety!
 *
 * uxQueueLength  - Maximum number of items the queue can hold
 * uxItemSize     - Size in bytes of each item
 */
QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize);

/*
 * xQueueSend — Sends an item to the BACK of the queue (FIFO).
 *
 * FreeRTOS Concept: If the queue is full, the task BLOCKS for xTicksToWait.
 * If xTicksToWait=0, returns immediately (non-blocking).
 * If xTicksToWait=portMAX_DELAY, blocks forever.
 *
 * Returns pdPASS if sent, pdFAIL if queue was full and timeout expired.
 */
BaseType_t xQueueSend(QueueHandle_t xQueue, const void* pvItemToQueue, TickType_t xTicksToWait);

/*
 * xQueueSendToFront — Sends to the FRONT of the queue (LIFO / urgent).
 * Used for high-priority messages that need to jump the queue.
 */
BaseType_t xQueueSendToFront(QueueHandle_t xQueue, const void* pvItemToQueue, TickType_t xTicksToWait);

/*
 * xQueueReceive — Receives (removes) an item from the queue.
 *
 * FreeRTOS Concept: Blocks for xTicksToWait if queue is empty.
 * This is the primary way tasks wait for work without busy-waiting.
 */
BaseType_t xQueueReceive(QueueHandle_t xQueue, void* pvBuffer, TickType_t xTicksToWait);

/*
 * uxQueueMessagesWaiting — Returns items currently in the queue.
 */
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t xQueue);

/*
 * uxQueueGetHighWaterMark — Returns the peak fill level (for tuning).
 */
UBaseType_t uxQueueGetHighWaterMark(QueueHandle_t xQueue);

void vQueueDelete(QueueHandle_t xQueue);

/* =========================================================================
 * Semaphore API — mirrors FreeRTOS semphr.h
 * ========================================================================= */

/*
 * xSemaphoreCreateBinary — Creates a binary semaphore (initially EMPTY).
 *
 * FreeRTOS Concept: Binary semaphore is the simplest signaling primitive.
 * Unlike a mutex, it has NO ownership — any task can Give or Take it.
 * Commonly used for ISR → task notification: ISR Gives, task Takes.
 *
 * State: 0 (empty/not signaled) or 1 (full/signaled)
 */
SemaphoreHandle_t xSemaphoreCreateBinary(void);

/*
 * xSemaphoreCreateCounting — Creates a counting semaphore.
 *
 * FreeRTOS Concept: Like multiple binary semaphores stacked.
 * Used to track a count of available resources.
 * Example: 3 DMA channels → counting semaphore with max=3.
 */
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount);

/*
 * xSemaphoreCreateMutex — Creates a mutex (initially AVAILABLE).
 *
 * FreeRTOS Concept: Mutex is a binary semaphore WITH ownership.
 * Only the task that Takes (locks) can Give (unlock) it.
 * FreeRTOS mutexes include priority inheritance to prevent priority inversion.
 *
 * NOTE: Do NOT use mutexes from ISRs — use binary semaphores instead.
 */
SemaphoreHandle_t xSemaphoreCreateMutex(void);

/*
 * xSemaphoreCreateMutexWithPI — Creates a mutex WITH priority inheritance.
 *
 * Priority Inheritance: When a HIGH priority task blocks waiting for a mutex
 * held by a LOW priority task, the LOW task's priority is TEMPORARILY RAISED
 * to the HIGH task's priority. This prevents MEDIUM priority tasks from
 * starving the LOW task (and thus the HIGH task waiting for it).
 */
SemaphoreHandle_t xSemaphoreCreateMutexWithPI(void);

/*
 * xSemaphoreTake — Acquires a semaphore/mutex.
 *
 * FreeRTOS Concept: If unavailable, task BLOCKS for xTicksToWait ticks.
 * For mutexes: records ownership and applies priority inheritance if needed.
 *
 * Returns pdPASS if acquired, pdFAIL if timeout.
 */
BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);

/*
 * xSemaphoreGive — Releases a semaphore/mutex.
 *
 * FreeRTOS Concept: For mutexes, restores the owner's original priority
 * (undoing any priority inheritance boost applied during Take).
 *
 * Returns pdPASS on success, pdFAIL if already at max (counting sem).
 */
BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore);

void vSemaphoreDelete(SemaphoreHandle_t xSemaphore);

/* =========================================================================
 * ANSI Color Codes for Educational Output
 * ========================================================================= */
#define ANSI_RESET   "\033[0m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_WHITE   "\033[37m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RED_BOLD     "\033[1;31m"
#define ANSI_GREEN_BOLD   "\033[1;32m"
#define ANSI_YELLOW_BOLD  "\033[1;33m"
#define ANSI_CYAN_BOLD    "\033[1;36m"
#define ANSI_WHITE_BOLD   "\033[1;37m"

/*
 * sim_printf — Thread-safe printf using a global print mutex.
 * In FreeRTOS, printf is NOT thread-safe by default; you must protect it.
 * This simulates the common pattern of a "logging mutex".
 */
void sim_printf(const char* format, ...);

/* Timestamp helper: returns ms since simulator start */
uint32_t sim_get_ms(void);

#endif /* FREERTOS_SIM_H */
