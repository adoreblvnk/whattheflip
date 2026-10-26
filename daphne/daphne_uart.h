/*
 * daphne_uart.h - UART0 RX receiver task for Daphne (RP2040 Core 0)
 *
 * Receives telemetry burst from Hestia on GP1 (Row 12) at 115200 8N1.
 * Validates framing and counter checksum.
 */

#ifndef DAPHNE_UART_H
#define DAPHNE_UART_H

#include <tk/tkernel.h>

/* Start UART0 RX monitoring task */
EXPORT void daphne_uart_rx_task(INT stacd, void *exinf);

#endif /* DAPHNE_UART_H */
