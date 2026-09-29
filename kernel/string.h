#ifndef NEXUS_STRING_H
#define NEXUS_STRING_H

#include <stddef.h>

/* Freestanding memory primitives. Clang may emit calls to these even with
 * -ffreestanding (struct copies, zeroing loops), so they must exist. */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#endif /* NEXUS_STRING_H */
