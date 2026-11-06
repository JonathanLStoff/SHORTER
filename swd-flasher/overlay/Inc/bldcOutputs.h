// Motor output gate for TIMER0 (included by the patched bldc.c).
//
// Upstream "disables" the outputs with timer_automatic_output_disable(), which only clears OAEN.
// That does not turn anything off: POEN (the master output enable) stays set, so after an invalid
// hall state or a current trip the last duty cycles stayed on the coils. BldcOutputsOff() also
// clears POEN, so all six gates go to their idle level at once.
// BldcOutputsOn() sets OAEN: the hardware sets POEN again at the next update event, but only while
// the break input (PB12, overcurrent) is inactive.
#ifndef BLDC_OUTPUTS_H
#define BLDC_OUTPUTS_H

#define BLDC_CCHP  (*(volatile uint32_t *)(TIMER_BLDC + 0x44u))   // TIMER0_CCHP
#define BLDC_POEN  (1u << 15)
#define BLDC_OAEN  (1u << 14)

static inline void BldcOutputsOff(void) { BLDC_CCHP &= ~(BLDC_OAEN | BLDC_POEN); }
static inline void BldcOutputsOn(void)  { BLDC_CCHP |= BLDC_OAEN; }

#endif
