#ifndef NEXUS_KERNEL_H
#define NEXUS_KERNEL_H

#include <stdint.h>

void kernel_main(void) __attribute__((noreturn));

/* Deliberate fatal exceptions (test_exception.c), used by fault demos. */
void test_divide_by_zero(void);
void test_invalid_opcode(void);
void test_double_fault(void);
void test_stack_overflow(void);

#endif /* NEXUS_KERNEL_H */
