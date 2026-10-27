/*
 * wtf_safety.h - Hardware E-Stop and Safety subsystem for whattheflip
 *
 * Implements FR-7: E-stop floats all probes in < 200 ns via GPIO9 ISR.
 * Controls WS2812 status LED (GPIO8) and Piezo Buzzer chirp (EXIO3).
 */

#ifndef WTF_SAFETY_H
#define WTF_SAFETY_H

#include "wtf_types.h"

EXPORT void wtf_safety_hw_init(void);
EXPORT void wtf_safety_task(INT stacd, void *exinf);
EXPORT bool wtf_safety_is_aborted(void);
EXPORT void wtf_safety_abort(void);
EXPORT void wtf_safety_rearm(void);
EXPORT void wtf_safety_set_state(wtf_safety_state_t state);
EXPORT void wtf_safety_neopixel_set_color(uint8_t r, uint8_t g, uint8_t b);
EXPORT void wtf_safety_chirp_buzzer(uint32_t duration_ms);

#endif /* WTF_SAFETY_H */
