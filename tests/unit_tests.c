/*
 * unit_tests.c — Unit Test Suite for FreeRTOS Multi-Task Simulator
 *
 * Test Coverage:
 *   1. Queue Full and Empty Semantics:
 *      - Non-blocking receive on empty queue (immediate pdFAIL)
 *      - Blocking receive timeout on empty queue (waits requested ms, returns pdFAIL)
 *      - Non-blocking send on full queue (immediate pdFAIL, high-water mark accurate)
 *      - Blocking send timeout on full queue (waits requested ms, returns pdFAIL)
 *      - Cross-task unblocking on queue receive (consumer unblocks when producer sends)
 *      - Cross-task unblocking on queue send (producer unblocks when consumer receives)
 *
 *   2. Ring-Buffer Wraparound & Integrity:
 *      - Circular buffer head/tail wraparound past buffer boundary without data loss
 *      - Multi-cycle torture test (3,000 items streamed in irregular chunks through prime-sized queue)
 *      - xQueueSendToFront head-index backward wraparound (head=0 -> maxItems-1)
 *      - Boundary protection: no corruption or index drift when full/empty in wrapped state
 *
 *   3. Mutex Priority Inheritance:
 *      - Standard mutex without PI (contention tracked, but priority NOT boosted)
 *      - PI mutex priority boost (P1 owner boosted to P6 when high-priority task contends)
 *      - PI mutex priority restoration (P1 owner restored to base priority on xSemaphoreGive)
 *      - Multi-level cascade inheritance (P1 -> P3 -> P6 -> restored to P1)
 */

#include "freertos_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* =========================================================================
 * Test Framework Macros & Counters
 * ========================================================================= */
static int s_total_assertions = 0;
static int s_passed_assertions = 0;
static int s_failed_assertions = 0;
static int s_test_count = 0;
static int s_test_failures = 0;

#define TEST_BEGIN(name) \
    do { \
        s_test_count++; \
        int _prev_fails = s_failed_assertions; \
        uint32_t _start_ms = sim_get_ms(); \
        printf("  [RUN] %-50s ", name); \
        fflush(stdout);

#define TEST_END() \
        uint32_t _dur = sim_get_ms() - _start_ms; \
        if (s_failed_assertions == _prev_fails) { \
            printf(ANSI_GREEN_BOLD "[PASS]" ANSI_RESET " (%u ms)\n", _dur); \
        } else { \
            s_test_failures++; \
            printf(ANSI_RED_BOLD "[FAIL]" ANSI_RESET " (%u ms)\n", _dur); \
        } \
    } while(0)

