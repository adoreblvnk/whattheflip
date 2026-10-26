/*
 * wtf_safety.h - Hardware E-Stop and Safety subsystem for whattheflip
 *
 * Implements FR-7: E-stop floats all probes in < 200 ns via GPIO9 ISR.
 */

#ifndef WTF_SAFETY_H
#define WTF_SAFETY_H

#include "wtf_types.h"

EXPORT void wtf_safety_hw_init(void);
EXPORT void wtf_safety_task(INT stacd, void *exinf);
EXPORT bool wtf_safety_is_aborted(void);

#endif /* WTF_SAFETY_H */
