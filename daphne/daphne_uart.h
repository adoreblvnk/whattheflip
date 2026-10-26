/*
 * daphne_uart.h - UART0 RX receiver task for Daphne (RP2040 Core 0)
 *
 * Receives telemetry burst from Hestia on GP1 (Row 12) at 115200 8N1.
 * Validates framing and counter checksum.
 */

#ifndef DAPHNE_UART_H
#define DAPHNE_UART_H

#include <tk/tkernel.h>
#include <stdint.h>
#include <stdbool.h>

/* Bit Error Rate (BER) Statistics */
typedef struct {
    uint32_t total_frames;
    uint32_t valid_frames;
    uint32_t corrupt_frames;
    uint32_t total_bit_errors;
    uint32_t total_bits_monitored;
    uint32_t last_frame_bit_errors;
} daphne_ber_stats_t;

/* Start UART0 RX monitoring task */
EXPORT void daphne_uart_rx_task(INT stacd, void *exinf);

/* Query BER statistics */
EXPORT void daphne_uart_get_ber_stats(daphne_ber_stats_t *out_stats);
EXPORT void daphne_uart_reset_ber_stats(void);

#endif /* DAPHNE_UART_H */
