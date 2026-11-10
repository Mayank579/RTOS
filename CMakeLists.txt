cmake_minimum_required(VERSION 3.16)
project(rtos_simulator C)

# ============================================================
# Compiler Settings
# Target: MinGW gcc on Windows, C11 standard
# ============================================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Enable all warnings — good practice for embedded C code
add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    -O2
)

# ============================================================
# Source Files
# ============================================================
set(SOURCES
    src/main.c
    src/freertos_sim.c
    src/sensor_task.c
    src/processing_task.c
    src/logging_task.c
    src/alert_task.c
    src/priority_inversion_demo.c
)

# ============================================================
# Executable
# ============================================================
add_executable(rtos_scheduler ${SOURCES})

# ============================================================
# Include Directories
# ============================================================
target_include_directories(rtos_scheduler PRIVATE src)

# ============================================================
# Link Libraries
# Windows system libraries needed:
#   winmm  — Windows Multimedia (high-resolution timers)
#   kernel32 — CreateThread, WaitForSingleObject, etc.
#   msvcrt — Microsoft C runtime (printf, malloc, etc.)
# math library for sinf() in sensor simulation
# ============================================================
target_link_libraries(rtos_scheduler
    winmm
    kernel32
    m
)

# ============================================================
# Output binary name
# ============================================================
set_target_properties(rtos_scheduler PROPERTIES
    OUTPUT_NAME "rtos_scheduler"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ============================================================
# Unit Tests Executable
# ============================================================
add_executable(test_runner
    tests/unit_tests.c
    src/freertos_sim.c
)

target_include_directories(test_runner PRIVATE src)

target_link_libraries(test_runner
    winmm
    kernel32
    m
)

set_target_properties(test_runner PROPERTIES
    OUTPUT_NAME "test_runner"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

enable_testing()
add_test(NAME RTOS_Unit_Tests COMMAND test_runner)

message(STATUS "Configured RTOS Simulator")
message(STATUS "  Sources: ${SOURCES}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
