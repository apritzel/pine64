/*
 * test_timer: test Linux and ARM generic timer for monotonicity
 * uses Perl's Test Anything Protocol (TAP), try "prove"
 *
 * Copyright (C) 2016 - 2018 Andre Przywara
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <sched.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>

#if defined __aarch64__
static void delay_tick(unsigned long r)
{
	__asm__ volatile (
		"1:subs	%0, %0, #1\n\t"
		"b.ne	1b\n"
		: : "r" (r)
	);
}

static uint64_t read_cntfrq(void)
{
	uint64_t reg;

	__asm__ volatile ("mrs %0, CNTFRQ_EL0\n" : "=r" (reg));

	return reg;
}

static uint64_t read_counter(void)
{
	uint64_t reg;

	__asm__ volatile ("mrs %0, CNTVCT_EL0\n" :"=r" (reg));

	return reg;
}

static uint64_t read_counter_sync(void)
{
	uint64_t reg;

	__asm__ volatile ("isb; mrs %0, CNTVCT_EL0\n" :"=r" (reg));

	return reg;
}

#elif defined(__arm__)

static void delay_tick(unsigned long r)
{
	__asm__ volatile (
		"1:subs	%0, %0, #1\n\t"
		"bne	1b\n"
		: : "r" (r)
	);
}

static uint64_t read_cntfrq(void)
{
	uint32_t reg;

	__asm__ volatile ("mrc p15, 0, %0, c14, c0, 0\n" : "=r" (reg));

	return reg;
}

static uint64_t read_counter(void)
{
	uint32_t lo, hi;

	__asm__ volatile ("mrrc p15, 1, %0, %1, c14\n" :"=r" (lo), "=r" (hi));

	return ((uint64_t)hi << 32) | lo;
}

static uint64_t read_counter_sync(void)
{
	uint32_t lo, hi;

	__asm__ volatile (
		"isb\n\t"
		"mrrc p15, 1, %0, %1, c14\n"
		: "=r" (lo), "=r" (hi)
	);

	return ((uint64_t)hi << 32) | lo;
}

#else
#error unsupported architecture
#endif

#define RESTORE_ONLY	-1
#define ALL_CORES	-2

static int pin_thread(pid_t pid, int core, bool restore)
{
	static cpu_set_t oldmask;
	cpu_set_t mask;

	if (restore)
		sched_setaffinity(pid, sizeof(cpu_set_t), &oldmask);
	else
		sched_getaffinity(pid, sizeof(cpu_set_t), &oldmask);

	if (core == RESTORE_ONLY)
		return 0;

	CPU_ZERO(&mask);
	if (core == ALL_CORES) {
		int i;
		int nr_cores = sysconf(_SC_NPROCESSORS_CONF);

		for (i = 0; i < nr_cores; i++)
			CPU_SET(i, &mask);
	} else {
		CPU_SET(core, &mask);
	}

	return sched_setaffinity(pid, sizeof(cpu_set_t), &mask);
}

static long nr_procs(void)
{
	long online = sysconf(_SC_NPROCESSORS_ONLN);
	long cpus = sysconf(_SC_NPROCESSORS_CONF);

	if (cpus != online)
		fprintf(stdout, "# %ld CPU%s offline\n",
			cpus - online,
			cpus - online > 1 ? "s" : "");

	return cpus;
}

static int test_frequency(FILE *stream, int testnr, int nr_cores)
{
	uint64_t freq = ~0, r;
	int i;
	bool equal = true;

	for (i = 0; i < nr_cores; i++) {
		if (pin_thread(0, i, false))
			continue;

		r = read_cntfrq();
		if (freq == ~0)
			freq = r;
		else
			equal = equal && (freq == r);

		pin_thread(0, RESTORE_ONLY, true);
	}
	fprintf(stream, "%sok %d same timer frequency on all cores\n",
		equal ? "" : "not ", testnr);
	fprintf(stream, "# timer frequency is %"PRId64" Hz (%"PRId64" MHz)\n",
		freq, freq / 1000000);

	return 1;
}

static void offset_info(FILE *stream, int core)
{
	uint64_t cnt, freq;
	uint64_t diff1, diff2, diff3;

	if (pin_thread(0, core, false))
		return;

	freq = read_cntfrq();
	cnt = read_counter_sync();
	diff1 = read_counter() - cnt;

	cnt = read_counter_sync();
	diff2 = read_counter_sync() - cnt;

	cnt = read_counter_sync();
	delay_tick(50);
	diff3 = read_counter_sync() - cnt;

	fprintf(stream, "# core %d: counter value: %"PRId64" => %"PRId64" sec\n",
		core, cnt, cnt / freq);
	fprintf(stream, "# core %d: offsets: back-to-back: %"PRId64", b-t-b synced: %"PRId64", b-t-b w/ delay: %"PRId64"\n",
		core, diff1, diff2, diff3);
	pin_thread(0, RESTORE_ONLY, true);
}

#define NSECS 1000000000U
#define MAX_ERRORS 16

static void test_monotonic(FILE *stream, int loops, int testnr)
{
	uint64_t time1, time2;
	int64_t diff, min = INT64_MAX, max = 0, sum = 0;
	int errcnt = 0;
	int i;

	for (i = 0; i < loops; i++) {
		time1 = read_counter_sync();
		time2 = read_counter();
		diff = time2 - time1;

		if (diff < 0) {
			errcnt++;
			if (errcnt <= MAX_ERRORS)
				fprintf(stream, "# time1: %"PRIx64", time2: %"PRIx64", diff: %"PRId64"\n",
					time1, time2, diff);
			if (errcnt == MAX_ERRORS + 1)
				fprintf(stream, "# too many errors, stopping reports\n");
		}

		if (diff < min)
			min = diff;
		if (diff > max)
			max = diff;
		sum += diff;
	}
	fprintf(stream, "%sok %d native counter reads are monotonic # %d errors\n",
		min >= 0 ? "" : "not ", testnr, errcnt);
	fprintf(stream, "# min: %"PRId64", avg: %"PRId64", max: %"PRId64"\n",
		min, sum / loops, max);
}

static void test_monotonic_linux(FILE *stream, int loops, int testnr)
{
	struct timespec tp1, tp2;
	int64_t diff, min = INT64_MAX, max = 0, sum = 0;
	int errcnt = 0;
	int i;

	for (i = 0; i < loops; i++) {
		clock_gettime(CLOCK_MONOTONIC_RAW, &tp1);
		clock_gettime(CLOCK_MONOTONIC_RAW, &tp2);
		diff = (tp2.tv_sec * NSECS + tp2.tv_nsec) -
			(tp1.tv_sec * NSECS + tp1.tv_nsec);

		if (diff < 0) {
			if (errcnt == 0)
				fprintf(stream, "# diffs: ");
			errcnt++;
			if (errcnt <= MAX_ERRORS)
				fprintf(stream, "%s%"PRId64"",
					errcnt == 1 ? "" : ", ", diff);
			if (errcnt == MAX_ERRORS + 1)
				fprintf(stream, "\n# too many errors, stopping reports");
		}

		if (diff < min)
			min = diff;
		if (diff > max)
			max = diff;
		sum += diff;
	}
	if (errcnt)
		fprintf(stream, "\n");
	fprintf(stream, "%sok %d Linux counter reads are monotonic # %d errors\n",
		min >= 0 ? "" : "not ", testnr, errcnt);
	fprintf(stream, "# min: %"PRId64", avg: %"PRId64", max: %"PRId64"\n",
		min, sum / loops, max);
}

int main(int argc, char** argv)
{
	int nr_cpus;
	int i;

	fprintf(stdout, "TAP version 13\n");
	nr_cpus = nr_procs();
	fprintf(stdout, "# number of cores: %d\n", nr_cpus);


	test_frequency(stdout, 1, nr_cpus);

	test_monotonic(stdout, 10000000, 2);
	test_monotonic_linux(stdout, 10000000, 3);

	for (i = 0; i < nr_cpus; i++)
		offset_info(stdout, i);

	fprintf(stdout, "1..3\n");
	return 0;
}
