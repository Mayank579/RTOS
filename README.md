# FreeRTOS Multi-Task Simulator

A comprehensive **FreeRTOS-compatible multi-task application** implemented using Win32 threads as the backend, designed to demonstrate real-world RTOS concepts for technical interviews and embedded systems education.

---

## Overview

This project simulates a **sensor data acquisition and processing pipeline** using FreeRTOS-style APIs (tasks, queues, semaphores, mutexes) running natively on Windows via MinGW/GCC. No real FreeRTOS kernel is needed — the same API surface is implemented using Win32 primitives.

---

## Project Structure

```
d:\RTOS\
├── CMakeLists.txt              — CMake build configuration
├── build.bat                   — One-click build script
├── README.md                   — This file
└── src/
    ├── freertos_sim.h/.c       — FreeRTOS-compatible API (Win32 backend)
    ├── sensor_task.h/.c        — Sensor acquisition task (Priority 5)
    ├── processing_task.h/.c    — Signal processing task (Priority 4)
    ├── logging_task.h/.c       — Logging task (Priority 3)
    ├── alert_task.h/.c         — Alert handler task (Priority 6)
    ├── priority_inversion_demo.h/.c — Priority inversion bug + fix
    └── main.c                  — Entry point, task creation, statistics
```

---

## Build Requirements

