/* SPDX-License-Identifier: GPL-2.0-only */
/* Monitor the M6 battery and take over the early boot watchdog. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int in_range(const char *text, int low, int high)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    return end != text && !*end && !errno && value >= low && value <= high;
}

static const char *check_battery(int battery)
{
    int fd = openat(battery, "uevent", O_RDONLY);
    char buf[4096];
    if (fd < 0) return "missing battery";
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0 || n == sizeof(buf) - 1) return "incomplete battery data";
    buf[n] = 0;
    unsigned int present = 0;
    char *next = buf, *line;
    while ((line = strsep(&next, "\n"))) {
        if (!strncmp(line, "POWER_SUPPLY_PRESENT=", 21)) {
            if (strcmp(line + 21, "1")) return "battery absent";
            present |= 1;
        } else if (!strncmp(line, "POWER_SUPPLY_HEALTH=", 20)) {
            if (strcmp(line + 20, "Good")) return "battery health";
            present |= 2;
        } else if (!strncmp(line, "POWER_SUPPLY_TEMP=", 18)) {
            if (!in_range(line + 18, 100, 419)) return "temperature";
            present |= 4;
        } else if (!strncmp(line, "POWER_SUPPLY_CAPACITY=", 22)) {
            if (!in_range(line + 22, 30, 100)) return "charge level";
            present |= 8;
        }
    }
    return present == 15 ? NULL : "missing battery data";
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--check-battery")) {
        int fd = open(argv[2], O_RDONLY | O_DIRECTORY);
        return check_battery(fd) != NULL;
    }
    if (argc != 1) return 2;
    int battery = open("/sys/class/power_supply/battery", O_RDONLY | O_DIRECTORY);
    int log = open("/dev/kmsg", O_WRONLY);
    int wd = open("/dev/watchdog0", O_WRONLY);
    int seconds = 120;
    struct watchdog_info info = {0};
    if (battery < 0 || log < 0 || wd < 0 || ioctl(wd, WDIOC_GETSUPPORT, &info) ||
        strcmp((char *)info.identity, "qcom_wdt") || check_battery(battery) ||
        ioctl(wd, WDIOC_SETTIMEOUT, &seconds) || seconds != 120 ||
        ioctl(wd, WDIOC_KEEPALIVE, 0)) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (child != 0) return 0;
    dprintf(log, "<5>v500-manual: session active, no recovery deadline\n");
    for (;;) {
        sleep(5);
        const char *reason = check_battery(battery);
        if (!reason && !ioctl(wd, WDIOC_KEEPALIVE, 0)) continue;
        dprintf(log, "<3>v500-manual: returning to recovery: %s\n", reason ? reason : "watchdog");
        sync();
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, "recovery");
        for (;;) pause();
    }
}
