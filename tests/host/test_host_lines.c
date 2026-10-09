/*
 * The host link's line assembler: what a real wire does to a line of text.
 * Host-side, no Zephyr:
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/host \
 *      tests/host/test_host_lines.c src/host/host_lines.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_lines.h"

static int g_fail;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
			g_fail++;                                              \
		}                                                              \
	} while (0)

/* What came out, in order. */
#define LOG_MAX 16
static char g_log[LOG_MAX][HOST_LINE_MAX];
static int g_n;

static void emit(const char *line)
{
	/* The promises in host_lines.h, checked on every line there is. */
	CHECK(line[0] != '\0');
	CHECK(strlen(line) <= HOST_LINE_MAX - 1);
	CHECK(strchr(line, '\r') == NULL);
	if (g_n < LOG_MAX) {
		strcpy(g_log[g_n], line);
	}
	g_n++;
}

static void clear(void)
{
	g_n = 0;
	memset(g_log, 0, sizeof(g_log));
}

static unsigned int feed(struct host_lines *l, const char *s)
{
	return host_lines_feed(l, (const uint8_t *)s, strlen(s), emit);
}

static void test_whole_lines(void)
{
	struct host_lines l = {0};

	clear();
	CHECK(feed(&l, "C 37\nM 62\n") == 2);
	CHECK(g_n == 2 && !strcmp(g_log[0], "C 37") && !strcmp(g_log[1], "M 62"));

	/* CRLF, a blank line and a bare CR are not lines. */
	clear();
	CHECK(feed(&l, "T 100\r\n\r\n\n\rD 5\n") == 2);
	CHECK(!strcmp(g_log[0], "T 100") && !strcmp(g_log[1], "D 5"));

	/* Nothing is emitted until the newline arrives. */
	clear();
	CHECK(feed(&l, "N So What") == 0 && g_n == 0);
	CHECK(feed(&l, "\n") == 1 && !strcmp(g_log[0], "N So What"));
}

/* A Bluetooth write is 20 bytes: every line is cut somewhere. */
static void test_chunks(void)
{
	static const char burst[] =
		"T 48720\nD 20709\nC 37\nM 62\nN Kind Of Blue\nA Miles Davis\nP 1\n";
	struct host_lines l = {0};
	unsigned int got = 0;

	clear();
	for (size_t at = 0; at < strlen(burst); at += 20) {
		size_t n = strlen(burst) - at;

		got += host_lines_feed(&l, (const uint8_t *)burst + at,
				       n < 20 ? n : 20, emit);
	}
	CHECK(got == 7 && g_n == 7);
	CHECK(!strcmp(g_log[0], "T 48720") && !strcmp(g_log[4], "N Kind Of Blue"));
	CHECK(!strcmp(g_log[5], "A Miles Davis") && !strcmp(g_log[6], "P 1"));

	/* And one byte at a time, which is every cut there is. */
	clear();
	got = 0;
	for (size_t at = 0; at < strlen(burst); at++) {
		got += host_lines_feed(&l, (const uint8_t *)burst + at, 1, emit);
	}
	CHECK(got == 7 && !strcmp(g_log[1], "D 20709") && !strcmp(g_log[3], "M 62"));
}

static void test_overlong(void)
{
	struct host_lines l = {0};
	char big[200];

	memset(big, 'x', sizeof(big));
	big[0] = 'N';
	big[1] = ' ';
	big[sizeof(big) - 1] = '\0';

	clear();
	CHECK(feed(&l, big) == 0);
	CHECK(feed(&l, "\nC 5\n") == 2);
	/* Truncated to what fits, not split into two lines... */
	CHECK(strlen(g_log[0]) == HOST_LINE_MAX - 1 && g_log[0][0] == 'N');
	/* ...and the line after it is untouched. */
	CHECK(!strcmp(g_log[1], "C 5"));
}

