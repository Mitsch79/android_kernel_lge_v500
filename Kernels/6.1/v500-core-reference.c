/* Identical bounded integer workload for LG 3.4 and the V500 6.1 port. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t ns(clockid_t clock)
{
    struct timespec t;
    if (clock_gettime(clock, &t)) _exit(10);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static uint32_t __attribute__((noinline)) compute(uint32_t x, unsigned count)
{
    while (count--) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    return x;
}

static long read_frequency(int cpu)
{
    char path[120];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    FILE *f = fopen(path, "r");
    long khz = -1;
    if (f) {
        if (fscanf(f, "%ld", &khz) != 1) khz = -1;
        fclose(f);
    }
    return khz;
}

static int probe(int cpu, int pass)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    uint64_t deadline = ns(CLOCK_MONOTONIC) + 6000000000ULL;
    volatile uint32_t warm = 7;
    while (sched_setaffinity(0, sizeof(mask), &mask)) {
        if (errno != EINVAL || ns(CLOCK_MONOTONIC) >= deadline) {
            dprintf(1, "unavailable cpu=%d pass=%d errno=%d\n", cpu, pass, errno);
            return 1;
        }
        warm = compute(warm, 500000);
    }
    struct perf_event_attr attr = {
        .type = PERF_TYPE_HARDWARE, .size = sizeof(attr),
        .config = PERF_COUNT_HW_CPU_CYCLES,
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING
    };
    int fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    uint64_t start[3] = {0}, end[3] = {0};
    long first_khz = read_frequency(cpu);
    int first_cpu = sched_getcpu();
    int have_counter = fd >= 0 && read(fd, start, sizeof(start)) == sizeof(start);
    uint64_t wall0 = ns(CLOCK_MONOTONIC), cpu0 = ns(CLOCK_THREAD_CPUTIME_ID);
    uint32_t seed = 12345;
    /* Keep the pure integer loop between the two clock reads under optimization. */
    __asm__ volatile ("" : "+r"(seed) : : "memory");
    uint32_t result = compute(seed, 64000000);
    __asm__ volatile ("" : "+r"(result) : : "memory");
    uint64_t cpu1 = ns(CLOCK_THREAD_CPUTIME_ID), wall1 = ns(CLOCK_MONOTONIC);
    have_counter = have_counter && read(fd, end, sizeof(end)) == sizeof(end);
    if (fd >= 0) close(fd);
    cpu_set_t after;
    int valid = sched_getaffinity(0, sizeof(after), &after) == 0 &&
                CPU_EQUAL(&mask, &after) && first_cpu == cpu && sched_getcpu() == cpu;
    dprintf(1, "sample cpu=%d pass=%d pinned=%d result=%08x cpu_ns=%" PRIu64
        " wall_ns=%" PRIu64 " cycles=%" PRIu64 " enabled_ns=%" PRIu64
        " running_ns=%" PRIu64 " pmu=%d before_kHz=%ld after_kHz=%ld\n",
        cpu, pass, valid, result, cpu1-cpu0, wall1-wall0, end[0]-start[0],
        end[1]-start[1], end[2]-start[2], have_counter, first_khz, read_frequency(cpu));
    return !valid || result != 0x04ad20c5 || cpu1-cpu0 < 10000000;
}

int main(void)
{
    int failure = 0;
    for (int pass = 0; pass <= 3; pass++) {
        int children = 0;
        for (int cpu = 0; cpu < 4; cpu++) {
            pid_t child = fork();
            if (!child) _exit(probe(cpu, pass));
            if (child < 0) return 2;
            children++;
        }
        while (children--) {
            int status;
            if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
                if (pass) failure = 1;
            }
        }
    }
    return failure;
}
