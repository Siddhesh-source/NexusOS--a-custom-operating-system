#ifndef NEXUS_TIMER_H
#define NEXUS_TIMER_H

#include <stdint.h>

void timer_init(void);
void timer_wait(uint32_t ticks);
void timer_irq(void);

extern volatile uint64_t timer_ticks;

#endif /* NEXUS_TIMER_H */