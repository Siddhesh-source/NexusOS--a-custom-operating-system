#ifndef BHARAT_STRING_H
#define BHARAT_STRING_H

#include <stddef.h>

/* Freestanding memory primitives. Clang may emit calls to these even with
 * -ffreestanding (struct copies, zeroing loops), so they must exist. */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strcmp(const char *a, const char *b);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
/* Copy with truncation; dst is always 0-terminated (size > 0). Returns
 * strlen(src), so a result >= size means it was cut short. */
size_t strlcpy(char *dst, const char *src, size_t size);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);           /* last c, or NULL */
char *strstr(const char *hay, const char *needle);

#endif /* BHARAT_STRING_H */
