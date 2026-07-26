/*
 * Standalone reproducer for the GetTimeAndDate NULL-deref segfault.
 * Build/run on host (no iotop deps):
 *   gcc -O0 -g -o /tmp/test-gettime scripts/test-gettime-crash.c && /tmp/test-gettime
 */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

/* --- original buggy implementation --- */
static struct tm *GetTimeAndDate_BUGGY(unsigned long long milliseconds)
{
	time_t seconds = (time_t)(milliseconds / 1000);
	if ((uint64_t)seconds * 1000 == milliseconds)
		return localtime(&seconds);
	return NULL; /* callers then crash on ptm->tm_hour */
}

/* --- fixed implementation --- */
static struct tm *GetTimeAndDate_FIXED(unsigned long long milliseconds)
{
	time_t seconds = (time_t)(milliseconds / 1000ULL);
	return localtime(&seconds);
}

static int try_format(const char *label, struct tm *(*fn)(unsigned long long), unsigned long long v)
{
	char buf[16];
	struct tm *ptm = fn(v);

	printf("%s value=%llu -> ptm=%p", label, (unsigned long long)v, (void *)ptm);
	if (!ptm) {
		printf("  [would SIGSEGV on ptm->tm_hour]\n");
		return 1;
	}
	snprintf(buf, sizeof buf, "%02d:%02d:%02d", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
	printf("  format=%s\n", buf);
	return 0;
}

int main(void)
{
	unsigned long long samples[] = {
		0,
		1000,       /* exact multiple — old code OK */
		1500,       /* not multiple — old code NULL / crash */
		1234567,    /* typical accumulated usecs/msecs */
		999,        /* < 1s, not multiple */
	};
	size_t i;
	int buggy_nulls = 0;

	printf("=== Buggy GetTimeAndDate (historical) ===\n");
	for (i = 0; i < sizeof samples / sizeof samples[0]; i++)
		buggy_nulls += try_format("BUGGY", GetTimeAndDate_BUGGY, samples[i]);

	printf("\n=== Fixed GetTimeAndDate ===\n");
	for (i = 0; i < sizeof samples / sizeof samples[0]; i++)
		try_format("FIXED", GetTimeAndDate_FIXED, samples[i]);

	printf("\n");
	if (buggy_nulls) {
		printf("Reproduced: buggy path returned NULL %d time(s) — matches occasional batch segfault.\n",
		       buggy_nulls);
		return 0;
	}
	printf("Unexpected: buggy path never returned NULL\n");
	return 1;
}
