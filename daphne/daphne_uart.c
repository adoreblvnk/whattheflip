/*
 * daphne_uart.c - UART0 RX receiver implementation for Daphne (RP2040 Core 0)
 *
 * Invariants:
 * - Dedicated to Processor 1 (Core 0) under TA_ASSPRC.
 * - GP1 = UART0 RX (Row 12).
 * - Zero dynamic memory allocation.
 * - Validates checksum: expected == (uint16_t)(~counter).
 * - Implements Bit Error Rate (BER) tracking: counts bit differences between
 *   received corrupted frames and expected format, logging BER metrics during
 *   active fuzzing.
 */

#include "daphne_uart.h"
#include <tm/tmonitor.h>
#include <sys/sysdef.h>
#include <stdint.h>
#include <stdbool.h>

#if __has_include("packet_types.h")
#include "packet_types.h"
#elif __has_include("../shared/packet_types.h")
#include "../shared/packet_types.h"
#elif __has_include("../../shared/packet_types.h")
#include "../../shared/packet_types.h"
#endif

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

/* Static BER Tracking State */
LOCAL daphne_ber_stats_t s_ber_stats;
LOCAL uint32_t s_last_counter = 0;

EXPORT void daphne_uart_get_ber_stats(daphne_ber_stats_t *out_stats)
{
    if (out_stats != NULL) {
        *out_stats = s_ber_stats;
    }
}

EXPORT void daphne_uart_reset_ber_stats(void)
{
    s_ber_stats.total_frames = 0;
    s_ber_stats.valid_frames = 0;
    s_ber_stats.corrupt_frames = 0;
    s_ber_stats.total_bit_errors = 0;
    s_ber_stats.total_bits_monitored = 0;
    s_ber_stats.last_frame_bit_errors = 0;
}

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

LOCAL int daphne_format_expected(char *buf, uint32_t counter, uint16_t checksum)
{
    const char prefix[] = "HESTIA:";
    int idx = 0;

    for (int i = 0; prefix[i] != '\0'; i++) {
        buf[idx++] = prefix[i];
    }

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

    const char hex_chars[] = "0123456789ABCDEF";
    buf[idx++] = hex_chars[(checksum >> 12) & 0x0F];
    buf[idx++] = hex_chars[(checksum >> 8)  & 0x0F];
    buf[idx++] = hex_chars[(checksum >> 4)  & 0x0F];
    buf[idx++] = hex_chars[checksum & 0x0F];

    buf[idx] = '\0';
    return idx;
}

LOCAL uint32_t daphne_calc_bit_diff(const char *rx, int rx_len, const char *exp, int exp_len)
{
    uint32_t bit_diff = 0;
    int min_len = (rx_len < exp_len) ? rx_len : exp_len;

    for (int i = 0; i < min_len; i++) {
        uint8_t diff = (uint8_t)rx[i] ^ (uint8_t)exp[i];
        while (diff > 0) {
            if (diff & 1) {
                bit_diff++;
            }
            diff >>= 1;
        }
    }

    /* Account for length differences (8 bits per missing/extra character) */
    int len_delta = (rx_len > exp_len) ? (rx_len - exp_len) : (exp_len - rx_len);
    bit_diff += (uint32_t)(len_delta * 8);

    return bit_diff;
}

LOCAL bool daphne_check_test_pattern(const char *buf, int len, uint8_t *out_pat)
{
    if (len < 4) {
        return false;
    }
    uint8_t first = (uint8_t)buf[0];
    if (first != 0xAA && first != 0x55) {
        return false;
    }
    for (int i = 1; i < len; i++) {
        if ((uint8_t)buf[i] != first) {
            return false;
        }
    }
    *out_pat = first;
    return true;
}

