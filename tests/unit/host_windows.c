/* Windows host layer for unit tests: three kernel32 calls, no C library.
 * The import library is made from kernel32.def by llvm-dlltool, so only
 * the LLVM tools the kernel build already needs are required. */
#include "unit.h"

typedef void *HANDLE;
#define STD_OUTPUT_HANDLE ((unsigned long)-11)

__declspec(dllimport) HANDLE GetStdHandle(unsigned long which);
__declspec(dllimport) int WriteFile(HANDLE h, const void *buf, unsigned long len,
                                    unsigned long *written, void *overlapped);
__declspec(dllimport) __attribute__((noreturn)) void ExitProcess(unsigned code);

void host_write(const char *s, size_t n)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    while (n > 0) {
        unsigned long done = 0;
        if (!WriteFile(out, s, (unsigned long)n, &done, 0) || done == 0)
            return;
        s += done;
        n -= done;
    }
}

void host_exit(int code)
{
    ExitProcess((unsigned)code);
}

/* Program entry (linked with /entry:unit_start). */
__attribute__((noreturn)) void unit_start(void)
{
    host_exit(unit_main());
}
