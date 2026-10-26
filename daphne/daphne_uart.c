/*
 * daphne_uart.c - UART0 RX receiver implementation for Daphne (RP2040 Core 0)
 *
 * Invariants:
 * - Dedicated to Processor 1 (Core 0) under TA_ASSPRC.
 * - GP1 = UART0 RX (Row 12).
 * - No dynamic memory allocation.
 * - Validates checksum: expected == (uint16_t)(~counter).
 */

#include "daphne_uart.h"
#include <tm/tmonitor.h>
#include <sys/sysdef.h>
#include <stdint.h>
#include <stdbool.h>

/* RP2040 UART0 Register Definitions */
#define DAPHNE_UART0_BASE       0x40034000UL
#define DAPHNE_UART_DR          (DAPHNE_UART0_BASE + 0x000)
#define DAPHNE_UART_FR          (DAPHNE_UART0_BASE + 0x018)
#define DAPHNE_UART_IBRD        (DAPHNE_UART0_BASE + 0x024)
#define DAPHNE_UART_FBRD        (DAPHNE_UART0_BASE + 0x028)
#define DAPHNE_UART_LCR_H       (DAPHNE_UART0_BASE + 0x02C)
#define DAPHNE_UART_CR          (DAPHNE_UART0_BASE + 0x030)

#define DAPHNE_UART_FR_RXFE     (1 << 4)

/* Buffer size for single UART line */
#define DAPHNE_RX_BUF_SIZE      128

LOCAL void daphne_uart_hw_init(void)
{
    /* Reset UART0 peripheral */
    set_w(RESETS_RESET, RESETS_RESET_UART0);
    clr_w(RESETS_RESET, RESETS_RESET_UART0);
    while ((in_w(RESETS_RESET_DONE) & RESETS_RESET_UART0) == 0) {
        /* Wait for reset completion */
    }

    /* Route GP0 to UART0 TX and GP1 to UART0 RX */
    out_w(GPIO_CTRL(0), GPIO_CTRL_FUNCSEL_UART);
    out_w(GPIO_CTRL(1), GPIO_CTRL_FUNCSEL_UART);
    out_w(GPIO(1), GPIO_IE | GPIO_DRIVE_4MA | GPIO_SHEMITT);

    /* 115200 baud at 125 MHz sys_clk:
     * Divisor = 125,000,000 / (16 * 115200) = 67.816666...
     * IBRD = 67, FBRD = round(0.816666 * 64) = 52.
     */
    out_w(DAPHNE_UART_IBRD, 67);
    out_w(DAPHNE_UART_FBRD, 52);

    /* 8 bits, 1 stop bit, no parity, FIFO enable (0x70) */
    out_w(DAPHNE_UART_LCR_H, (3 << 5) | (1 << 4));

    /* Enable UART, RX and TX (0x301) */
    out_w(DAPHNE_UART_CR, (1 << 9) | (1 << 8) | (1 << 0));
}

LOCAL bool daphne_parse_line(const char *buf, uint32_t *out_ctr, uint16_t *out_csum)
{
    /* Find start of "HESTIA:" */
    const char *p = buf;
    while (*p != '\0') {
        if (p[0] == 'H' && p[1] == 'E' && p[2] == 'S' &&
            p[3] == 'T' && p[4] == 'I' && p[5] == 'A' && p[6] == ':') {
            p += 7;
            break;
        }
        p++;
    }
    if (*p == '\0') {
        return false;
    }

    /* Parse counter digits */
    uint32_t ctr = 0;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        ctr = (ctr * 10) + (uint32_t)(*p - '0');
        p++;
    }

    if (*p != ':') {
        return false;
    }
    p++;

    /* Parse hex checksum (up to 4 hex digits) */
    uint16_t csum = 0;
    int digits = 0;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        char c = *p;
        uint8_t nibble = 0;
        if (c >= '0' && c <= '9') {
            nibble = (uint8_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            nibble = (uint8_t)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            nibble = (uint8_t)(c - 'a' + 10);
        } else {
            break;
        }
        csum = (uint16_t)((csum << 4) | nibble);
        digits++;
        p++;
    }

    if (digits == 0) {
        return false;
    }

    *out_ctr = ctr;
    *out_csum = csum;
    return true;
}

EXPORT void daphne_uart_rx_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    daphne_uart_hw_init();
    tm_printf((UB *)"[DAPHNE:CORE0] UART RX task online (Processor %d, GP1 @ 115200)\n", tk_get_prc());

    LOCAL char rx_line[DAPHNE_RX_BUF_SIZE];
    uint32_t rx_idx = 0;
    uint32_t valid_pkts = 0;
    uint32_t corrupt_pkts = 0;
    uint32_t total_pkts = 0;

    for (;;) {
        /* Read characters until RX FIFO is empty */
        while ((in_w(DAPHNE_UART_FR) & DAPHNE_UART_FR_RXFE) == 0) {
            char ch = (char)(in_w(DAPHNE_UART_DR) & 0xFF);

            if (ch == '\n' || ch == '\r') {
                if (rx_idx > 0) {
                    rx_line[rx_idx] = '\0';
                    total_pkts++;

                    uint32_t counter = 0;
                    uint16_t csum = 0;
                    if (daphne_parse_line(rx_line, &counter, &csum)) {
                        uint16_t expected = (uint16_t)(~counter);
                        if (csum == expected) {
                            valid_pkts++;
                            if ((valid_pkts % 5) == 0 || valid_pkts <= 3) {
                                tm_printf((UB *)"[DAPHNE:UART] PKT #%u VALID (total=%u, valid=%u, corrupt=%u)\n",
                                          counter, total_pkts, valid_pkts, corrupt_pkts);
                            }
                        } else {
                            corrupt_pkts++;
                            tm_printf((UB *)"[DAPHNE:UART] CORRUPT CHECKSUM: ctr=%u csum=0x%04X exp=0x%04X raw=[%s]\n",
                                      counter, csum, expected, rx_line);
                        }
                    } else {
                        corrupt_pkts++;
                        tm_printf((UB *)"[DAPHNE:UART] CORRUPT FRAME: len=%u raw=[%s]\n",
                                  rx_idx, rx_line);
                    }
                    rx_idx = 0;
                }
            } else {
                if (rx_idx < (DAPHNE_RX_BUF_SIZE - 1)) {
                    rx_line[rx_idx++] = ch;
                } else {
                    /* Overflow from noise or sustained glitch */
                    rx_line[rx_idx] = '\0';
                    corrupt_pkts++;
                    total_pkts++;
                    tm_printf((UB *)"[DAPHNE:UART] BUFFER OVERFLOW: raw=[%s]\n", rx_line);
                    rx_idx = 0;
                }
            }
        }

        /* Yield briefly to kernel */
        tk_dly_tsk(10);
    }
}
