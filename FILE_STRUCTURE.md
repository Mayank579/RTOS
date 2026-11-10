# Project File Structure

A complete directory and architectural map of the **FreeRTOS Multi-Task Simulator** codebase.

---

## 1. Directory Tree Overview

```
D:\RTOS\
├── build.bat                       — One-click build, run, and test script (Windows)
├── CMakeLists.txt                  — Cross-platform CMake build configuration
├── README.md                       — High-level project documentation & concept guide
├── FILE_STRUCTURE.md               — This file: Complete codebase directory & module index
│
├── src\                            — All C/C++ source and header files
│   ├── freertos_sim.h              — FreeRTOS API definition (Task, Queue, Sem, Mutex)
│   ├── freertos_sim.c              — Win32 backend implementation of FreeRTOS API
│   │
│   ├── sensor_task.h               — Periodic Sensor Task interface & telemetry structs
│   ├── sensor_task.c               — Sensor telemetry generator (P5, 500ms period)
│   │
│   ├── processing_task.h           — Signal Processing Task interface & DSP structures
│   ├── processing_task.c           — Moving average filter & threshold anomaly detection (P4)
│   │
│   ├── logging_task.h              — Logging Task interface
│   ├── logging_task.c              — Best-effort serialized console logger (P3)
│   │
│   ├── alert_task.h                — Reactive Alert Task interface
│   ├── alert_task.c                — High-priority anomaly handler with animation (P6)
│   │
│   ├── priority_inversion_demo.h   — Priority Inversion test harness interface
│   ├── priority_inversion_demo.c   — Mars Pathfinder Priority Inversion bug & PI fix demo
│   │
│   └── main.c                      — Program entry point, RTOS object creation, 3 phases
│
├── tests\                          — Automated unit test suite
│   └── unit_tests.c                — Comprehensive unit tests for Queue empty/full, Ring-Buffer wraparound, Mutex PI
│
└── build\                          — Compilation artifacts directory
    ├── rtos_scheduler.exe          — Main compiled executable
    ├── test_runner.exe             — Compiled automated unit test runner
    ├── output.txt                  — Saved execution log from simulator run
    └── err.txt                     — Build error output log
```

---

## 2. File-by-File Detailed Guide

### Root Build & Configuration

| File | Purpose | Key Details |
| :--- | :--- | :--- |
| `build.bat` | Windows compilation script | Auto-detects GCC (MinGW-w64/MSYS2) or CMake; supports `build.bat`, `build.bat clean`, and `build.bat run`. |
| `CMakeLists.txt` | CMake configuration | Configures C11 standard, includes `src/`, links `-lwinmm -lkernel32 -lm`. |
| `README.md` | Primary documentation | Overview of FreeRTOS concepts, tasks, priorities, build instructions, and interview talking points. |
| `FILE_STRUCTURE.md` | Codebase architecture map | Complete file inventory, module interactions, and dependency topology. |

---

### Core RTOS Emulation Layer (`src/`)

| File | Role | Key Components |
| :--- | :--- | :--- |
| `freertos_sim.h` | FreeRTOS API Header | Defines `TaskHandle_t`, `QueueHandle_t`, `SemaphoreHandle_t`, priorities `1`–`7`, `xTaskCreate`, `vTaskDelay`, `xQueueSend/Receive`, `xSemaphoreTake/Give`, ANSI color macros. |
| `freertos_sim.c` | Win32 Engine Implementation | Maps FreeRTOS API to Windows primitives (`CreateThread`, `Events`, `CRITICAL_SECTION`, `QueryPerformanceCounter`); implements single-core CPU affinity, ring-buffer queues, and Priority Inheritance tracking. |

---

### Pipeline Application Tasks (`src/`)

| Module | Priority | Role & Inter-Process Communication (IPC) |
| :--- | :--- | :--- |
| `sensor_task.h/.c` | **P5** (High) | Periodic sampling task (`vTaskDelayUntil(500ms)`). Generates synthetic temperature, pressure, and humidity telemetry; accesses `xCalibrationMutex`; copies packets into `xSensorQueue`. |
| `processing_task.h/.c` | **P4** (Above Normal) | Pipeline consumer/producer. Blocks on `xSensorQueue`; applies a 5-sample circular moving average filter; updates `xStatsMutex`; signals `xAlertSemaphore` if threshold exceeded; pushes to `xProcessedQueue`. |
| `logging_task.h/.c` | **P3** (Normal) | Best-effort consumer. Blocks on `xProcessedQueue`; acquires `xLoggingMutex`; formats and displays color-coded console telemetry. |
| `alert_task.h/.c` | **P6** (Reactive) | Highest regular priority. Blocks on binary semaphore `xAlertSemaphore`; instantly preempts processing and logging tasks to render a critical anomaly visual alarm. |

---

### Priority Inversion Benchmark (`src/`)

