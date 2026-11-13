#ifndef BHARAT_SMP_H
#define BHARAT_SMP_H

/* Starting the other CPUs ("application processors", APs).
 *
 * Limine already sent the INIT/SIPI wake-up messages and parked every AP
 * in 64-bit mode, each spinning on a `goto_address`. smp_init gives each
 * one an idle thread (whose stack it starts on) and its struct percpu,
 * then writes the address of ap_entry. The AP loads our page tables,
 * its own GDT/TSS, the shared IDT, turns on its local APIC and timer,
 * reports online and enters the scheduler's idle loop. CPUs are started
 * one at a time.
 *
 * Limits: SMP_MAX_CPUS CPUs; CPUs cannot be taken offline again. */

#include <stdbool.h>

/* `mp_response` is Limine's struct limine_mp_response (or NULL: one CPU).
 * Needs the scheduler running. Returns the number of CPUs online. */
unsigned smp_init(void *mp_response);

#endif /* BHARAT_SMP_H */
