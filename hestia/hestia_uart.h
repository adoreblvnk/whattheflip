/*
 * hestia_uart.h - UART0 TX transmitter task for Hestia (RP2040 Core 0)
 *
 * Emits periodic telemetry bursts on GP0 (Row 10) at 115200 baud, 8N1.
 * Format: "HESTIA:<counter>:<checksum>\n" with checksum = ~counter.
 */

#ifndef HESTIA_UART_H
#define HESTIA_UART_H

#include <tk/tkernel.h>

/* Start UART0 TX transmitter task */
EXPORT void hestia_uart_tx_task(INT stacd, void *exinf);

#endif /* HESTIA_UART_H */
