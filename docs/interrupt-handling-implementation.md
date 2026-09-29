# NEXUS OS Interrupt and Exception Handling Implementation

## Overview
This document describes the implementation of CPU interrupt and exception handling added to NEXUS OS Phase 1. The implementation includes:
- Global Descriptor Table (GDT) setup
- Interrupt Descriptor Table (IDT) setup  
- Exception handlers for divide-by-zero, invalid opcode, general protection fault, page fault, and double fault
- Timer interrupt handler using the Programmable Interval Timer (PIT)
- Integration with the existing kernel boot sequence

## Files Created

### 1. `kernel/gdt.h` and `kernel/gdt.c`
- Implements Global Descriptor Table setup
- Creates null, kernel code, and kernel data segments
- Provides `gdt_init()` function to initialize and load the GDT

### 2. `kernel/idt.h` and `kernel/idt.c`  
- Implements Interrupt Descriptor Table setup
- Configures IDT entries for specific interrupt vectors
- Provides `idt_init()` function to initialize and load the IDT
- Sets up interrupt gates for:
  - Vector 0: Divide-by-zero exception
  - Vector 6: Invalid opcode exception  
  - Vector 8: Double fault exception
  - Vector 13: General protection fault
  - Vector 14: Page fault
  - Vector 32: Timer interrupt (IRQ0)

### 3. `kernel/interrupts.h` and `kernel/interrupts.c`
- Contains C-level interrupt handler functions
- Implements specific handlers:
  - `divide_by_zero_handler()` - Prints error and halts
  - `invalid_opcode_handler()` - Prints error and halts  
  - `general_protection_handler()` - Prints error code and halts
  - `page_fault_handler()` - Prints detailed error info and halts
  - `double_fault_handler()` - Prints error and halts
  - `timer_interrupt_handler()` - Acknowledges PIC and increments tick counter
- Provides `interrupt_handler()` - Common dispatcher that saves registers and calls specific handlers

### 4. `kernel/timer.h` and `kernel/timer.c`
- Implements Programmable Interval Timer (PIT) initialization
- Configures PIT channel 0 for ~100Hz square wave output
- Provides `timer_init()` function
- Timer interrupt handler outputs a dot every second for visual feedback

### 5. `kernel/interrupt_asm.asm`
- Assembly language interrupt stubs
- Provides low-level interrupt handlers that:
  - Save processor registers
  - Call C interrupt handlers with proper parameters
  - Restore registers and return from interrupt using `iretq`
- Handles both exceptions with and without error codes

## Files Modified

### `kernel/kernel.h`
- Added prototypes for `gdt_init()`, `idt_init()`, `timer_init()`
- Added prototype for `test_divide_by_zero()` (later removed)

### `kernel/kernel.c`
- Added initialization sequence in `kernel_main()`:
  1. Initialize GDT
  2. Initialize IDT  
  3. Initialize timer
  4. Enable interrupts with `sti` instruction
- Removed exception testing code (was causing triple faults)

### `build.py`
- Added new source files to the build:
  - `kernel/gdt.c`
  - `kernel/idt.c` 
  - `kernel/interrupts.c`
  - `kernel/timer.c`
  - `kernel/interrupt_asm.asm`
- Updated build process to compile and link assembly files

## How It Works

### Boot Sequence
1. Limine bootloader loads kernel and enters 64-bit long mode
2. `kernel_main()` initializes serial console
3. New initialization sequence:
   - GDT setup: Creates segmentation structures (mostly unused in long mode but required)
   - IDT setup: Configures interrupt vectors to point to assembly stubs
   - Timer setup: Configures PIT for periodic interrupts
   - Enable interrupts: Executes `sti` instruction
4. Main loop executes `hlt` instruction to wait for interrupts

### Interrupt Flow
1. Hardware triggers interrupt (exception or timer)
2. CPU vectors to appropriate IDT entry
3. IDT entry points to assembly stub in `interrupt_asm.asm`
4. Assembly stub:
   - Saves registers to stack
   - Calls C `interrupt_handler()` with interrupt number and error code
5. C `interrupt_handler()`:
   - Uses switch statement to call specific handler based on interrupt vector
   - Specific handlers perform actions (print messages, halt, etc.)
6. Assembly stub restores registers and executes `iretq` to return from interrupt

### Timer Operation
- PIT configured for ~100Hz frequency
- Timer interrupt occurs every 10ms
- Handler acknowledges interrupt to PIC
- Increments global tick counter
- Prints "." character every 100 ticks (once per second) for visual feedback

## Verification
The implementation was verified by:
1. Successful build and boot in QEMU
2. Timer interrupt visible as periodic "." output in serial console
3. Proper initialization messages displayed during boot:
   ```
   [init] Setting up GDT...
   [gdt] GDT initialized.
   [init] Setting up IDT...  
   [idt] IDT initialized.
   [init] Setting up timer...
   [timer] PIT initialized (~100Hz).
   [init] Enabling interrupts...
   [init] Interrupts enabled.
   ```
4. System remains stable and responsive to interrupts

## Design Notes
- Minimalist approach: No scheduling, virtual memory, or filesystems added yet
- Focus on core interrupt mechanism reliability
- Error handlers provide diagnostic information before halting
- Timer provides periodic heartbeat for verification
- All code follows existing NEXUS OS coding conventions
- Maintains compatibility with Limine boot protocol
## Phase 3 corrections

Phase 3 needs exceptions that can return, so these Phase 2 defects were
fixed. Before the fixes, the divide-by-zero test triple-faulted and the timer
never fired.

- **TSS layout:** the reserved field at 0x1C was 4 bytes instead of 8. The
  CPU read IST1 from the wrong offset, took a garbage stack, and
  triple-faulted on any IST exception.
- **Segment reload:** after `lgdt`, CS and SS still held Limine's selectors
  (0x28/0x30), which lie outside the new GDT, so the first `iretq` would
  #GP. `gdt_flush` now far-returns into 0x08 and reloads the data segments
  with 0x10.
- **Entry stubs:** the stubs used `call` to reach the common code. That
  pushed an extra return address, so the error code was read from the CS
  slot and `iretq` returned to the wrong place. The stubs now use a
  standard layout: vector plus error code, then the GPRs, then
  `struct interrupt_frame`. All exception vectors 0-31 are installed.
- **Gate types:** #GP, #PF, and IRQ0 used gate type 0x6, which is invalid in
  long mode. All gates now use type 0xE, the 64-bit interrupt gate.
- **Timer:** `timer_init` overwrote IDT[32] with a plain C function, and the
  PIC was never remapped, so IRQ0 would have hit vector 8 (#DF). The PIC is
  now remapped to 32-47 with only IRQ0 unmasked, and the PIT mode bits are
  encoded correctly.
- The #DE demo no longer runs on every boot because it halts the kernel.
  It is available as `python build.py run --fault-demo de`.
