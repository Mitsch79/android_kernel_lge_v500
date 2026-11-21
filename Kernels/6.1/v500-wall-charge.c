/* SPDX-License-Identifier: GPL-2.0-only */
/* V500 M6 initramfs logger. All resources survive Android's root switch. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef V500_HOLD_TO_TEN
#define V500_HOLD_TO_TEN 0
#endif
#ifndef V500_FULL_CYCLE
#define V500_FULL_CYCLE 0
#endif

static long uptime_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts)) return -1;
    return ts.tv_sec;
}

static bool read_text(int dir, const char *path, char *buf, size_t size)
{
    int fd = openat(dir, path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0 || (size_t)n == size - 1) return false;
    buf[n] = 0;
    if (buf[n - 1] == '\n') buf[n - 1] = 0;
    return true;
}

/* uevent is one key per line. Keep values such as "Not charging" intact. */
static char *value(char *lines, const char *field)
{
    size_t len = strlen(field);
    for (char *s = lines; *s; s += strlen(s) + 1)
        if (!strncmp(s, field, len) && s[len] == '=') return s + len + 1;
    return "";
}

static void split_lines(char *buf)
{
    for (; *buf; buf++) if (*buf == '\n') *buf = 0;
}

static bool number(const char *s, long min, long max)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    return !errno && end != s && !*end && v >= min && v <= max;
}

struct sample {
    char battery[2048], usb[1024];
    const char *present, *health, *temp, *uv, *ua, *counter, *level, *status;
    const char *online, *limit, *type;
};

static bool decode(struct sample *s)
{
    split_lines(s->battery);
    split_lines(s->usb);
#define BAT(key) value(s->battery, "POWER_SUPPLY_" key)
#define USB(key) value(s->usb, "POWER_SUPPLY_" key)
    s->present = BAT("PRESENT"); s->health = BAT("HEALTH"); s->temp = BAT("TEMP");
    s->uv = BAT("VOLTAGE_NOW"); s->ua = BAT("CURRENT_NOW"); s->counter = BAT("CHARGE_COUNTER");
    s->level = BAT("CAPACITY"); s->status = BAT("STATUS");
    s->online = USB("ONLINE"); s->limit = USB("CURRENT_MAX");
    char *type = strchr(USB("USB_TYPE"), '[');
    char *end = type ? strchr(type + 1, ']') : NULL;
    s->type = "";
    if (end) { *end = 0; s->type = type + 1; }
    return number(s->present, 0, 1) && *s->health && *s->status &&
        number(s->temp, -1000, 1500) && number(s->level, 0, 100) &&
        number(s->uv, 1, 10000000) && number(s->ua, -10000000, 10000000) &&
        number(s->counter, -10000000, 10000000) && number(s->online, 0, 1) &&
        number(s->limit, 0, 500000) && *s->type;
}

static const char *check_sample(const struct sample *s)
{
    if (strcmp(s->present, "1") || strcmp(s->health, "Good")) return "battery-health";
    if (!number(s->temp, 100, 419)) return "temperature";
    if (!V500_HOLD_TO_TEN && !number(s->level, 30, 100)) return "below-30-percent";
    return NULL;
}

struct session {
    bool saw_dcp;
    long disconnected;
    bool started, released;
    long released_at, initial_counter, first_hundred;
};

static const char *advance(struct session *run, const struct sample *s, long elapsed)
{
    const char *fault = check_sample(s);
    if (fault) return fault;
    if (V500_HOLD_TO_TEN) {
        if (!run->started && !number(s->level, 90, 100)) return "unexpected-start-level";
        run->started = true;
        if (!number(s->uv, 3400001, 4320000)) return "cell-voltage";
        if (!strcmp(s->online, "1")) run->disconnected = -1;
        else if (run->disconnected < 0) run->disconnected = elapsed;
        else if (elapsed - run->disconnected >= 30) return "lost-input";
        if (!run->released && elapsed >= 16 * 3600) return "hold-timeout";
        if (!run->released && atoi(s->level) <= 10) {
            run->released = true;
            run->released_at = elapsed;
            run->initial_counter = atol(s->counter);
        }
        if (!run->released) return strcmp(s->limit, "0") ? "unexpected-input-limit" : NULL;
        if (atoi(s->level) < 10) return "continued-discharge";
        long since = elapsed - run->released_at;
        if (since >= 90 && (!atoi(s->limit) || atol(s->counter) <= run->initial_counter))
            return "release-needs-reference-charger";
        if (V500_FULL_CYCLE) {
            if (!strcmp(s->status, "Full")) return "pmic-full";
            if (atoi(s->level) < 100) run->first_hundred = 0;
            else if (!run->first_hundred) run->first_hundred = elapsed;
            else if (elapsed - run->first_hundred >= 3600) return "one-hour-at-100-percent";
            return since >= 86400 ? "24h-charge-limit" : NULL;
        }
        if (since >= 180 && atol(s->ua) > 0 && atol(s->counter) > run->initial_counter)
            return "charge-resumed";
        return since >= 300 ? "release-window-ended" : NULL;
    }
    bool online = !strcmp(s->online, "1");
    bool dcp = online && !strcmp(s->type, "DCP");
    if (dcp) run->saw_dcp = true;
    if (online) run->disconnected = -1;
    else if (run->disconnected < 0) run->disconnected = elapsed;
    if (run->saw_dcp && run->disconnected >= 0 && elapsed - run->disconnected >= 60)
        return "charger-removed";
    if (run->saw_dcp && online && !strcmp(s->type, "SDP")) return "pc-reconnected";
    if (dcp && !strcmp(s->status, "Full")) return "pmic-full";
    if (!run->saw_dcp && elapsed >= 600) return "no-dcp-in-600s";
    if (elapsed >= 86400) return "24h-limit";
    return NULL;
}