/* Two wires, a byte from each in turn: each keeps its own lines. */
static void test_two_sources(void)
{
	static const char usb[] = "C 11\nN From The Cable\n";
	static const char ble[] = "C 99\nN Over The Air\n";
	struct host_lines a = {0}, b = {0};
	size_t ia = 0, ib = 0;

	clear();
	while (ia < strlen(usb) || ib < strlen(ble)) {
		if (ia < strlen(usb)) {
			host_lines_feed(&a, (const uint8_t *)usb + ia++, 1, emit);
		}
		if (ib < strlen(ble)) {
			host_lines_feed(&b, (const uint8_t *)ble + ib++, 1, emit);
		}
	}
	CHECK(g_n == 4);
	CHECK(!strcmp(g_log[0], "C 11") && !strcmp(g_log[1], "C 99"));
	/* The longer line finishes second; neither has the other's bytes. */
	CHECK(!strcmp(g_log[2], "N Over The Air"));
	CHECK(!strcmp(g_log[3], "N From The Cable"));
}

/*
 * A write that did not fit is dropped whole and leaves a gap. Without the
 * gap, "C 1" + [dropped "5\nM 3"] + "0\n" reads as "C 10": a number nobody
 * sent, which to_u32() has no way to reject.
 */
static void test_gap(void)
{
	static const uint8_t torn[] = {'C', ' ', '1', HOST_LINES_GAP,
				       '0', '\n', 'M', ' ', '5', '0', '\n'};
	struct host_lines l = {0};

	clear();
	CHECK(host_lines_feed(&l, torn, sizeof(torn), emit) == 1);
	CHECK(g_n == 1 && !strcmp(g_log[0], "M 50"));

	/* A gap between lines costs the line after it, and no more. */
	static const uint8_t between[] = {'C', ' ', '1', '\n', HOST_LINES_GAP,
					  ' ', '7', '\n', 'M', ' ', '2', '\n'};

	clear();
	CHECK(host_lines_feed(&l, between, sizeof(between), emit) == 2);
	CHECK(!strcmp(g_log[0], "C 1") && !strcmp(g_log[1], "M 2"));

	/* The skip survives across feeds until its newline comes. */
	static const uint8_t gap[] = {'N', ' ', 'a', HOST_LINES_GAP};

	clear();
	CHECK(host_lines_feed(&l, gap, sizeof(gap), emit) == 0);
	CHECK(feed(&l, "tail of a title") == 0);
	CHECK(feed(&l, "\nP 1\n") == 1 && !strcmp(g_log[0], "P 1"));
}

static void test_reset_and_discard(void)
{
	struct host_lines l = {0};

	/* Reset mid-line: the half is forgotten, the next line is whole. */
	clear();
	CHECK(feed(&l, "N half a ti") == 0);
	host_lines_reset(&l);
	CHECK(feed(&l, "C 8\n") == 1 && !strcmp(g_log[0], "C 8"));

	/* Reset also ends a skip. */
	static const uint8_t gap[] = {HOST_LINES_GAP};

	clear();
	host_lines_feed(&l, gap, sizeof(gap), emit);
	host_lines_reset(&l);
	CHECK(feed(&l, "M 9\n") == 1 && !strcmp(g_log[0], "M 9"));

	/* Discarded bytes that end on a line: the next line is read. */
	clear();
	CHECK(feed(&l, "N half") == 0);
	host_lines_discard(&l, (const uint8_t *)" a line\nC 1\n", 12);
	CHECK(feed(&l, "C 2\n") == 1 && !strcmp(g_log[0], "C 2"));

	/* Discarded bytes that stop inside a line: its tail is not a line.
	 * "N In C 4" cut after "N In " would otherwise read as "C 4". */
	clear();
	host_lines_discard(&l, (const uint8_t *)"M 5\nN In ", 9);
	CHECK(feed(&l, "C 4\nM 6\n") == 1 && !strcmp(g_log[0], "M 6"));

	/* Discarding nothing changes nothing. */
	clear();
	CHECK(feed(&l, "C 7") == 0);
	host_lines_discard(&l, NULL, 0);
	CHECK(feed(&l, "7\n") == 1 && !strcmp(g_log[0], "C 77"));
}

int main(void)
{
	test_whole_lines();
	test_chunks();
	test_overlong();
	test_two_sources();
	test_gap();
	test_reset_and_discard();

	if (g_fail) {
		printf("%d check(s) failed\n", g_fail);
		return EXIT_FAILURE;
	}
	printf("host lines: all checks passed\n");
	return EXIT_SUCCESS;
}
