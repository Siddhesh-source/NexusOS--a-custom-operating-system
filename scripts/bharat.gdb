# GDB setup for `python build.py debug` (QEMU paused on port 1234).
# Usage, from the repo folder:  gdb -x scripts/bharat.gdb
#
# Hardware breakpoints (hbreak) are used because the kernel's pages are
# mapped by the bootloader after QEMU starts; a software breakpoint set
# this early would be written into memory that is not loaded yet.

set architecture i386:x86-64
set pagination off
file build/bharat.elf
target remote localhost:1234

hbreak kernel_main
continue

# Try next:
#   hbreak context_switch     stop at every thread switch
#   continue                  run to it
#   info registers rsp rip    see the old thread's stack and position
#   stepi                     step one instruction through the switch
#   bt                        call trace (frame pointers are kept)
