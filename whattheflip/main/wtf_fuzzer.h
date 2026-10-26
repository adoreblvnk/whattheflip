/*
 * wtf_fuzzer.h - Controlled fault injector for UART and I2C buses
 *
 * Implements FR-5: Faults bounded: 10 µs bit flip, 50 µs ACK kill, 50 µs clock-stretch, 5% duty budget.
 */

#ifndef WTF_FUZZER_H
#define WTF_FUZZER_H

#include "wtf_types.h"

EXPORT void wtf_fuzzer_task(INT stacd, void *exinf);

/* Low-level glitch injectors */
EXPORT void inject_uart_bit_flip_10us(void);
EXPORT void inject_i2c_ack_kill_50us(void);
EXPORT void inject_i2c_clock_stretch_50us(void);

/* Higher-level safe injectors (mutex-protected with LED & buzzer feedback) */
EXPORT void wtf_fuzzer_inject_uart(void);
EXPORT void wtf_fuzzer_inject_i2c_ack(void);
EXPORT void wtf_fuzzer_inject_i2c_stretch(void);

#endif /* WTF_FUZZER_H */
