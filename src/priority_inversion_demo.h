/*
 * priority_inversion_demo.h - Priority Inversion Demo Interface
 *
 * FreeRTOS Concept: Priority Inversion is one of the most critical RTOS
 * bugs. It caused the Mars Pathfinder mission to reset repeatedly in 1997!
 *
 * The Problem:
 *   - LOW priority task holds a mutex
 *   - HIGH priority task needs the same mutex → blocks
 *   - MEDIUM priority task (CPU-bound) preempts LOW task
 *   - HIGH task is starved even though it's highest priority!
 *   → System appears "broken" — high priority tasks don't run
 *
 * The Fix: Priority Inheritance
 *   - When HIGH task blocks on mutex held by LOW task,
 *     LOW task's priority is TEMPORARILY RAISED to HIGH
 *   - MEDIUM task can no longer preempt LOW task (same priority)
 *   - LOW task finishes quickly, releases mutex
 *   - HIGH task runs with minimal delay
 */

#ifndef PRIORITY_INVERSION_DEMO_H
#define PRIORITY_INVERSION_DEMO_H

#include "freertos_sim.h"

/* Demo control structure passed to each demo task */
typedef struct {
    bool  use_pi_mutex;     /* true = use Priority Inheritance mutex */
    int   scenario;         /* Which demo scenario (BUG=0, FIX=1) */
} DemoConfig_t;

/* Shared resource mutex handles — created in demo, used by demo tasks */
extern SemaphoreHandle_t xSharedResourceMutex;    /* Plain mutex (shows BUG) */
extern SemaphoreHandle_t xSharedResourcePIMutex;  /* PI mutex (shows FIX) */

/* Demo synchronization */
extern HANDLE hDemoLowStarted;    /* LOW task signals it has the mutex */
extern HANDLE hDemoHighGo;        /* Main signals HIGH task to try mutex */
extern HANDLE hDemoMedGo;         /* Main signals MEDIUM task to start */
extern HANDLE hDemoComplete;      /* Signals demo scenario is done */

/* Entry point */
void run_priority_inversion_demo(void);

/* Internal task functions */
void vDemoLowTask   (void* pvParameters);
void vDemoHighTask  (void* pvParameters);
void vDemoMediumTask(void* pvParameters);

#endif /* PRIORITY_INVERSION_DEMO_H */