#define TEST_ASSERT(cond, msg) \
    do { \
        s_total_assertions++; \
        if (cond) { \
            s_passed_assertions++; \
        } else { \
            s_failed_assertions++; \
            printf("\n" ANSI_RED_BOLD "    FAIL: %s (line %d): %s\n          Condition: %s" ANSI_RESET "\n", \
                   __FILE__, __LINE__, msg, #cond); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_INT(expected, actual, msg) \
    do { \
        int _exp = (int)(expected); \
        int _act = (int)(actual); \
        s_total_assertions++; \
        if (_exp == _act) { \
            s_passed_assertions++; \
        } else { \
            s_failed_assertions++; \
            printf("\n" ANSI_RED_BOLD "    FAIL: %s (line %d): %s\n          Expected %d, got %d" ANSI_RESET "\n", \
                   __FILE__, __LINE__, msg, _exp, _act); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_PTR(expected, actual, msg) \
    do { \
        const void* _exp = (const void*)(expected); \
        const void* _act = (const void*)(actual); \
        s_total_assertions++; \
        if (_exp == _act) { \
            s_passed_assertions++; \
        } else { \
            s_failed_assertions++; \
            printf("\n" ANSI_RED_BOLD "    FAIL: %s (line %d): %s\n          Expected %p, got %p" ANSI_RESET "\n", \
                   __FILE__, __LINE__, msg, _exp, _act); \
        } \
    } while(0)

/* =========================================================================
 * GROUP 1: Queue Full and Empty Semantics
 * ========================================================================= */

static void test_queue_empty_nonblocking(void) {
    TEST_BEGIN("Queue Empty: non-blocking receive fails immediately");

    QueueHandle_t q = xQueueCreate(4, sizeof(int));
    TEST_ASSERT(q != NULL, "xQueueCreate returns valid handle");
    TEST_ASSERT_EQUAL_INT(0, uxQueueMessagesWaiting(q), "Initial message count is 0");
    TEST_ASSERT_EQUAL_INT(0, uxQueueGetHighWaterMark(q), "Initial high-water mark is 0");

    int item = 0xAA;
    BaseType_t res = xQueueReceive(q, &item, 0);

    TEST_ASSERT(res == pdFAIL, "xQueueReceive on empty queue with timeout 0 returns pdFAIL");
    TEST_ASSERT_EQUAL_INT(0xAA, item, "Output buffer unchanged after failed receive");
    TEST_ASSERT_EQUAL_INT(0, uxQueueMessagesWaiting(q), "Message count remains 0");

    vQueueDelete(q);
    TEST_END();
}

static void test_queue_empty_blocking_timeout(void) {
    TEST_BEGIN("Queue Empty: blocking receive honors timeout");

    QueueHandle_t q = xQueueCreate(4, sizeof(int));
    TEST_ASSERT(q != NULL, "Queue created");

    int item = 0;
    uint32_t t_start = sim_get_ms();
    BaseType_t res = xQueueReceive(q, &item, pdMS_TO_TICKS(60));
    uint32_t elapsed = sim_get_ms() - t_start;

    TEST_ASSERT(res == pdFAIL, "xQueueReceive returns pdFAIL on timeout");
    TEST_ASSERT(elapsed >= 45 && elapsed <= 200, "Blocking duration matches requested timeout");
    TEST_ASSERT_EQUAL_INT(0, uxQueueMessagesWaiting(q), "Queue remains empty");

    vQueueDelete(q);
    TEST_END();
}

static void test_queue_full_nonblocking(void) {
    TEST_BEGIN("Queue Full: non-blocking send fails immediately when full");

    const UBaseType_t CAPACITY = 3;
    QueueHandle_t q = xQueueCreate(CAPACITY, sizeof(int));
    TEST_ASSERT(q != NULL, "Queue created");

    for (int i = 0; i < (int)CAPACITY; i++) {
        int val = 100 + i;
        BaseType_t send_res = xQueueSend(q, &val, 0);
        TEST_ASSERT(send_res == pdPASS, "xQueueSend succeeds when space available");
        TEST_ASSERT_EQUAL_INT(i + 1, uxQueueMessagesWaiting(q), "Item count matches sent items");
    }

    TEST_ASSERT_EQUAL_INT(CAPACITY, uxQueueMessagesWaiting(q), "Queue is full");
    TEST_ASSERT_EQUAL_INT(CAPACITY, uxQueueGetHighWaterMark(q), "High-water mark reaches max capacity");

    /* Attempt to send to full queue non-blocking */
    int overflow_item = 999;
    BaseType_t res = xQueueSend(q, &overflow_item, 0);
    TEST_ASSERT(res == pdFAIL, "xQueueSend on full queue with timeout 0 returns pdFAIL");
    TEST_ASSERT_EQUAL_INT(CAPACITY, uxQueueMessagesWaiting(q), "Count does not exceed maxItems");

    vQueueDelete(q);
    TEST_END();
}

static void test_queue_full_blocking_timeout(void) {
    TEST_BEGIN("Queue Full: blocking send honors timeout");

    QueueHandle_t q = xQueueCreate(2, sizeof(int));
    TEST_ASSERT(q != NULL, "Queue created");

    int v1 = 11, v2 = 22, v3 = 33;
    xQueueSend(q, &v1, 0);
    xQueueSend(q, &v2, 0);
    TEST_ASSERT_EQUAL_INT(2, uxQueueMessagesWaiting(q), "Queue is full");

    uint32_t t_start = sim_get_ms();
    BaseType_t res = xQueueSend(q, &v3, pdMS_TO_TICKS(60));
    uint32_t elapsed = sim_get_ms() - t_start;

    TEST_ASSERT(res == pdFAIL, "xQueueSend on full queue returns pdFAIL on timeout");
    TEST_ASSERT(elapsed >= 45 && elapsed <= 200, "Blocking duration matches requested timeout");
    TEST_ASSERT_EQUAL_INT(2, uxQueueMessagesWaiting(q), "Item count still 2");

    vQueueDelete(q);
    TEST_END();
}

typedef struct {
    QueueHandle_t q;
    int received_val;
    bool receive_ok;
    HANDLE started_event;
    HANDLE finished_event;
} QueueWorkerContext_t;

static void queue_consumer_task(void* pvParam) {
    QueueWorkerContext_t* ctx = (QueueWorkerContext_t*)pvParam;
    SetEvent(ctx->started_event);

    /* Block until item arrives */
    BaseType_t res = xQueueReceive(ctx->q, &ctx->received_val, pdMS_TO_TICKS(1000));
    ctx->receive_ok = (res == pdPASS);

    SetEvent(ctx->finished_event);
}

static void test_queue_cross_task_unblock_on_receive(void) {
    TEST_BEGIN("Queue Sync: blocked consumer unblocks on producer send");

    QueueWorkerContext_t ctx = {
        .q = xQueueCreate(2, sizeof(int)),
        .received_val = 0,
        .receive_ok = false,
        .started_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .finished_event = CreateEvent(NULL, FALSE, FALSE, NULL)
    };

    TaskHandle_t hConsumer = NULL;
    xTaskCreate(queue_consumer_task, "TestConsumer", 1024, &ctx, TASK_PRIORITY_NORMAL, &hConsumer);

    /* Wait until consumer has started and entered blocked receive */
    WaitForSingleObject(ctx.started_event, 500);
    Sleep(30); /* Give time for consumer to block inside xQueueReceive */

    int item_to_send = 4242;
    BaseType_t send_res = xQueueSend(ctx.q, &item_to_send, 0);
    TEST_ASSERT(send_res == pdPASS, "Producer successfully sent item");

    /* Wait for consumer to wake up and finish */
    DWORD wait_res = WaitForSingleObject(ctx.finished_event, 1000);
    TEST_ASSERT(wait_res == WAIT_OBJECT_0, "Consumer finished promptly after send");
    TEST_ASSERT(ctx.receive_ok, "Consumer received item successfully");
    TEST_ASSERT_EQUAL_INT(4242, ctx.received_val, "Consumer received exact item value sent");
    TEST_ASSERT_EQUAL_INT(0, uxQueueMessagesWaiting(ctx.q), "Queue is empty after consumption");

    WaitForSingleObject(hConsumer->thread, 1000);
    CloseHandle(hConsumer->thread);
    free(hConsumer);
    CloseHandle(ctx.started_event);
    CloseHandle(ctx.finished_event);
    vQueueDelete(ctx.q);

    TEST_END();
}

typedef struct {
    QueueHandle_t q;
    int item_to_send;
    bool send_ok;
    HANDLE started_event;
    HANDLE finished_event;
} QueueProducerContext_t;

static void queue_producer_task(void* pvParam) {
    QueueProducerContext_t* ctx = (QueueProducerContext_t*)pvParam;
    SetEvent(ctx->started_event);

    /* Block until queue space is freed */
    BaseType_t res = xQueueSend(ctx->q, &ctx->item_to_send, pdMS_TO_TICKS(1000));
    ctx->send_ok = (res == pdPASS);

    SetEvent(ctx->finished_event);
}

static void test_queue_cross_task_unblock_on_send(void) {
    TEST_BEGIN("Queue Sync: blocked producer unblocks on consumer receive");

    QueueProducerContext_t ctx = {
        .q = xQueueCreate(2, sizeof(int)),
        .item_to_send = 9999,
        .send_ok = false,
        .started_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .finished_event = CreateEvent(NULL, FALSE, FALSE, NULL)
    };

    /* Fill queue completely */
    int f1 = 1, f2 = 2;
    xQueueSend(ctx.q, &f1, 0);
    xQueueSend(ctx.q, &f2, 0);
    TEST_ASSERT_EQUAL_INT(2, uxQueueMessagesWaiting(ctx.q), "Queue pre-filled to max capacity");

    TaskHandle_t hProducer = NULL;
    xTaskCreate(queue_producer_task, "TestProducer", 1024, &ctx, TASK_PRIORITY_NORMAL, &hProducer);

    WaitForSingleObject(ctx.started_event, 500);
    Sleep(30); /* Producer is now blocked in xQueueSend */

    /* Consume one item to free slot */
    int popped = 0;
    BaseType_t rx_res = xQueueReceive(ctx.q, &popped, 0);
    TEST_ASSERT(rx_res == pdPASS && popped == 1, "Consumer popped first pre-filled item");

    /* Wait for blocked producer to unblock and send */
    DWORD wait_res = WaitForSingleObject(ctx.finished_event, 1000);
    TEST_ASSERT(wait_res == WAIT_OBJECT_0, "Producer unblocked upon space available");
    TEST_ASSERT(ctx.send_ok, "Producer successfully sent item");
    TEST_ASSERT_EQUAL_INT(2, uxQueueMessagesWaiting(ctx.q), "Queue full again with popped slot replaced");

    /* Verify items currently in queue are [2, 9999] */
    int r1 = 0, r2 = 0;
    xQueueReceive(ctx.q, &r1, 0);
    xQueueReceive(ctx.q, &r2, 0);
    TEST_ASSERT_EQUAL_INT(2, r1, "FIFO head holds second pre-filled item");
    TEST_ASSERT_EQUAL_INT(9999, r2, "FIFO tail holds producer's unblocked item");

    WaitForSingleObject(hProducer->thread, 1000);
    CloseHandle(hProducer->thread);
    free(hProducer);
    CloseHandle(ctx.started_event);
    CloseHandle(ctx.finished_event);
    vQueueDelete(ctx.q);

    TEST_END();
}

/* =========================================================================
 * GROUP 2: Ring-Buffer Wraparound & Integrity
 * ========================================================================= */

static void test_ring_buffer_basic_wraparound(void) {
    TEST_BEGIN("Ring Buffer: circular head/tail wraparound maintains FIFO");

    const UBaseType_t CAPACITY = 5;
    QueueHandle_t q = xQueueCreate(CAPACITY, sizeof(int));
    TEST_ASSERT(q != NULL, "Queue created");

    /* 1. Fill queue to max capacity: [10, 20, 30, 40, 50] */
    for (int i = 0; i < 5; i++) {
        int v = (i + 1) * 10;
        BaseType_t s = xQueueSend(q, &v, 0);
        TEST_ASSERT(s == pdPASS, "Initial fill send succeeds");
    }
    TEST_ASSERT_EQUAL_INT(5, uxQueueMessagesWaiting(q), "Queue is full");

    /* 2. Dequeue 3 items: [10, 20, 30]. Remaining: [40, 50]. Head index advances to 3 */
    for (int i = 0; i < 3; i++) {
        int v = 0;
        BaseType_t r = xQueueReceive(q, &v, 0);
        TEST_ASSERT(r == pdPASS, "Dequeue succeeds");
        TEST_ASSERT_EQUAL_INT((i + 1) * 10, v, "Dequeued item matches FIFO order");
    }
    TEST_ASSERT_EQUAL_INT(2, uxQueueMessagesWaiting(q), "Remaining count is 2");

    /* 3. Enqueue 3 new items: [60, 70, 80]. Tail index wraps past capacity-1 (4) to 0, 1, 2 */
    for (int i = 0; i < 3; i++) {
        int v = (i + 6) * 10;
        BaseType_t s = xQueueSend(q, &v, 0);
        TEST_ASSERT(s == pdPASS, "Wrap-around send succeeds");
    }
    TEST_ASSERT_EQUAL_INT(5, uxQueueMessagesWaiting(q), "Queue is full again after wrap");
    TEST_ASSERT_EQUAL_INT(5, uxQueueGetHighWaterMark(q), "High-water mark remains 5");

    /* 4. Dequeue all 5 items: must be [40, 50, 60, 70, 80] in strict FIFO sequence */
    int expected_vals[] = {40, 50, 60, 70, 80};
    for (int i = 0; i < 5; i++) {
        int v = 0;
        BaseType_t r = xQueueReceive(q, &v, 0);
        TEST_ASSERT(r == pdPASS, "Drain receive succeeds");
        TEST_ASSERT_EQUAL_INT(expected_vals[i], v, "Item after wraparound maintains strict FIFO ordering");
    }

    TEST_ASSERT_EQUAL_INT(0, uxQueueMessagesWaiting(q), "Queue empty after full drain");

    vQueueDelete(q);
    TEST_END();
}

static void test_ring_buffer_multicycle_torture(void) {
    TEST_BEGIN("Ring Buffer: multi-cycle torture test (3,000 items, prime size)");

    /* Prime queue size ensures non-power-of-two wraparound behavior */
    const UBaseType_t CAPACITY = 7;
    const int TOTAL_ITEMS = 3000;
    QueueHandle_t q = xQueueCreate(CAPACITY, sizeof(int));
    TEST_ASSERT(q != NULL, "Queue created");

    int items_sent = 0;
    int items_received = 0;
    int send_chunk = 1;
    int recv_chunk = 1;

    while (items_received < TOTAL_ITEMS) {
        /* Send a burst of items */
        send_chunk = (send_chunk % 5) + 1; /* Cycle chunk size 1..5 */
        for (int i = 0; i < send_chunk && items_sent < TOTAL_ITEMS; i++) {
            if (uxQueueMessagesWaiting(q) < CAPACITY) {
                BaseType_t s = xQueueSend(q, &items_sent, 0);
                if (s == pdPASS) {
                    items_sent++;
                }
            }
        }

        /* Receive a burst of items */
        recv_chunk = (recv_chunk % 4) + 1; /* Cycle chunk size 1..4 */
        for (int i = 0; i < recv_chunk && items_received < items_sent; i++) {
            int val = -1;
            BaseType_t r = xQueueReceive(q, &val, 0);
            if (r == pdPASS) {
                TEST_ASSERT_EQUAL_INT(items_received, val, "Torture stream item strictly in sequence");
                items_received++;
            }
        }
    }

    TEST_ASSERT_EQUAL_INT(TOTAL_ITEMS, items_sent, "All 3,000 items sent");
    TEST_ASSERT_EQUAL_INT(TOTAL_ITEMS, items_received, "All 3,000 items received");
    TEST_ASSERT_EQUAL_INT(0, uxQueueMessagesWaiting(q), "Queue cleanly emptied at end of torture run");
    TEST_ASSERT_EQUAL_INT(CAPACITY, uxQueueGetHighWaterMark(q), "Torture run exercised peak queue capacity");

    vQueueDelete(q);
    TEST_END();
}

static void test_ring_buffer_send_to_front_wraparound(void) {
    TEST_BEGIN("Ring Buffer: xQueueSendToFront head reverse wraparound");

    const UBaseType_t CAPACITY = 4;
    QueueHandle_t q = xQueueCreate(CAPACITY, sizeof(int));
    TEST_ASSERT(q != NULL, "Queue created");

    /* Initial state: head=0, tail=0 */
    /* 1. Normal send: 10, 20. Queue: [10, 20]. head=0, tail=2 */
    int v1 = 10, v2 = 20;
    xQueueSend(q, &v1, 0);
    xQueueSend(q, &v2, 0);

    /* 2. SendToFront: 99.
     * head was 0 -> wraps backwards to maxItems - 1 = 3.
     * Queue logical view: [99, 10, 20]. count=3
     */
    int urgent = 99;
    BaseType_t res = xQueueSendToFront(q, &urgent, 0);
    TEST_ASSERT(res == pdPASS, "xQueueSendToFront succeeds when space available");
    TEST_ASSERT_EQUAL_INT(3, uxQueueMessagesWaiting(q), "Count updated to 3");

    /* 3. Pop: Must receive 99 first! */
    int out = 0;
    xQueueReceive(q, &out, 0);
    TEST_ASSERT_EQUAL_INT(99, out, "Urgent item jumped queue to front");

    /* 4. Pop: Must receive 10 */
    xQueueReceive(q, &out, 0);
    TEST_ASSERT_EQUAL_INT(10, out, "Next item is 10");

    /* 5. Pop: Must receive 20 */
    xQueueReceive(q, &out, 0);
    TEST_ASSERT_EQUAL_INT(20, out, "Next item is 20");

    TEST_ASSERT_EQUAL_INT(0, uxQueueMessagesWaiting(q), "Queue empty");

    /* 6. Consecutive SendToFront calls crossing index 0 backwards */
    int u1 = 101, u2 = 102, u3 = 103;
    xQueueSendToFront(q, &u1, 0);
    xQueueSendToFront(q, &u2, 0);
    xQueueSendToFront(q, &u3, 0);
    TEST_ASSERT_EQUAL_INT(3, uxQueueMessagesWaiting(q), "Queue holds 3 front-sent items");

    /* Receives must come out in LIFO order for consecutive front-sends: 103, 102, 101 */
    int r1 = 0, r2 = 0, r3 = 0;
    xQueueReceive(q, &r1, 0);
    xQueueReceive(q, &r2, 0);
    xQueueReceive(q, &r3, 0);
    TEST_ASSERT_EQUAL_INT(103, r1, "LIFO front item 1");
    TEST_ASSERT_EQUAL_INT(102, r2, "LIFO front item 2");
    TEST_ASSERT_EQUAL_INT(101, r3, "LIFO front item 3");

    vQueueDelete(q);
    TEST_END();
}

static void test_ring_buffer_boundary_integrity_wrapped(void) {
    TEST_BEGIN("Ring Buffer: overflow & underflow safety in wrapped state");

    const UBaseType_t CAPACITY = 4;
    QueueHandle_t q = xQueueCreate(CAPACITY, sizeof(int));
    TEST_ASSERT(q != NULL, "Queue created");

    /* Fill 4, pop 2, push 2 -> queue is in wrapped state with tail < head */
    for (int i = 0; i < 4; i++) {
        int v = i;
        xQueueSend(q, &v, 0);
    }
    int dummy = 0;
    xQueueReceive(q, &dummy, 0); // pops 0
    xQueueReceive(q, &dummy, 0); // pops 1
    // Queue now has [2, 3] at indices 2 and 3. head = 2.
    int v4 = 4, v5 = 5;
    xQueueSend(q, &v4, 0); // written to index 0
    xQueueSend(q, &v5, 0); // written to index 1. tail = 2. Queue full!

    TEST_ASSERT_EQUAL_INT(4, uxQueueMessagesWaiting(q), "Queue full in wrapped state");

    /* Attempt overflow with Send */
    int overflow_val = 999;
    BaseType_t s_fail = xQueueSend(q, &overflow_val, 0);
    TEST_ASSERT(s_fail == pdFAIL, "xQueueSend fails on wrapped full queue");

    /* Attempt overflow with SendToFront */
    BaseType_t f_fail = xQueueSendToFront(q, &overflow_val, 0);
    TEST_ASSERT(f_fail == pdFAIL, "xQueueSendToFront fails on wrapped full queue");

    /* Verify contents were not corrupted by failed sends */
    int expected[] = {2, 3, 4, 5};
    for (int i = 0; i < 4; i++) {
        int item = -1;
        BaseType_t r = xQueueReceive(q, &item, 0);
        TEST_ASSERT(r == pdPASS, "Read wrapped item succeeds");
        TEST_ASSERT_EQUAL_INT(expected[i], item, "Wrapped item uncorrupted after failed overflow");
    }

    /* Underflow check */
    BaseType_t u_fail = xQueueReceive(q, &dummy, 0);
    TEST_ASSERT(u_fail == pdFAIL, "xQueueReceive fails on wrapped empty queue");

    vQueueDelete(q);
    TEST_END();
}

/* =========================================================================
 * GROUP 3: Mutex Priority Inheritance
 * ========================================================================= */

typedef struct {
    MutexHandle_t m;
    HANDLE low_acquired_event;
    HANDLE high_attempt_event;
    HANDLE low_can_release_event;
    HANDLE low_finished_event;
    HANDLE high_finished_event;
    bool high_take_succeeded;
    UBaseType_t low_observed_priority;
    UBaseType_t low_restored_priority;
} MutexTestContext_t;

/* Task L: Low priority worker */
static void mutex_test_low_task(void* pvParam) {
    MutexTestContext_t* ctx = (MutexTestContext_t*)pvParam;

    /* Acquire mutex */
    BaseType_t res = xSemaphoreTake(ctx->m, pdMS_TO_TICKS(1000));
    if (res == pdPASS) {
        SetEvent(ctx->low_acquired_event);

        /* Wait until coordinator tells Low task it can release mutex */
        WaitForSingleObject(ctx->low_can_release_event, 2000);

        /* Capture priority right before give */
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        ctx->low_observed_priority = uxTaskPriorityGet(self);

        /* Release mutex (this triggers priority restoration if PI enabled) */
        xSemaphoreGive(ctx->m);

        /* Capture priority right after give */
        ctx->low_restored_priority = uxTaskPriorityGet(self);
    }

    SetEvent(ctx->low_finished_event);
}

/* Task H: High priority worker */
static void mutex_test_high_task(void* pvParam) {
    MutexTestContext_t* ctx = (MutexTestContext_t*)pvParam;

    /* Wait until Low task holds the mutex */
    WaitForSingleObject(ctx->low_acquired_event, 2000);

    /* Signal that High task is about to attempt take */
    SetEvent(ctx->high_attempt_event);

    /* Attempt to take mutex with timeout */
    BaseType_t res = xSemaphoreTake(ctx->m, pdMS_TO_TICKS(120));
    ctx->high_take_succeeded = (res == pdPASS);

    if (res == pdPASS) {
        /* Release immediately if acquired */
        xSemaphoreGive(ctx->m);
    }

    SetEvent(ctx->high_finished_event);
}

static void test_mutex_basic_lock_unlock(void) {
    TEST_BEGIN("Mutex: standard take and give sanity check");

    MutexHandle_t m = xSemaphoreCreateMutex();
    TEST_ASSERT(m != NULL, "Mutex created successfully");
    TEST_ASSERT_EQUAL_INT(0, m->contentionCount, "Initial contention count is 0");

    BaseType_t take_res = xSemaphoreTake(m, 0);
    TEST_ASSERT(take_res == pdPASS, "xSemaphoreTake succeeds on unlocked mutex");

    BaseType_t give_res = xSemaphoreGive(m);
    TEST_ASSERT(give_res == pdPASS, "xSemaphoreGive releases mutex successfully");

    vSemaphoreDelete(m);
    TEST_END();
}

static void test_mutex_no_inheritance(void) {
    TEST_BEGIN("Mutex (No PI): contention tracked, owner priority NOT boosted");

    MutexTestContext_t ctx = {
        .m = xSemaphoreCreateMutex(), /* Standard mutex WITHOUT PI */
        .low_acquired_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .high_attempt_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .low_can_release_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .low_finished_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .high_finished_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .high_take_succeeded = false,
        .low_observed_priority = 0,
        .low_restored_priority = 0
    };
    TEST_ASSERT(ctx.m != NULL, "Standard mutex created");
    TEST_ASSERT(!ctx.m->priorityInheritanceEnabled, "PI flag is disabled");

    TaskHandle_t hLow = NULL;
    TaskHandle_t hHigh = NULL;

    xTaskCreate(mutex_test_low_task, "TestLow", 1024, &ctx, TASK_PRIORITY_LOW, &hLow);
    xTaskCreate(mutex_test_high_task, "TestHigh", 1024, &ctx, TASK_PRIORITY_ALERT, &hHigh);

    /* Wait for Low to acquire and High to attempt take */
    WaitForSingleObject(ctx.high_attempt_event, 1000);
    Sleep(30); /* Allow High task to enter xSemaphoreTake and block */

    /* Check Low task priority: MUST STILL BE TASK_PRIORITY_LOW (1), NO inheritance! */
    UBaseType_t pLowDuring = uxTaskPriorityGet(hLow);
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_LOW, pLowDuring, "Low task priority NOT boosted without PI");

    /* Allow High task to time out */
    WaitForSingleObject(ctx.high_finished_event, 1000);
    TEST_ASSERT(!ctx.high_take_succeeded, "High task timed out as expected");

    /* Now allow Low task to release mutex */
    SetEvent(ctx.low_can_release_event);
    WaitForSingleObject(ctx.low_finished_event, 1000);

    TEST_ASSERT(ctx.m->contentionCount >= 1, "Contention counter recorded high-priority conflict");
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_LOW, ctx.low_restored_priority, "Low task priority remained at base priority");

    WaitForSingleObject(hLow->thread, 1000);
    WaitForSingleObject(hHigh->thread, 1000);
    CloseHandle(hLow->thread);
    CloseHandle(hHigh->thread);
    free(hLow);
    free(hHigh);

    CloseHandle(ctx.low_acquired_event);
    CloseHandle(ctx.high_attempt_event);
    CloseHandle(ctx.low_can_release_event);
    CloseHandle(ctx.low_finished_event);
    CloseHandle(ctx.high_finished_event);
    vSemaphoreDelete(ctx.m);

    TEST_END();
}

static void test_mutex_priority_inheritance_boost_and_restore(void) {
    TEST_BEGIN("Mutex (With PI): owner priority boosted to high, restored on give");

    MutexTestContext_t ctx = {
        .m = xSemaphoreCreateMutexWithPI(), /* Mutex WITH PI */
        .low_acquired_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .high_attempt_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .low_can_release_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .low_finished_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .high_finished_event = CreateEvent(NULL, FALSE, FALSE, NULL),
        .high_take_succeeded = false,
        .low_observed_priority = 0,
        .low_restored_priority = 0
    };
    TEST_ASSERT(ctx.m != NULL, "PI Mutex created");
    TEST_ASSERT(ctx.m->priorityInheritanceEnabled, "PI flag is enabled");

    TaskHandle_t hLow = NULL;
    TaskHandle_t hHigh = NULL;

    xTaskCreate(mutex_test_low_task, "TestLowPI", 1024, &ctx, TASK_PRIORITY_LOW, &hLow);
    xTaskCreate(mutex_test_high_task, "TestHighPI", 1024, &ctx, TASK_PRIORITY_ALERT, &hHigh);

    /* Wait until Low acquired and High starts attempt */
    WaitForSingleObject(ctx.high_attempt_event, 1000);
    Sleep(30); /* Allow High task to enter xSemaphoreTake and contend */

    /* Verify Low task priority is BOOSTED to TASK_PRIORITY_ALERT (6) */
    UBaseType_t pLowBoosted = uxTaskPriorityGet(hLow);
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_ALERT, pLowBoosted, "Low task priority BOOSTED to High task priority (6)");
    TEST_ASSERT(ctx.m->contentionCount >= 1, "Contention counted on PI mutex");

    /* Allow Low task to release mutex */
    SetEvent(ctx.low_can_release_event);

    /* Wait for Low task to finish Give */
    WaitForSingleObject(ctx.low_finished_event, 1000);
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_ALERT, ctx.low_observed_priority, "Low task held boosted priority until release");
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_LOW, ctx.low_restored_priority, "Low task priority RESTORED to base priority (1)");

    /* High task should now acquire mutex without timing out */
    WaitForSingleObject(ctx.high_finished_event, 1000);
    TEST_ASSERT(ctx.high_take_succeeded, "High task successfully acquired mutex after release");

    WaitForSingleObject(hLow->thread, 1000);
    WaitForSingleObject(hHigh->thread, 1000);
    CloseHandle(hLow->thread);
    CloseHandle(hHigh->thread);
    free(hLow);
    free(hHigh);

    CloseHandle(ctx.low_acquired_event);
    CloseHandle(ctx.high_attempt_event);
    CloseHandle(ctx.low_can_release_event);
    CloseHandle(ctx.low_finished_event);
    CloseHandle(ctx.high_finished_event);
    vSemaphoreDelete(ctx.m);

    TEST_END();
}

