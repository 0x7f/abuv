#pragma once

#if defined(_WIN32)
#include <Windows.h>
#endif

#if defined(__MACH__)
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#if defined(__unix__)
#include <time.h>
#include <sys/time.h>
#endif

struct stopwatch_s {
#if defined(_WIN32)
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
#elif defined(__MACH__)
    mach_timespec_t ts_start;
#elif defined(__unix__)
    struct timespec ts_start;
#else
#error Unsupported platform
#endif
};

typedef struct stopwatch_s stopwatch_t;
void stopwatch_start(stopwatch_t *sw);
double stopwatch_elapsed(stopwatch_t *sw);
