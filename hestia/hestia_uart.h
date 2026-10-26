/*
 * hestia_uart.h - UART0 TX transmitter task for Hestia (RP2040 Core 0)
 *
 * Emits periodic telemetry bursts on GP0 (Row 10) at 115200 baud, 8N1.
 * Supports:
 * - Rate agility: default 2 Hz, configurable (1 Hz, 2 Hz, 10 Hz).
 * - Pattern modes: alternating 0xAA and 0x55 bursts alongside canonical
 *   "HESTIA:<counter>:<checksum>\n" with checksum = (uint16_t)(~counter).
 * - Strict Barr C compliance: zero dynamic memory allocation.
 */

#ifndef HESTIA_UART_H
#define HESTIA_UART_H

#include <tk/tkernel.h>
#include <stdint.h>
#include <stdbool.h>

#if __has_include("packet_types.h")
#include "packet_types.h"
#elif __has_include("../shared/packet_types.h")
#include "../shared/packet_types.h"
#elif __has_include("../../shared/packet_types.h")
#include "../../shared/packet_types.h"
#endif

/* Rate Agility Constants */
#define HESTIA_RATE_1HZ             1U
#define HESTIA_RATE_2HZ             2U
#define HESTIA_RATE_5HZ             5U
#define HESTIA_RATE_10HZ            10U

#ifndef HESTIA_DEFAULT_RATE_HZ
#define HESTIA_DEFAULT_RATE_HZ      HESTIA_RATE_2HZ
#endif

/* Operating Modes */
#ifndef WTF_PACKET_TYPES_H
typedef enum {
    WTF_UART_MODE_CANONICAL   = 0, /* "HESTIA:<counter>:<checksum>\n" */
    WTF_UART_MODE_PATTERN_AA  = 1, /* Continuous 0xAA burst (10101010b) */
    WTF_UART_MODE_PATTERN_55  = 2, /* Continuous 0x55 burst (01010101b) */
    WTF_UART_MODE_ALT_BURST   = 3, /* Alternating 0xAA and 0x55 bursts */
    WTF_UART_MODE_INTERLEAVED = 4  /* Canonical frame + alternating 0xAA/0x55 stress burst */
} wtf_uart_mode_t;
#endif

#define HESTIA_MODE_CANONICAL       WTF_UART_MODE_CANONICAL
#define HESTIA_MODE_PATTERN_AA      WTF_UART_MODE_PATTERN_AA
#define HESTIA_MODE_PATTERN_55      WTF_UART_MODE_PATTERN_55
#define HESTIA_MODE_ALT_BURST       WTF_UART_MODE_ALT_BURST
#define HESTIA_MODE_INTERLEAVED     WTF_UART_MODE_INTERLEAVED

#ifndef HESTIA_DEFAULT_MODE
#define HESTIA_DEFAULT_MODE         HESTIA_MODE_CANONICAL
#endif

/* Task entry point */
EXPORT void hestia_uart_tx_task(INT stacd, void *exinf);

/* Rate Configuration API */
EXPORT ER hestia_uart_set_rate(uint32_t rate_hz);
EXPORT uint32_t hestia_uart_get_rate(void);

/* Test Mode Configuration API */
EXPORT ER hestia_uart_set_mode(uint8_t mode);
EXPORT uint8_t hestia_uart_get_mode(void);

#endif /* HESTIA_UART_H */