typedef struct {
    MutexHandle_t m;
    HANDLE low_acquired;
    HANDLE med_start;
    HANDLE high_start;
    HANDLE low_release;
    HANDLE done;
} CascadeContext_t;

static void cascade_low_task(void* pvParam) {
    CascadeContext_t* ctx = (CascadeContext_t*)pvParam;
    xSemaphoreTake(ctx->m, pdMS_TO_TICKS(1000));
    SetEvent(ctx->low_acquired);

    WaitForSingleObject(ctx->low_release, 2000);
    xSemaphoreGive(ctx->m);
}

static void cascade_waiter_task(void* pvParam) {
    CascadeContext_t* ctx = (CascadeContext_t*)pvParam;
    xSemaphoreTake(ctx->m, pdMS_TO_TICKS(300));
    xSemaphoreGive(ctx->m);
}

static void test_mutex_multitask_cascade_boost(void) {
    TEST_BEGIN("Mutex (With PI): multi-task cascade boost (P1 -> P3 -> P6 -> P1)");

    CascadeContext_t ctx = {
        .m = xSemaphoreCreateMutexWithPI(),
        .low_acquired = CreateEvent(NULL, FALSE, FALSE, NULL),
        .med_start = CreateEvent(NULL, FALSE, FALSE, NULL),
        .high_start = CreateEvent(NULL, FALSE, FALSE, NULL),
        .low_release = CreateEvent(NULL, FALSE, FALSE, NULL),
        .done = CreateEvent(NULL, FALSE, FALSE, NULL)
    };

    TaskHandle_t hLow = NULL;
    TaskHandle_t hMed = NULL;
    TaskHandle_t hHigh = NULL;

    /* 1. Low task takes mutex */
    xTaskCreate(cascade_low_task, "CascLow", 1024, &ctx, TASK_PRIORITY_LOW, &hLow);
    WaitForSingleObject(ctx.low_acquired, 1000);
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_LOW, uxTaskPriorityGet(hLow), "Low task starts at P1");

    /* 2. Med task contends */
    xTaskCreate(cascade_waiter_task, "CascMed", 1024, &ctx, TASK_PRIORITY_NORMAL, &hMed);
    Sleep(30);
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_NORMAL, uxTaskPriorityGet(hLow), "Low task boosted P1 -> P3 by Med task");

    /* 3. High task contends */
    xTaskCreate(cascade_waiter_task, "CascHigh", 1024, &ctx, TASK_PRIORITY_ALERT, &hHigh);
    Sleep(30);
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_ALERT, uxTaskPriorityGet(hLow), "Low task boosted P3 -> P6 by High task");

    /* 4. Release mutex and verify drop to P1 */
    SetEvent(ctx.low_release);
    Sleep(30);
    TEST_ASSERT_EQUAL_INT(TASK_PRIORITY_LOW, uxTaskPriorityGet(hLow), "Low task restored directly to base priority P1");

    WaitForSingleObject(hLow->thread, 1000);
    WaitForSingleObject(hMed->thread, 1000);
    WaitForSingleObject(hHigh->thread, 1000);
    CloseHandle(hLow->thread);
    CloseHandle(hMed->thread);
    CloseHandle(hHigh->thread);
    free(hLow);
    free(hMed);
    free(hHigh);

    CloseHandle(ctx.low_acquired);
    CloseHandle(ctx.med_start);
    CloseHandle(ctx.high_start);
    CloseHandle(ctx.low_release);
    CloseHandle(ctx.done);
    vSemaphoreDelete(ctx.m);

    TEST_END();
}