| File | Role | Key Components |
| :--- | :--- | :--- |
| `priority_inversion_demo.h` | Demo Header | Declarations for `vDemoLowTask`, `vDemoHighTask`, `vDemoMediumTask`, and `run_priority_inversion_demo()`. |
| `priority_inversion_demo.c` | Benchmark Engine | Recreates the 1997 Mars Pathfinder incident: compares Scenario 1 (unbounded priority inversion bug using standard mutex) against Scenario 2 (automatic priority promotion using Priority Inheritance mutex). |

---

### Main Entry Point (`src/`)

| File | Role | Execution Flow |
| :--- | :--- | :--- |
| `main.c` | Application Lifecycle | Initializes simulator $\rightarrow$ creates shared queues, semaphores, and mutexes $\rightarrow$ executes **Phase 1** (10s telemetry pipeline) $\rightarrow$ executes **Phase 2** (Priority Inversion benchmark) $\rightarrow$ executes **Phase 3** (summary statistics report). |

---

### Automated Unit Test Suite (`tests/`)

| File | Role | Test Cases Covered |
| :--- | :--- | :--- |
| `unit_tests.c` | Test Runner & Suite | **14 test cases / 3,118 assertions (100% pass rate)**:<br>• **Queue Full & Empty**: non-blocking empty fail, empty blocking timeout, non-blocking full fail, full blocking timeout, cross-task receive unblocking, cross-task send unblocking.<br>• **Ring-Buffer Wraparound**: circular head/tail wraparound, 3,000-item non-power-of-two torture test, `xQueueSendToFront` reverse wraparound, boundary integrity protection.<br>• **Mutex Priority Inheritance**: basic lock/unlock, standard mutex contention without PI (no boost), PI mutex boost (P1 $\rightarrow$ P6) & restoration on give, multi-task cascade boost (P1 $\rightarrow$ P3 $\rightarrow$ P6 $\rightarrow$ P1). |

---

## 3. Dataflow & Module Dependency Diagram

```
                 ┌──────────────────┐
                 │     main.c       │
                 └────────┬─────────┘
                          │ (Spawns & Orchestrates)
       ┌──────────────────┼─────────────────────────┐
       ▼                  ▼                         ▼
┌──────────────┐   ┌──────────────┐          ┌──────────────────────────┐
│ Phase 1:     │   │ Phase 2:     │          │ Phase 3:                 │
│ Sensor Tasks │   │ PI Benchmark │          │ System Statistics        │
└──────┬───────┘   └──────┬───────┘          └──────────────────────────┘
       │                  │
       │                  ▼
       │         [priority_inversion_demo.c]
       │           - DemoLOW  (P1)
       │           - DemoMED  (P3)
       │           - DemoHIGH (P7)
       ▼
 ┌─────────────┐   xSensorQueue   ┌─────────────────┐   xProcessedQueue   ┌──────────────┐
 │ SensorTask  │ ───────────────► │ ProcessingTask  │ ──────────────────► │ LoggingTask  │
 │ (P5, 500ms) │                  │ (P4, Filter/DSP)│                     │ (P3, Output) │
 └─────────────┘                  └────────┬────────┘                     └──────────────┘
                                           │
                                           │ xAlertSemaphore (Binary)
                                           ▼
                                  ┌─────────────────┐
                                  │   AlertTask     │
                                  │   (P6, Reactive)│
                                  └─────────────────┘
                                           │
                                           ▼
                             [freertos_sim.h / freertos_sim.c]
                             - Win32 Threads & Priority Levels
                             - Ring-Buffer Queues
                             - Priority Inheritance Engine
```

---

## 4. Module Size & Metrics

| File | Lines of Code | Primary Language | Description |
| :--- | :---: | :---: | :--- |
| `src/freertos_sim.c` | ~700 | C11 | Win32 kernel simulator & synchronization |
| `src/freertos_sim.h` | 367 | C11 | FreeRTOS-compatible API definitions |
| `src/priority_inversion_demo.c` | ~450 | C11 | Priority inversion benchmark suite |
| `src/priority_inversion_demo.h` | 51 | C11 | Demo task prototypes |
| `src/main.c` | ~350 | C11 | Entry point and phase controller |
| `src/processing_task.c` | 219 | C11 | Circular moving average & anomaly logic |
| `src/processing_task.h` | 74 | C11 | Processed data packet & statistics types |
| `src/sensor_task.c` | 192 | C11 | ADC & sensor simulation logic |
| `src/sensor_task.h` | 71 | C11 | Raw sensor data structures |
| `src/alert_task.c` | 143 | C11 | Reactive alert and ASCII animation |
| `src/alert_task.h` | 25 | C11 | Alert task header |
| `src/logging_task.c` | 133 | C11 | Telemetry log formatter |
| `src/logging_task.h` | 25 | C11 | Logger header |
| `build.bat` | 97 | Windows Batch | Clean, build, and run automation |
| `CMakeLists.txt` | 50 | CMake | Cross-platform build target |
