# Porting Guide

microres compiles on any platform with a C99 compiler. You provide two
callbacks — `clock_fn` and `sleep_fn` — and everything else is portable.

---

## Files to include

```
include/mres.h    →  add to your include path
src/mres.c        →  add to your build
```

---

## Platform recipes

### ESP32 (ESP-IDF / FreeRTOS)

```c
#include "mres.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t platform_clock(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void platform_sleep(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
```

**CMakeLists.txt:**
```cmake
idf_component_register(
    SRCS "mres.c"
    INCLUDE_DIRS "include"
)
```

### STM32 (HAL)

```c
#include "mres.h"
#include "stm32f4xx_hal.h"

static uint32_t platform_clock(void) { return HAL_GetTick(); }
static void platform_sleep(uint32_t ms) { HAL_Delay(ms); }
```

**Makefile:**
```makefile
C_SOURCES += lib/microres/src/mres.c
C_INCLUDES += -Ilib/microres/include
```

**ISR note:** Never call `mres_retry_exec` or `mres_breaker_call` from
an ISR. If events come from interrupts, post them to a queue and process
in the main loop.

### Zephyr RTOS

```c
#include "mres.h"
#include <zephyr/kernel.h>

static uint32_t platform_clock(void) { return k_uptime_get_32(); }
static void platform_sleep(uint32_t ms) { k_msleep(ms); }
```

### Arduino

```cpp
extern "C" {
    #include "mres.h"
}

static uint32_t platform_clock(void) { return millis(); }
static void platform_sleep(uint32_t ms) { delay(ms); }
```

### Linux / POSIX

```c
#include "mres.h"
#include <time.h>
#include <unistd.h>

static uint32_t platform_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void platform_sleep(uint32_t ms) {
    usleep(ms * 1000);
}
```

### Windows

```c
#include "mres.h"
#include <windows.h>

static uint32_t platform_clock(void) { return GetTickCount(); }
static void platform_sleep(uint32_t ms) { Sleep(ms); }
```

---

## Non-blocking usage

On bare-metal super-loops where you cannot afford to block in `sleep_fn`,
pass `NULL` as the sleep parameter:

```c
/* All attempts execute immediately — no blocking */
mres_retry_exec(&retry, my_op, &ctx, hal_clock, NULL);
```

For timer-driven retry, use `mres_delay_calc()` to get the delay, then
set a hardware timer and retry on the next timer interrupt:

```c
uint32_t delay = mres_delay_calc(policy, attempt, hal_clock);
start_timer(delay);  /* fires callback when elapsed */
```

---

## CMake integration

```cmake
add_library(microres STATIC lib/microres/src/mres.c)
target_include_directories(microres PUBLIC lib/microres/include)

target_link_libraries(my_app PRIVATE microres)
```

---

## Checklist for a new platform

1. **C99 compiler?** → good to go.
2. **Provide `clock_fn`** → any monotonic millisecond counter.
3. **Provide `sleep_fn`** → any blocking delay, or NULL for non-blocking.
4. **Events from ISR?** → don't call microres from ISR. Buffer and process
   in main loop / task.
5. **Multiple threads?** → protect shared instances with a mutex.
6. **Memory-constrained?** → disable jitter with `MRES_ENABLE_JITTER 0`.
