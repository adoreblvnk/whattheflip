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
EXPORT void inject_wireless_target_jam_50us(void);

/* Higher-level safe injectors (mutex-protected with feedback) */
EXPORT void wtf_fuzzer_cancel(void);
EXPORT void wtf_fuzzer_inject_uart(void);
EXPORT void wtf_fuzzer_inject_i2c_ack(void);
EXPORT void wtf_fuzzer_inject_i2c_stretch(void);
EXPORT void wtf_fuzzer_inject_wireless_jam(void);
EXPORT void wtf_fuzzer_inject_wifi_jam(uint8_t ch);

/* Over-the-air Iris command transmission (FR-6 targeted 802.15.4 frame) */
EXPORT void wtf_fuzzer_send_iris_cmd(const char *cmd_str);
#endif /* WTF_FUZZER_H */
