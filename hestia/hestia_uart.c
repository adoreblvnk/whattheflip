/*
 * hestia_uart.c - UART0 TX transmitter implementation for Hestia (RP2040 Core 0)
 *
 * Invariants:
 * - Dedicated to Processor 1 (Core 0) under TA_ASSPRC.
 * - GP0 = UART0 TX (Row 10).
 * - Transmits at 2 Hz (500 ms interval).
 * - Format: "HESTIA:<counter>:<checksum>\n" with checksum = ~counter.
 * - Strict Barr C compliance: no dynamic memory allocation.
 */

#include "hestia_uart.h"
#include <tm/tmonitor.h>
#include <sys/sysdef.h>
#include <stdint.h>
#include <stdbool.h>

/* RP2040 UART0 Register Definitions */
#define HESTIA_UART0_BASE       0x40034000UL
#define HESTIA_UART_DR          (HESTIA_UART0_BASE + 0x000)
#define HESTIA_UART_FR          (HESTIA_UART0_BASE + 0x018)
#define HESTIA_UART_IBRD        (HESTIA_UART0_BASE + 0x024)
#define HESTIA_UART_FBRD        (HESTIA_UART0_BASE + 0x028)
#define HESTIA_UART_LCR_H       (HESTIA_UART0_BASE + 0x02C)
#define HESTIA_UART_CR          (HESTIA_UART0_BASE + 0x030)

#define HESTIA_UART_FR_TXFF     (1 << 5) /* Transmit FIFO full */
#define HESTIA_UART_FR_BUSY     (1 << 3) /* UART busy */

#define HESTIA_BUF_SIZE         64

LOCAL void hestia_uart_hw_init(void)
{
    /* Reset UART0 peripheral */
    set_w(RESETS_RESET, RESETS_RESET_UART0);
    clr_w(RESETS_RESET, RESETS_RESET_UART0);
    while ((in_w(RESETS_RESET_DONE) & RESETS_RESET_UART0) == 0) {
        /* Wait for reset completion */
    }

    /* Configure GP0 to UART0 TX (function 2) */
    out_w(GPIO_CTRL(0), GPIO_CTRL_FUNCSEL_UART);
    out_w(GPIO(0), GPIO_DRIVE_4MA | GPIO_SHEMITT);

    /* 115200 baud at 125 MHz sys_clk:
     * Divisor = 125,000,000 / (16 * 115200) = 67.816666...
     * IBRD = 67, FBRD = round(0.816666 * 64) = 52.
     */
    out_w(HESTIA_UART_IBRD, 67);
    out_w(HESTIA_UART_FBRD, 52);

    /* 8 bits, 1 stop bit, no parity, FIFO enable (0x70) */
    out_w(HESTIA_UART_LCR_H, (3 << 5) | (1 << 4));

    /* Enable UART and TX (0x101) */
    out_w(HESTIA_UART_CR, (1 << 8) | (1 << 0));
}

LOCAL int hestia_format_packet(char *buf, uint32_t counter, uint16_t checksum)
{
    /* Deterministic formatting without libc sprintf allocation:
     * "HESTIA:<counter>:<checksum>\n"
     */
    const char prefix[] = "HESTIA:";
    int idx = 0;

    for (int i = 0; prefix[i] != '\0'; i++) {
        buf[idx++] = prefix[i];
    }

    /* Convert decimal counter */
    char num_rev[12];
    int num_len = 0;
    uint32_t c = counter;
    if (c == 0) {
        num_rev[num_len++] = '0';
    } else {
        while (c > 0) {
            num_rev[num_len++] = (char)('0' + (c % 10));
            c /= 10;
        }
    }
    while (num_len > 0) {
        buf[idx++] = num_rev[--num_len];
    }

    buf[idx++] = ':';

    /* Convert 4-digit hex checksum */
    const char hex_chars[] = "0123456789ABCDEF";
    buf[idx++] = hex_chars[(checksum >> 12) & 0x0F];
    buf[idx++] = hex_chars[(checksum >> 8) & 0x0F];
    buf[idx++] = hex_chars[(checksum >> 4) & 0x0F];
    buf[idx++] = hex_chars[checksum & 0x0F];

    buf[idx++] = '\n';
    buf[idx] = '\0';

    return idx;
}

LOCAL void hestia_uart_send_string(const char *str, int len)
{
    for (int i = 0; i < len; i++) {
        /* Wait until TX FIFO has space */
        int timeout = 50000;
        while ((in_w(HESTIA_UART_FR) & HESTIA_UART_FR_TXFF) != 0 && --timeout > 0) {
            /* Spin wait */
        }
        out_w(HESTIA_UART_DR, (uint32_t)((uint8_t)str[i]));
    }
}

EXPORT void hestia_uart_tx_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    hestia_uart_hw_init();
    tm_printf((UB *)"[HESTIA:CORE0] UART TX task online (Processor %d, GP0 @ 115200, 2 Hz)\n", tk_get_prc());

    uint32_t counter = 0;
    LOCAL char pkt_buf[HESTIA_BUF_SIZE];

    for (;;) {
        counter++;
        uint16_t checksum = (uint16_t)(~counter);

        int pkt_len = hestia_format_packet(pkt_buf, counter, checksum);
        hestia_uart_send_string(pkt_buf, pkt_len);

        /* Log telemetry every 5 packets or first 3 packets */
        if ((counter % 5) == 0 || counter <= 3) {
            tm_printf((UB *)"[HESTIA:TX] PKT #%u (csum=0x%04X, len=%d)\n",
                      counter, checksum, pkt_len);
        }

        /* 500 ms periodic interval (2 Hz transmit rate) */
        tk_dly_tsk(500);
    }
}
