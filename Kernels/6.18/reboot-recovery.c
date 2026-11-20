#include <linux/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

_Static_assert(SYS_reboot == 88, "V500 needs the 32-bit ARM reboot syscall");

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--dry-run") == 0) {
        puts("Linux RESTART2 argument: recovery");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--after") == 0) {
        char *end;
        unsigned long seconds = strtoul(argv[2], &end, 10);
        if (*end != '\0' || seconds == 0 || seconds > 900) {
            fputs("Recovery delay must be in the range 1–900 seconds\n", stderr);
            return 2;
        }
        while (seconds != 0)
            seconds = sleep(seconds);
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--dry-run | --after SECONDS]\n", argv[0]);
        return 2;
    }

    sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART2, "recovery");
    perror("reboot recovery");
    return 1;
}