/* =========================================================================
 * Main Runner
 * ========================================================================= */
int main(void) {
    /* Initialize FreeRTOS simulator runtime */
    vSimulatorInit();
    vSimulatorStart();

    printf("\n");
    printf(ANSI_CYAN_BOLD "======================================================================\n" ANSI_RESET);
    printf(ANSI_CYAN_BOLD " FREERTOS SIMULATOR - AUTOMATED UNIT TEST SUITE                       \n" ANSI_RESET);
    printf(ANSI_CYAN_BOLD " Target: Queue Empty/Full, Ring Buffer Wraparound, Mutex Inheritance  \n" ANSI_RESET);
    printf(ANSI_CYAN_BOLD "======================================================================\n\n" ANSI_RESET);

    printf(ANSI_BOLD "--- [GROUP 1: Queue Full and Empty Semantics] ---\n" ANSI_RESET);
    test_queue_empty_nonblocking();
    test_queue_empty_blocking_timeout();
    test_queue_full_nonblocking();
    test_queue_full_blocking_timeout();
    test_queue_cross_task_unblock_on_receive();
    test_queue_cross_task_unblock_on_send();
    printf("\n");

    printf(ANSI_BOLD "--- [GROUP 2: Ring-Buffer Wraparound & Integrity] ---\n" ANSI_RESET);
    test_ring_buffer_basic_wraparound();
    test_ring_buffer_multicycle_torture();
    test_ring_buffer_send_to_front_wraparound();
    test_ring_buffer_boundary_integrity_wrapped();
    printf("\n");

    printf(ANSI_BOLD "--- [GROUP 3: Mutex Priority Inheritance] ---\n" ANSI_RESET);
    test_mutex_basic_lock_unlock();
    test_mutex_no_inheritance();
    test_mutex_priority_inheritance_boost_and_restore();
    test_mutex_multitask_cascade_boost();
    printf("\n");

    printf(ANSI_CYAN_BOLD "======================================================================\n" ANSI_RESET);
    printf(ANSI_CYAN_BOLD " TEST EXECUTION SUMMARY                                               \n" ANSI_RESET);
    printf(ANSI_CYAN_BOLD "======================================================================\n" ANSI_RESET);
    printf("  Total Test Cases   : %d\n", s_test_count);
    printf("  Passed Test Cases  : %s%d%s\n",
           s_test_failures == 0 ? ANSI_GREEN_BOLD : ANSI_RED_BOLD,
           s_test_count - s_test_failures, ANSI_RESET);
    printf("  Failed Test Cases  : %s%d%s\n",
           s_test_failures == 0 ? ANSI_GREEN_BOLD : ANSI_RED_BOLD,
           s_test_failures, ANSI_RESET);
    printf("  Total Assertions   : %d\n", s_total_assertions);
    printf("  Passed Assertions  : %s%d%s\n", ANSI_GREEN_BOLD, s_passed_assertions, ANSI_RESET);
    printf("  Failed Assertions  : %s%d%s\n",
           s_failed_assertions == 0 ? ANSI_GREEN_BOLD : ANSI_RED_BOLD,
           s_failed_assertions, ANSI_RESET);

    if (s_test_failures == 0 && s_failed_assertions == 0) {
        printf(ANSI_GREEN_BOLD "\n  >>> RESULT: ALL UNIT TESTS PASSED (100%%) <<<\n\n" ANSI_RESET);
    } else {
        printf(ANSI_RED_BOLD "\n  >>> RESULT: UNIT TESTS FAILED <<<\n\n" ANSI_RESET);
    }

    vSimulatorStop();
    return (s_test_failures == 0) ? 0 : 1;
}