static void record(int sys, int kmsg, int wd, FILE *log, const char *name, char *activity, size_t activity_size)
{
    long start = uptime_s();
    long last_ready = -30;
    bool reported_ready = false;
    struct session run = { .disconnected = -1 };
    const char *reason = "initial-telemetry";
    for (;;) {
        long now = uptime_s();
        if (now < start || start < 0) { reason = "clock"; break; }
        struct sample s = {0};
        bool battery = read_text(sys, "class/power_supply/battery/uevent", s.battery, sizeof(s.battery)-1);
        bool usb = read_text(sys, "class/power_supply/usb/uevent", s.usb, sizeof(s.usb)-1);
        bool valid = decode(&s);
        char brightness[32] = "", dpms[32] = "";
        read_text(sys, "class/backlight/panel-backlight/actual_brightness", brightness, sizeof(brightness));
        if (!read_text(sys, "class/drm/card1-DSI-1/dpms", dpms, sizeof(dpms)))
            read_text(sys, "class/drm/card0-DSI-1/dpms", dpms, sizeof(dpms));
        if (fprintf(log, "%ld,%ld,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                    (long)time(NULL), now, s.online, s.type, s.limit, s.present, s.health,
                    s.temp, s.uv, s.ua, s.counter, s.level, s.status, brightness, dpms) < 0 ||
            fflush(log) || fdatasync(fileno(log))) { reason = "log-write"; break; }
        if (!battery || !usb || !valid) {
            if (reported_ready || now - start >= 30) { reason = "missing-telemetry"; break; }
        } else {
            reason = advance(&run, &s, now - start);
            if (reason) break;
            if (ioctl(wd, WDIOC_KEEPALIVE, 0) < 0) { reason = "watchdog"; break; }
            if (V500_HOLD_TO_TEN) reported_ready = true;
            if (V500_FULL_CYCLE && now - last_ready >= 30) {
                /* Expose only the durable file name and length to the host's
                 * unprivileged handoff check through /proc/PID/cmdline. */
                snprintf(activity, activity_size, "%s:%ld", name, ftell(log));
                dprintf(kmsg, "<5>v500-cycle: synced %s bytes=%ld level=%s input=%s\n",
                        name, ftell(log), s.level, s.limit);
                last_ready = now;
            }
            if (!run.saw_dcp && now - last_ready >= 30 && !strcmp(s.type, "SDP") && !strcmp(s.online, "1")) {
                dprintf(kmsg, "<5>v500-wall: ready %s\n", name);
                last_ready = now;
                reported_ready = true;
            }
        }
        sleep((run.saw_dcp || (V500_HOLD_TO_TEN && reported_ready)) ? 5 : 2);
    }
    fprintf(log, "# end=%s uptime_s=%ld\n", reason, uptime_s());
    fflush(log);
    fsync(fileno(log));
    dprintf(kmsg, "<4>v500-wall: end %s %s\n", name, reason);
    sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART2, "recovery");
    for (;;) pause(); /* Keep the watchdog open if the reboot syscall fails. */
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--self-test")) {
        struct sample s = {
            .battery = "POWER_SUPPLY_PRESENT=1\nPOWER_SUPPLY_HEALTH=Good\nPOWER_SUPPLY_TEMP=285\nPOWER_SUPPLY_VOLTAGE_NOW=4096000\nPOWER_SUPPLY_CURRENT_NOW=-1234\nPOWER_SUPPLY_CHARGE_COUNTER=3030000\nPOWER_SUPPLY_CAPACITY=90\nPOWER_SUPPLY_STATUS=Not charging",
            .usb = "POWER_SUPPLY_ONLINE=1\nPOWER_SUPPLY_CURRENT_MAX=500000\nPOWER_SUPPLY_USB_TYPE=Unknown SDP [DCP]"
        };
        if (!decode(&s) || strcmp(s.type, "DCP") || strcmp(s.status, "Not charging") || check_sample(&s)) return 1;
        struct session run = { .disconnected = -1 };
        if (V500_HOLD_TO_TEN) {
            s.type = "SDP"; s.limit = "0";
            if (advance(&run, &s, 10)) return 1;
            s.level = "11"; if (advance(&run, &s, 3600)) return 1;
            s.level = "10"; if (advance(&run, &s, 3700) || !run.released) return 1;
            if (!advance(&run, &s, 3790)) return 1;
            s.limit = "500000"; s.counter = "3030400"; s.ua = "50000";
            if (advance(&run, &s, 3790)) return 1;
            if (V500_FULL_CYCLE) {
                if (advance(&run, &s, 3880) || advance(&run, &s, 4000)) return 1;
                s.level = "100"; if (advance(&run, &s, 12000) || !advance(&run, &s, 15600)) return 1;
                s.status = "Full"; if (strcmp(advance(&run, &s, 15605), "pmic-full")) return 1;
                s.status = "Charging";
            } else if (strcmp(advance(&run, &s, 3880), "charge-resumed")) return 1;
            s.level = "9"; if (!advance(&run, &s, 3800)) return 1;
            s.level = "30"; s.uv = "3399999"; if (!advance(&run, &s, 3800)) return 1;
            puts("One-shot discharge guard ARM self-test passed");
            return 0;
        }
        s.type = "SDP";
        if (advance(&run, &s, 1) || !advance(&run, &s, 600)) return 1;
        s.online = "0"; s.type = "Unknown";
        if (advance(&run, &s, 30)) return 1;
        s.online = "1"; s.type = "DCP";
        if (advance(&run, &s, 32) || advance(&run, &s, 3600) || !run.saw_dcp) return 1;
        s.status = "Full";
        if (!advance(&run, &s, 3605)) return 1;
        s.type = "SDP";
        if (!advance(&run, &s, 3606)) return 1;
        s.temp = "420";
        if (!check_sample(&s)) return 1;
        s.temp = "298"; s.level = "29";
        if (!check_sample(&s)) return 1;
        puts("Charge logger ARM self-test passed (DCP, return to PC, full, time, temperature, charge hold)");
        return 0;
    }
    if (chdir(argv[1])) return 1;
    /* The host arms the one-shot only on tablet 2. A reboot cannot restart
     * the discharge; bootstrap returns to the original recovery instead. */
    if (V500_HOLD_TO_TEN) {
        int dir = open(".", O_RDONLY | O_DIRECTORY);
        if (dir < 0 || unlinkat(dir, "nr2-hold-to-ten.once", 0) || fsync(dir)) return 1;
        close(dir);
    }
    char name[] = "wall-charge-XXXXXX.csv";
    int fd = mkstemps(name, 4);
    if (fd < 0 || fchmod(fd, 0644)) return 1;
    FILE *log = fdopen(fd, "w");
    int sys = open("/sys", O_RDONLY | O_DIRECTORY);
    int kmsg = open("/dev/kmsg", O_WRONLY);
    int wd = open("/dev/watchdog0", O_WRONLY);
    struct watchdog_info info = {0};
    int seconds = 120;
    if (!log || sys < 0 || kmsg < 0 || wd < 0 || ioctl(wd, WDIOC_GETSUPPORT, &info) ||
        strcmp((const char *)info.identity, "qcom_wdt") || ioctl(wd, WDIOC_SETTIMEOUT, &seconds) ||
        seconds != 120) return 1;
    fputs("epoch_s,uptime_s,usb_online,usb_type,input_limit_uA,present,health,temp_dC,voltage_uV,cell_current_uA,charge_uAh,capacity_pct,status,backlight_actual,panel_dpms\n", log);
    int dir = open(".", O_RDONLY | O_DIRECTORY);
    if (dir < 0 || fflush(log) || fsync(fd) || fsync(dir)) return 1;
    close(dir);
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) record(sys, kmsg, wd, log, name, argv[1], strlen(argv[1]) + 1);
    return 0;
}
