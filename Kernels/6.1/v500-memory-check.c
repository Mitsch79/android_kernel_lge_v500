// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <stdint.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* Four independent 16 MiB buffers. Read in a different order from writes
 * so the check must leave the shared L2 and fetch data from DRAM. */
static int check(unsigned int cpu, uint32_t seed)
{
	const unsigned int words = 1U << 22;
	volatile uint32_t *data = malloc(words * sizeof(*data));
	cpu_set_t mask;
	unsigned int i;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	if (!data || sched_setaffinity(0, sizeof(mask), &mask))
		return 1;
	for (i = 0; i < words; i++)
		data[i] = (i * 2654435761U) ^ seed;
	/* Visit each cache line once per word position. */
	for (i = 0; i < words; i++) {
		unsigned int j = ((i << 4) | (i >> 18)) & (words - 1);
		if (data[j] != ((j * 2654435761U) ^ seed))
			return 1;
	}
	return sched_getcpu() != (int)cpu;
}

int main(void)
{
	unsigned int cpu;
	int status, failed = 0;
	for (cpu = 0; cpu < 4; cpu++) {
		pid_t child = fork();
		if (!child)
			_exit(check(cpu, (uint32_t)getpid() ^ (cpu << 28)));
		if (child < 0) {
			failed = 1;
			break;
		}
	}
	while (cpu-- > 0) {
		if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
			failed = 1;
	}
	return failed;
}
