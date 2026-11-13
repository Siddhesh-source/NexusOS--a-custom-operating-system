#ifndef BHARAT_VM_TEST_H
#define BHARAT_VM_TEST_H

/* Boot-time self-tests for the scalable memory layer: buddy allocator,
 * slab caches, demand paging, copy-on-write and memory objects. */

#include <stdbool.h>

bool vm_run_tests(void);

#endif /* BHARAT_VM_TEST_H */