EXPORT void daphne_uart_rx_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    daphne_uart_hw_init();
    daphne_uart_reset_ber_stats();

    tm_printf((UB *)"[DAPHNE:CORE0] UART RX task online with BER tracking (Processor %d, GP1 @ 115200)\n",
              tk_get_prc());

    LOCAL char rx_line[DAPHNE_RX_BUF_SIZE];
    LOCAL char exp_line[DAPHNE_RX_BUF_SIZE];
    uint32_t rx_idx = 0;

    for (;;) {
        /* Read characters until RX FIFO is empty */
        while ((in_w(DAPHNE_UART_FR) & DAPHNE_UART_FR_RXFE) == 0) {
            char ch = (char)(in_w(DAPHNE_UART_DR) & 0xFF);

            if (ch == '\n' || ch == '\r') {
                if (rx_idx > 0) {
                    rx_line[rx_idx] = '\0';
                    s_ber_stats.total_frames++;

                    /* Check if incoming burst is a stress test bit pattern */
                    uint8_t pattern_byte = 0;
                    if (daphne_check_test_pattern(rx_line, (int)rx_idx, &pattern_byte)) {
                        tm_printf((UB *)"[DAPHNE:UART] TEST PATTERN BURST: 0x%02X (length=%u bytes)\n",
                                  pattern_byte, rx_idx);
                        rx_idx = 0;
                        continue;
                    }

                    uint32_t counter = 0;
                    uint16_t csum = 0;
                    bool parsed = daphne_parse_line(rx_line, &counter, &csum);

                    if (parsed) {
                        uint16_t expected_csum = (uint16_t)(~counter);
                        int exp_len = daphne_format_expected(exp_line, counter, expected_csum);
                        uint32_t frame_bits = (uint32_t)(exp_len * 8);

                        if (csum == expected_csum) {
                            /* Frame completely valid */
                            s_ber_stats.valid_frames++;
                            s_ber_stats.total_bits_monitored += frame_bits;
                            s_ber_stats.last_frame_bit_errors = 0;
                            s_last_counter = counter;

                            if ((s_ber_stats.valid_frames % 5) == 0 || s_ber_stats.valid_frames <= 3) {
                                tm_printf((UB *)"[DAPHNE:UART] PKT #%u VALID (total=%u, valid=%u, corrupt=%u)\n",
                                          counter, s_ber_stats.total_frames, s_ber_stats.valid_frames, s_ber_stats.corrupt_frames);
                            }
                        } else {
                            /* Checksum corrupted by glitch */
                            s_ber_stats.corrupt_frames++;
                            uint32_t err_bits = daphne_calc_bit_diff(rx_line, (int)rx_idx, exp_line, exp_len);
                            if (err_bits == 0) {
                                err_bits = 1; /* Checksum was wrong, at least 1 bit flip */
                            }
                            s_ber_stats.last_frame_bit_errors = err_bits;
                            s_ber_stats.total_bit_errors += err_bits;
                            s_ber_stats.total_bits_monitored += frame_bits;
                            s_last_counter = counter;

                            uint32_t ber_x100 = 0;
                            if (s_ber_stats.total_bits_monitored > 0) {
                                ber_x100 = (uint32_t)(((uint64_t)s_ber_stats.total_bit_errors * 10000ULL) /
                                                      s_ber_stats.total_bits_monitored);
                            }

                            tm_printf((UB *)"[DAPHNE:UART] CORRUPT CHECKSUM: ctr=%u csum=0x%04X exp=0x%04X raw=[%s]\n",
                                      counter, csum, expected_csum, rx_line);
                            tm_printf((UB *)"[DAPHNE:BER] ERR_BITS=%u | CUMULATIVE: %u/%u bits | BER=%u.%02u%% | FRAMES: val=%u cor=%u\n",
                                      err_bits, s_ber_stats.total_bit_errors, s_ber_stats.total_bits_monitored,
                                      ber_x100 / 100, ber_x100 % 100,
                                      s_ber_stats.valid_frames, s_ber_stats.corrupt_frames);
                        }
                    } else {
                        /* Framing corrupted: estimate expected counter from sequence */
                        s_ber_stats.corrupt_frames++;
                        uint32_t exp_ctr = (s_last_counter > 0) ? (s_last_counter + 1) : 1;
                        uint16_t exp_csum = (uint16_t)(~exp_ctr);
                        int exp_len = daphne_format_expected(exp_line, exp_ctr, exp_csum);
                        uint32_t frame_bits = (uint32_t)(exp_len * 8);

                        uint32_t err_bits = daphne_calc_bit_diff(rx_line, (int)rx_idx, exp_line, exp_len);
                        if (err_bits == 0) {
                            err_bits = 8;
                        }
                        s_ber_stats.last_frame_bit_errors = err_bits;
                        s_ber_stats.total_bit_errors += err_bits;
                        s_ber_stats.total_bits_monitored += frame_bits;

                        uint32_t ber_x100 = 0;
                        if (s_ber_stats.total_bits_monitored > 0) {
                            ber_x100 = (uint32_t)(((uint64_t)s_ber_stats.total_bit_errors * 10000ULL) /
                                                  s_ber_stats.total_bits_monitored);
                        }

                        tm_printf((UB *)"[DAPHNE:UART] CORRUPT FRAME: len=%u raw=[%s]\n",
                                  rx_idx, rx_line);
                        tm_printf((UB *)"[DAPHNE:BER] ERR_BITS=%u | CUMULATIVE: %u/%u bits | BER=%u.%02u%% | FRAMES: val=%u cor=%u\n",
                                  err_bits, s_ber_stats.total_bit_errors, s_ber_stats.total_bits_monitored,
                                  ber_x100 / 100, ber_x100 % 100,
                                  s_ber_stats.valid_frames, s_ber_stats.corrupt_frames);
                    }
                    rx_idx = 0;
                }
            } else {
                if (rx_idx < (DAPHNE_RX_BUF_SIZE - 1)) {
                    rx_line[rx_idx++] = ch;
                } else {
                    /* Overflow from sustained noise or glitch */
                    rx_line[rx_idx] = '\0';
                    s_ber_stats.corrupt_frames++;
                    s_ber_stats.total_frames++;
                    s_ber_stats.total_bit_errors += (rx_idx * 8);
                    s_ber_stats.total_bits_monitored += (rx_idx * 8);
                    tm_printf((UB *)"[DAPHNE:UART] BUFFER OVERFLOW: raw=[%s]\n", rx_line);
                    rx_idx = 0;
                }
            }
        }

        /* Yield briefly to kernel */
        tk_dly_tsk(10);
    }
}
