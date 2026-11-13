/* Fake page allocator and page tables for heap_unit (heap.c). The heap's
 * memory is already there (unit_heap_area), so mapping only has to
 * succeed or fail. Programs that test the real pmm.c do not use this. */
#include "unit.h"
#include "kprintf.h"
#include "pmm.h"
#include "vmm.h"

/* Heap backing store for heap.c (see unit_config.h). */
__attribute__((aligned(4096))) unsigned char unit_heap_area[8 << 20];

/* ---- page allocator and page tables ------------------------------------
 * The heap's memory is already there (unit_heap_area), so mapping only
 * has to succeed or fail. Tests can make the Nth next frame allocation
 * fail to check the heap's all-or-nothing growth. */

uint64_t unit_frames_in_use;
int64_t unit_fail_frame_in = -1;   /* <0: never; 0: next allocation fails */
static address_space_t fake_space;

uint64_t pmm_alloc_page(void)
{
    if (unit_fail_frame_in == 0) {
        unit_fail_frame_in = -1;
        return 0;
    }
    if (unit_fail_frame_in > 0)
        unit_fail_frame_in--;
    unit_frames_in_use++;
    return 0x100000 + unit_frames_in_use * 0x1000;
}

pmm_status_t pmm_free_page(uint64_t phys)
{
    (void)phys;
    unit_frames_in_use--;
    return PMM_OK;
}

address_space_t *vmm_kernel_space(void)
{
    return &fake_space;
}

vmm_status_t vmm_map_page(address_space_t *as, uint64_t virt, uint64_t phys, uint32_t flags)
{
    (void)as; (void)virt; (void)phys; (void)flags;
    return VMM_OK;
}

vmm_status_t vmm_unmap_page(address_space_t *as, uint64_t virt, uint64_t *old_phys)
{
    (void)as;
    *old_phys = 0x100000 + virt % 0x1000;
    return VMM_OK;
}
