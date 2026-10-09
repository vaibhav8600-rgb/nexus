/*
 * Bytes to lines, for the host link. See host_lines.h for why this file
 * knows nothing about Zephyr.
 */

#include "host_lines.h"

unsigned int host_lines_feed(struct host_lines *l, const uint8_t *buf, size_t n,
			     host_line_fn emit)
{
	unsigned int lines = 0;

	for (size_t i = 0; i < n; i++) {
		uint8_t c = buf[i];

		if (c == HOST_LINES_GAP) {
			l->len = 0;
			l->skip = true;
			continue;
		}
		if (c == '\n') {
			if (l->skip) {
				l->skip = false;
				continue;
			}
			if (l->len == 0) {
				continue;
			}
			l->line[l->len] = '\0';
			l->len = 0;
			emit(l->line);
			lines++;
			continue;
		}
		if (l->skip || c == '\r') {
			continue;
		}
		/* Truncate rather than wrap: the tail of an over-long line is
		 * dropped, the next line still parses. */
		if (l->len < HOST_LINE_MAX - 1) {
			l->line[l->len++] = (char)c;
		}
	}
	return lines;
}

void host_lines_discard(struct host_lines *l, const uint8_t *buf, size_t n)
{
	if (n > 0) {
		l->skip = buf[n - 1] != '\n';
	} else if (l->len > 0) {
		l->skip = true;
	}
	l->len = 0;
}

void host_lines_reset(struct host_lines *l)
{
	l->len = 0;
	l->skip = false;
}
