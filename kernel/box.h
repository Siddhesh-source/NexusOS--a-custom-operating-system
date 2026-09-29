#ifndef NEXUS_BOX_H
#define NEXUS_BOX_H

/* Fixed-width box drawing for structured serial diagnostics. */

#define BOX_INNER_WIDTH 72   /* display columns between the side borders */

void box_top(void);
void box_separator(void);
void box_bottom(void);
/* One bordered line; content is truncated or padded to BOX_INNER_WIDTH. */
void box_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* NEXUS_BOX_H */
