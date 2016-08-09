#include <stdio.h>
#include "stopwatch.h"

void stopwatch_start(stopwatch_t *sw) {
#if defined(_WIN32)
    BOOL ret = TRUE;
    ret = ret && QueryPerformanceFrequency(&sw->freq);
    ret = ret && QueryPerformanceCounter(&sw->start);
    if (!ret) {
        fprintf(stderr, "Unable to query hig performance timer\n");
        exit(EXIT_FAILURE);
    }
#elif defined(__MACH__)
    clock_serv_t cclock;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &sw->ts_start);
    mach_port_deallocate(mach_task_self(), cclock);
#elif defined(__unix__)
    clock_gettime(CLOCK_REALTIME, &sw->ts_start);
#else
#error Unsupported platform
#endif
}

double stopwatch_elapsed(stopwatch_t *sw) {
#if defined(_WIN32)
    BOOL ret = TRUE;
    ret = ret && QueryPerformanceCounter(&sw->end);
    if (!ret) {
        fprintf(stderr, "Unable to query hig performance timer\n");
        exit(EXIT_FAILURE);
    }
    return (sw->end.QuadPart - sw->start.QuadPart) / (1.0 * sw->freq.QuadPart);
#elif defined(__MACH__)
    mach_timespec_t ts_end;
    clock_serv_t cclock;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &ts_end);
    mach_port_deallocate(mach_task_self(), cclock);
    double elapsed = (ts_end.tv_sec - sw->ts_start.tv_sec);
    elapsed += (ts_end.tv_nsec - sw->ts_start.tv_nsec) / 1.0e9;
    return elapsed;
#elif defined(__unix__)
    struct timespec ts_end;
    clock_gettime(CLOCK_REALTIME, &ts_end);
    double elapsed = (ts_end.tv_sec - sw->ts_start.tv_sec);
    elapsed += (ts_end.tv_nsec - sw->ts_start.tv_nsec) / 1.0e9;
    return elapsed;
#else
#error Unsupported platform
#endif
}
