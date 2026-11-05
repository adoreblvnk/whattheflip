/*
 * wtf_sniffer.h - Passive packet sniffer for UART and I2C buses
 *
 * Implements FR-1: Passive sampling with 1 µs edge resolution, zero bus loading.
 */

#ifndef WTF_SNIFFER_H
#define WTF_SNIFFER_H

#include "wtf_types.h"

EXPORT void wtf_sniffer_task(INT stacd, void *exinf);

/* Wi-Fi Channel Control (0 = Sweep All Channels 1..13, 1..13 = Lock Channel) */
EXPORT void    wtf_sniffer_set_wifi_channel(uint8_t channel);
EXPORT uint8_t wtf_sniffer_get_wifi_channel(void);
EXPORT uint8_t wtf_sniffer_get_wifi_mode(void);

/* Notify sniffer of active HTTP/PCAP client transfer to hold SoftAP home channel */
EXPORT void    wtf_sniffer_notify_http_activity(void);

/* Live decoded GY-511 I2C sensor values captured passively on Probes 2 & 3 */
EXPORT void    wtf_sniffer_get_i2c_imu(int16_t *ax, int16_t *ay, int16_t *az,
                                       int16_t *mx, int16_t *my, int16_t *mz);

#endif /* WTF_SNIFFER_H */
