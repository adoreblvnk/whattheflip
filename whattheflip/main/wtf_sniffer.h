/*
 * wtf_sniffer.h - Passive packet sniffer for UART and I2C buses
 *
 * Implements FR-1: Passive sampling with 1 µs edge resolution, zero bus loading.
 */

#ifndef WTF_SNIFFER_H
#define WTF_SNIFFER_H

#include "wtf_types.h"

EXPORT void wtf_sniffer_task(INT stacd, void *exinf);

/* Wi-Fi Channel Control & Fast Off-Channel Burst (1..13) */
EXPORT void    wtf_sniffer_set_wifi_channel(uint8_t channel);
EXPORT void    wtf_sniffer_request_wifi_burst(uint8_t channel);
EXPORT uint8_t wtf_sniffer_get_wifi_channel(void);
EXPORT uint8_t wtf_sniffer_get_wifi_mode(void);

/* UART Dynamic Baud-Rate Hunting (9600, 38400, 57600, 115200) */
EXPORT void     wtf_sniffer_set_uart_baud(uint32_t baud);
EXPORT uint32_t wtf_sniffer_get_uart_baud(void);
EXPORT void     wtf_sniffer_request_uart_autodetect(void);

/* IEEE 802.15.4 / Zigbee Channel Control (11..26) */
EXPORT void    wtf_sniffer_set_154_channel(uint8_t channel);
EXPORT uint8_t wtf_sniffer_get_154_channel(void);

/* Notify sniffer of active HTTP/PCAP client transfer to hold SoftAP home channel */
EXPORT void    wtf_sniffer_notify_http_activity(void);

/* Live decoded GY-511 I2C sensor values captured passively on Probes 2 & 3 */
EXPORT void    wtf_sniffer_get_i2c_imu(int16_t *ax, int16_t *ay, int16_t *az,
                                       int16_t *mx, int16_t *my, int16_t *mz);

/* Iris Proximity & Verified Location Tracking (FR-6) */
EXPORT int8_t  wtf_sniffer_get_iris_rssi(void);
EXPORT bool    wtf_sniffer_is_iris_verified(void);

/* CTF Flag & Protocol Hunt Telemetry Accessors */
EXPORT void    wtf_sniffer_clear_stats(void);
EXPORT void    wtf_sniffer_mark_exploit(uint8_t proto);
EXPORT void    wtf_sniffer_on_wifi_jam(uint8_t ch);
EXPORT void    wtf_sniffer_on_154_jam(uint8_t ch);
EXPORT void    wtf_sniffer_get_ctf_snapshot(
    char *uart_sample, size_t uart_sz, bool *uart_clear,
    char *wifi_sample, size_t wifi_sz, bool *wifi_clear, uint8_t *wifi_sample_ch,
    char *z154_sample, size_t z154_sz, bool *z154_clear,
    char *flag_uart, char *flag_i2c, char *flag_wifi, char *flag_z154, char *flag_bonus,
    size_t flag_sz, uint8_t *intercept_mask, uint8_t *exploit_mask);

#endif /* WTF_SNIFFER_H */
