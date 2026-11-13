/* Stack protector support.
 *
 * The compiler (-fstack-protector-strong) puts a secret value, the canary,
 * between a function's local arrays and its return address, and checks it
 * just before returning. A write that runs past the end of a local buffer
 * changes the canary, and the check calls __stack_chk_fail instead of
 * returning into whatever the overflow wrote.
 *
 * The canary is the global __stack_chk_guard (-mstack-protector-guard=
 * global). entry.asm replaces this starting value with a boot-time random
 * one before any C code runs. Its lowest byte is kept 0 so string copies,
 * which stop at a 0 byte, cannot reproduce it. */

#include <stdint.h>
#include "panic.h"

uint64_t __stack_chk_guard = 0x2f8e6a1b7c3d5900ULL;

__attribute__((noreturn)) void __stack_chk_fail(void)
{
    panic("stack smashing detected: a function wrote past the end of a "
          "local buffer and overwrote its stack canary");
}