| Tool | Version | Download |
|------|---------|----------|
| MinGW-w64 (gcc) | 8.0+ | [winlibs.com](https://winlibs.com) or [MSYS2](https://msys2.org) |
| CMake | 3.16+ | [cmake.org](https://cmake.org/download/) |

Ensure `gcc`, `mingw32-make`, and `cmake` are in your `PATH`.

---

## Build & Run

```bat
REM Standard build
build.bat

REM Build and immediately run
build.bat run

REM Clean rebuild
build.bat clean run
```

Or manually:

```bat
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
cd ..
build\rtos_scheduler.exe
```

---

## FreeRTOS Concepts Demonstrated

### 1. Tasks & Preemptive Scheduling

```
Priority 6 — AlertTask      (preempts everything when alert fires)
Priority 5 — SensorTask     (runs every 500ms, never starved)
Priority 4 — ProcessingTask (runs when sensor data arrives)
Priority 3 — LoggingTask    (lowest, can be deferred under load)
```

Each task is an independent thread of execution. Higher-priority tasks **immediately preempt** lower ones — no polling needed.

### 2. Queues (Inter-Task Communication)

```
SensorTask → xSensorQueue → ProcessingTask → xProcessedQueue → LoggingTask
```

FreeRTOS queues **copy data by value** — no shared pointers, no races. If a queue is full, the sender blocks (or times out). If empty, the receiver blocks.

### 3. Binary Semaphore (Event Signaling)

```
ProcessingTask ──xSemaphoreGive──► xAlertSemaphore ──xSemaphoreTake──► AlertTask
                (anomaly detected)                   (wakes immediately)
```

The alert task sleeps (blocked state, 0 CPU) until the processing task signals it. This is the standard **ISR→task notification** pattern.

### 4. Mutexes (Mutual Exclusion)

| Mutex | Protects | Users |
|-------|----------|-------|
| `xCalibrationMutex` | `g_calibration` struct | SensorTask, (calibration task) |
| `xStatsMutex` | `g_processingStats` | ProcessingTask, main() |
| `xLoggingMutex` | Console output | LoggingTask |

### 5. Priority Inversion — The Most Important Demo

#### The Bug (without Priority Inheritance):

```
t=  0ms  LOW  task (P1) acquires shared_resource_mutex
t= 10ms  HIGH task (P7) needs mutex → BLOCKS
t= 20ms  MED  task (P3) becomes runnable
         MED (P3 > P1) PREEMPTS LOW ← Bug starts here!
t=120ms  MED  task finishes 100ms of CPU work
t=130ms  LOW  task resumes, releases mutex (held 130ms!)
t=131ms  HIGH task finally runs (waited 121ms!)
```

**Result:** HIGH priority task (P7) was starved by MEDIUM priority task (P3)! Priority was **inverted**: effective P3 > P7. This is what crashed the Mars Pathfinder rover in 1997.

#### The Fix (with Priority Inheritance):

```
t=  0ms  LOW  task (P1) acquires PI_mutex
t= 10ms  HIGH task (P7) needs PI_mutex → LOW BOOSTED to P7!
t= 20ms  MED  task (P3) tries to preempt LOW
         CANNOT — LOW now runs at P7!
t= 30ms  LOW  task releases PI_mutex (held only 30ms!)
         LOW restored to P1
t= 31ms  HIGH task acquires mutex immediately
```

**Result:** HIGH task waited only ~21ms (vs 121ms). Priority inheritance **bounded** the inversion window to the critical section duration only.

---

## Task State Machine

```
         xTaskCreate()
              │
              ▼
         ┌─────────┐
         │  READY  │◄──────────────────────────────┐
         └────┬────┘                               │
              │ Scheduler selects highest          │ Delay expires /
              │ priority READY task                │ Queue data arrives /
              ▼                                    │ Semaphore given
         ┌─────────┐    vTaskDelay()          ┌────┴──────┐
         │ RUNNING │──────────────────────────►  BLOCKED  │
         └────┬────┘    xQueueReceive()        └───────────┘
              │         xSemaphoreTake()
              │ vTaskDelete()
              ▼
         ┌─────────┐
         │ DELETED │
         └─────────┘
```

---

## Color-Coded Output Guide

| Color | Task / Meaning |
|-------|---------------|
| 🟢 Green | SensorTask — normal readings |
| 🔵 Cyan | ProcessingTask — filtered data |
| 🟣 Magenta | LoggingTask — log entries |
| 🔴 Red | AlertTask — anomaly alerts |
| 🟡 Yellow | Warnings (queue full, priority inheritance) |
| ⚪ White | Priority inversion demo output |

---

## Architecture Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                    RTOS TASK PIPELINE                        │
│                                                              │
│  ┌─────────────┐  xSensorQueue   ┌──────────────────┐       │
│  │ SensorTask  │─────────────────►  ProcessingTask  │       │
│  │   (P5)      │  (10 items)     │      (P4)        │       │
│  │             │                 │                  │       │
│  │ Every 500ms │                 │ Moving avg filter│       │
│  │ Reads:      │                 │ Anomaly detect   │       │
│  │  - Temp     │                 │                  │       │
│  │  - Pressure │                 └──────┬───────────┘       │
│  │  - Humidity │                        │                   │
│  └─────────────┘                        │ xProcessedQueue   │
│         │                               │ (10 items)        │
│  xCalib │Mutex                          │                   │
│  (shared│calibration)         ┌─────────▼──────────┐        │
│         │                     │   LoggingTask       │       │
│  ┌──────▼──────┐              │       (P3)          │       │
│  │ g_calib     │              │                     │       │
│  │ (protected) │              │ Color-coded output  │       │
│  └─────────────┘              └─────────────────────┘       │
│                                                               │
│  ┌─────────────┐              ┌─────────────────────┐       │
│  │  AlertTask  │◄─────────────│ xAlertSemaphore     │       │
│  │    (P6)     │  (binary sem)│ (Given on anomaly)  │       │
│  │             │              └─────────────────────┘       │
│  │ Flash alert │                                             │
│  │ ASCII art   │                                             │
│  └─────────────┘                                             │
└──────────────────────────────────────────────────────────────┘
```

---

## Interview Talking Points

1. **Why use an RTOS vs. superloop?**
   - Deterministic response times via priorities
   - Blocked tasks consume zero CPU (no busy-waiting)
   - Clean task isolation — private stack, no shared state issues

2. **What is priority inversion and why does it matter?**
   - Crashed the Mars Pathfinder rover in 1997
   - HIGH priority task starved by MEDIUM via shared mutex
   - Fix: priority inheritance (FreeRTOS mutexes have this by default)

3. **Queue vs. shared memory — which is better for IPC?**
   - Queue: thread-safe by design, copies data, decouples producer/consumer
   - Shared memory: requires manual mutex, prone to races, tight coupling
   - FreeRTOS recommendation: prefer queues for IPC

4. **Binary semaphore vs. mutex — what's the difference?**
   - Semaphore: no ownership, used for SIGNALING (ISR→task)
   - Mutex: has ownership, used for MUTUAL EXCLUSION
   - Mutex has priority inheritance; binary semaphore does NOT
   - Never use a semaphore as a mutex — no priority inheritance!

5. **vTaskDelay vs. vTaskDelayUntil — when to use each?**
   - `vTaskDelay(100)`: delay 100ms FROM NOW (can drift over time)
   - `vTaskDelayUntil(100)`: delay until absolute tick (no drift)
   - Use `vTaskDelayUntil` for precise periodic tasks (control loops, sensors)

---

## License

MIT — Free to use for educational and interview purposes.
