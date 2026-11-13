#include "kernel.h"
#include "serial.h"

void test_divide_by_zero(void)
{
    volatile int zero = 0;
    volatile int result = 42 / zero; /* This should trigger divide-by-zero exception */
    (void)result; /* Prevent unused variable warning */
}

/* Trigger an invalid-opcode exception (#UD, vector 6) via the ud2 instruction. */
void test_invalid_opcode(void)
{
    __asm__ volatile("ud2");
}

/* Deliberately cause a double fault (#DF, vector 8) in isolation.
 *
 * Method (genuine hardware-fault trigger): with interrupts disabled, set RSP
 * to a non-canonical address and execute `push rax`. The push faults because
 * the stack pointer is non-canonical -> #SS (stack fault, vector 12, which is
 * not installed) -> delivering #SS faults -> the CPU raises a double fault
 * (#DF, vector 8). The #DF gate has IST=1, so it switches to the dedicated
 * IST1 stack and runs double_fault_handler.
 *
 * This deliberately causes a double fault without modifying any other
 * exception handler's body. */
void test_double_fault(void)
{
    __asm__ volatile(
        "cli\n"
        "mov $0x800000000000, %%rsp\n"   /* non-canonical stack address */
        "push %%rax\n"                   /* faults: stack not canonical -> #SS */
        : : : "rsp");
}
/* Recurse until the boot stack runs into its unmapped guard page. The #PF
 * cannot be delivered on the dead stack, so the CPU escalates to #DF, which
 * runs on its own IST1 stack and reports the failure. */
__attribute__((noinline)) static uint64_t recurse(uint64_t depth)
{
    volatile uint8_t pad[512];
    pad[0] = (uint8_t)depth;
    if (depth == UINT64_MAX)   /* never true; keeps the recursion non-tail */
        return 0;
    return recurse(depth + 1) + pad[0];
}

void test_stack_overflow(void)
{
    recurse(0);
}
