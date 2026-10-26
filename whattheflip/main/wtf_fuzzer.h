/*
 * wtf_fuzzer.h - Controlled fault injector for UART and I2C buses
 *
 * Implements FR-5: Faults bounded: 10 µs bit flip, 50 µs ACK kill, 5% duty cycle.
 */

#ifndef WTF_FUZZER_H
#define WTF_FUZZER_H

#include "wtf_types.h"

EXPORT void wtf_fuzzer_task(INT stacd, void *exinf);

#endif /* WTF_FUZZER_H */
