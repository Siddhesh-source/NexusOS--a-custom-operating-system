/* Linux host layer for unit tests: raw system calls, no C library. */
#include "unit.h"

void host_write(const char *s, size_t n)
{
    while (n > 0) {
        long r;
        __asm__ volatile("syscall"
                         : "=a"(r)
                         : "a"(1L), "D"(1L), "S"(s), "d"(n)   /* write(1, s, n) */
                         : "rcx", "r11", "memory");
        if (r <= 0)
            return;
        s += r;
        n -= (size_t)r;
    }
}

void host_exit(int code)
{
    for (;;)
        __asm__ volatile("syscall" :: "a"(231L), "D"((long)code)   /* exit_group */
                         : "rcx", "r11", "memory");
}

/* Program entry: the kernel passes the stack 16-byte aligned; clear RBP
 * so the frame chain ends here. */
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    xor %ebp, %ebp\n"
    "    and $-16, %rsp\n"
    "    call unit_start\n"
    "    ud2\n");

__attribute__((noreturn, used)) void unit_start(void)
{
    host_exit(unit_main());
}
