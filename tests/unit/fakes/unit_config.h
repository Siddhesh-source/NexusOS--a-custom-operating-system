/* Force-included (-include) before every unit-test source file.
 *
 * The heap runs over a plain page-aligned array instead of the kernel's
 * fixed virtual range. 8 MiB is plenty for the tests and small enough to
 * keep the programs light. */
#include <stdint.h>

extern unsigned char unit_heap_area[];
#define KERNEL_HEAP_BASE  ((uint64_t)unit_heap_area)
#define KERNEL_HEAP_MAX   ((uint64_t)8 << 20)
