/*
 * Bytes to lines, for the host link. RTOS-free.
 *
 * No Zephyr in here, so tests/host/test_host_lines.c can run every case - a
 * line cut across packets, a line that never ends, bytes that went missing -
 * under a plain C compiler with the sanitizers on.
 */
#ifndef NEXUS_HOST_LINES_H_
#define NEXUS_HOST_LINES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longest line we will look at, terminator included. Anything longer is
 * truncated rather than split, so a runaway host cannot desynchronise the
 * parser. */
#define HOST_LINE_MAX 72

/*
 * "Bytes are missing here." A transport that had to drop some puts this one
 * in their place, and the assembler throws away the line it was in the
 * middle of and everything up to the next newline - so a line with a hole in
 * it is never read as a shorter line. The protocol is text; no line has a
 * NUL in it.
 */
#define HOST_LINES_GAP 0x00

/* One per wire. Two wires never share one: see host_priv.h. */
struct host_lines {
	char line[HOST_LINE_MAX];
	uint8_t len;
	/* Dropping bytes until the next newline. */
	bool skip;
};

typedef void (*host_line_fn)(const char *line);

/**
 * Take @p n bytes and call @p emit once for every line they finish. A line
 * is NUL-terminated, never empty, never longer than HOST_LINE_MAX - 1, and
 * has no CR in it.
 *
 * @return how many lines were emitted.
 */
unsigned int host_lines_feed(struct host_lines *l, const uint8_t *buf, size_t n,
			     host_line_fn emit);

/**
 * Take @p n bytes that are not going to be read: forget the line in
 * progress, and if they stop part-way through a line, skip the rest of it
 * when bytes are next fed.
 */
void host_lines_discard(struct host_lines *l, const uint8_t *buf, size_t n);

/** Forget the line in progress. The next byte starts a new one. */
void host_lines_reset(struct host_lines *l);

#endif /* NEXUS_HOST_LINES_H_ */
