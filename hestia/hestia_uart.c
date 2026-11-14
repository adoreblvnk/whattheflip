/*
 * hestia_uart.c - UART0 TX transmitter implementation for Hestia (RP2040 Core 0)
 *
 * Invariants:
 * - Dedicated to Processor 1 (Core 0) under TA_ASSPRC.
 * - GP0 = UART0 TX (Row 10), GP1 = UART0 RX (optional runtime command input).
 * - Rate agility: default 2 Hz (500 ms), configurable (1 Hz, 2 Hz, 10 Hz).
 * - Test bit pattern modes: alternating 0xAA and 0x55 bursts for edge stress
 *   testing alongside canonical "HESTIA:<counter>:<checksum>\n".
 * - Strict Barr C compliance: no dynamic memory allocation, static buffers.
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
#define HESTIA_UART_FR_RXFE     (1 << 4) /* Receive FIFO empty */
#define HESTIA_UART_FR_BUSY     (1 << 3) /* UART busy */

#define HESTIA_BUF_SIZE         80
#define HESTIA_BURST_LEN        16
#define HESTIA_FLAG_STR         "WTF{hands_off_the_altar}"

/* RP2040 Hardware Random & Timer Registers for dynamic boot baud selection */
#define RP2040_ROSC_RANDOMBIT   0x4006001CUL
#define RP2040_TIMERAWL         0x40054028UL

/* Static State */
LOCAL volatile uint32_t s_tx_rate_hz   = HESTIA_DEFAULT_RATE_HZ;
LOCAL volatile uint32_t s_tx_period_ms = 1000U / HESTIA_DEFAULT_RATE_HZ;
LOCAL volatile uint8_t  s_tx_mode      = HESTIA_DEFAULT_MODE;
LOCAL volatile uint32_t s_tx_baud      = HESTIA_BAUD_115200;
EXPORT ER hestia_uart_set_rate(uint32_t rate_hz)
{
    if (rate_hz == 0U || rate_hz > 100U) {
        return E_PAR;
    }
    s_tx_rate_hz = rate_hz;
    s_tx_period_ms = 1000U / rate_hz;
    if (s_tx_period_ms == 0U) {
        s_tx_period_ms = 10U;
    }
    tm_printf((UB *)"[HESTIA:CFG] Transmission rate updated: %u Hz (%u ms interval)\n",
              (unsigned)s_tx_rate_hz, (unsigned)s_tx_period_ms);
    return E_OK;
}

EXPORT uint32_t hestia_uart_get_rate(void)
{
    return s_tx_rate_hz;
}

EXPORT ER hestia_uart_set_mode(uint8_t mode)
{
    if (mode > HESTIA_MODE_INTERLEAVED) {
        return E_PAR;
    }
    s_tx_mode = mode;
    const char *mode_str = "UNKNOWN";
    switch (mode) {
    case HESTIA_MODE_CANONICAL:   mode_str = "CANONICAL (ASCII)"; break;
    case HESTIA_MODE_PATTERN_AA:  mode_str = "PATTERN 0xAA"; break;
    case HESTIA_MODE_PATTERN_55:  mode_str = "PATTERN 0x55"; break;
    case HESTIA_MODE_ALT_BURST:   mode_str = "ALT BURST 0xAA/0x55"; break;
    case HESTIA_MODE_INTERLEAVED: mode_str = "INTERLEAVED (ASCII + BURST)"; break;
    default: break;
    }
    tm_printf((UB *)"[HESTIA:CFG] Test mode updated: %u (%s)\n", (unsigned)mode, mode_str);
    return E_OK;
}

EXPORT uint8_t hestia_uart_get_mode(void)
{
    return s_tx_mode;
}
LOCAL void hestia_apply_baud_regs(uint32_t baud)
{
    uint32_t ibrd = 67U;
    uint32_t fbrd = 52U;
    switch (baud) {
    case 9600U:
        ibrd = 813U;
        fbrd = 51U;
        break;
    case 38400U:
        ibrd = 203U;
        fbrd = 29U;
        break;
    case 57600U:
        ibrd = 135U;
        fbrd = 41U;
        break;
    case 115200U:
    default:
        ibrd = 67U;
        fbrd = 52U;
        break;
    }

    /* Wait until TX FIFO / shifter idle before changing divisor */
    int wait_loops = 100000;
    while ((in_w(HESTIA_UART_FR) & HESTIA_UART_FR_BUSY) != 0 && --wait_loops > 0) {
    }
    out_w(HESTIA_UART_CR, 0U);
    out_w(HESTIA_UART_IBRD, ibrd);
    out_w(HESTIA_UART_FBRD, fbrd);
    out_w(HESTIA_UART_LCR_H, (3U << 5) | (1U << 4));
    out_w(HESTIA_UART_CR, (1U << 9) | (1U << 8) | (1U << 0));
}

EXPORT ER hestia_uart_set_baud(uint32_t baud)
{
    if (baud != 9600U && baud != 38400U && baud != 57600U && baud != 115200U) {
        return E_PAR;
    }
    s_tx_baud = baud;
    hestia_apply_baud_regs(baud);
    tm_printf((UB *)"[HESTIA:CFG] UART0 baud rate set to %u bps\n", (unsigned)baud);
    return E_OK;
}

EXPORT uint32_t hestia_uart_get_baud(void)
{
    return s_tx_baud;
}

LOCAL uint32_t hestia_pick_random_baud(void)
{
    static const uint32_t k_bauds[4] = { 9600U, 38400U, 57600U, 115200U };
    uint32_t entropy = in_w(RP2040_TIMERAWL);
    for (int i = 0; i < 16; i++) {
        entropy = (entropy << 1) ^ (in_w(RP2040_ROSC_RANDOMBIT) & 1UL) ^ in_w(RP2040_TIMERAWL);
        for (volatile int d = 0; d < 47; d++) {
        }
    }
    return k_bauds[entropy & 3U];
}

#define HESTIA_LED_MASK         0x000003FCUL /* GP2..GP9 on Maker Pi Base */

LOCAL void hestia_uart_hw_init(void)
{
    /* Reset UART0 peripheral */
    set_w(RESETS_RESET, RESETS_RESET_UART0);
    clr_w(RESETS_RESET, RESETS_RESET_UART0);
    while ((in_w(RESETS_RESET_DONE) & RESETS_RESET_UART0) == 0) {
        /* Wait for reset completion */
    }

    /* Configure GP0 to UART0 TX and GP1 to UART0 RX (function 2) */
    out_w(GPIO_CTRL(0), GPIO_CTRL_FUNCSEL_UART);
    out_w(GPIO(0), GPIO_DRIVE_4MA | GPIO_SHEMITT);
    out_w(GPIO_CTRL(1), GPIO_CTRL_FUNCSEL_UART);
    out_w(GPIO(1), GPIO_IE | GPIO_DRIVE_4MA | GPIO_SHEMITT);

    /* Configure Maker Pi Base GP2..GP9 LEDs as SIO outputs for TX sweep feedback */
    for (uint32_t pin = 2U; pin <= 9U; pin++) {
        out_w(GPIO_CTRL(pin), GPIO_CTRL_FUNCSEL_SIO);
        out_w(GPIO(pin), GPIO_DRIVE_4MA);
    }
    out_w(GPIO_OUT_CLR, HESTIA_LED_MASK);
    out_w(GPIO_OE_SET, HESTIA_LED_MASK);

    /* Pick dynamic random baud rate from {9600, 38400, 57600, 115200} */
    s_tx_baud = hestia_pick_random_baud();
    hestia_apply_baud_regs(s_tx_baud);
}

LOCAL int hestia_format_packet(char *buf, uint32_t counter, uint16_t checksum)
{
    /* Deterministic formatting without libc sprintf allocation:
     * "HESTIA:<counter>:<checksum>:WTF{hands_off_the_altar}\n"
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
    buf[idx++] = hex_chars[(checksum >> 8)  & 0x0F];
    buf[idx++] = hex_chars[(checksum >> 4)  & 0x0F];
    buf[idx++] = hex_chars[checksum & 0x0F];

    buf[idx++] = ':';
    const char flag[] = HESTIA_FLAG_STR;
    for (int i = 0; flag[i] != '\0'; i++) {
        buf[idx++] = flag[i];
    }

    buf[idx++] = '\n';
    buf[idx] = '\0';
    return idx;
}

LOCAL void hestia_uart_send_string(const char *str, int len)
{
    for (int i = 0; i < len; i++) {
        int timeout = 50000;
        while ((in_w(HESTIA_UART_FR) & HESTIA_UART_FR_TXFF) != 0 && --timeout > 0) {
            /* Spin wait */
        }
        out_w(HESTIA_UART_DR, (uint32_t)((uint8_t)str[i]));
    }
}

LOCAL void hestia_uart_send_burst(uint8_t byte_val, int count)
{
    for (int i = 0; i < count; i++) {
        int timeout = 50000;
        while ((in_w(HESTIA_UART_FR) & HESTIA_UART_FR_TXFF) != 0 && --timeout > 0) {
            /* Spin wait */
        }
        out_w(HESTIA_UART_DR, (uint32_t)byte_val);
    }
}

LOCAL void hestia_uart_check_rx_cmds(void)
{
    while ((in_w(HESTIA_UART_FR) & HESTIA_UART_FR_RXFE) == 0) {
        char ch = (char)(in_w(HESTIA_UART_DR) & 0xFF);
        switch (ch) {
        case '1':
            (void)hestia_uart_set_rate(HESTIA_RATE_1HZ);
            break;
        case '2':
            (void)hestia_uart_set_rate(HESTIA_RATE_2HZ);
            break;
        case '5':
            (void)hestia_uart_set_rate(HESTIA_RATE_5HZ);
            break;
        case '0':
        case '9':
            (void)hestia_uart_set_rate(HESTIA_RATE_10HZ);
            break;
        case 'C':
        case 'c':
            (void)hestia_uart_set_mode(HESTIA_MODE_CANONICAL);
            break;
        case 'A':
        case 'a':
            (void)hestia_uart_set_mode(HESTIA_MODE_PATTERN_AA);
            break;
        case 'U':
        case 'u':
            (void)hestia_uart_set_mode(HESTIA_MODE_PATTERN_55);
            break;
        case 'T':
        case 't':
            (void)hestia_uart_set_mode(HESTIA_MODE_ALT_BURST);
            break;
        case 'I':
        case 'i':
            (void)hestia_uart_set_mode(HESTIA_MODE_INTERLEAVED);
            break;
        case 'B':
        case 'b': {
            static const uint32_t k_bauds[4] = { 9600U, 38400U, 57600U, 115200U };
            uint32_t next = k_bauds[0];
            for (int i = 0; i < 4; i++) {
                if (k_bauds[i] == s_tx_baud) {
                    next = k_bauds[(i + 1) & 3];
                    break;
                }
            }
            (void)hestia_uart_set_baud(next);
            break;
        }
        default:
            break;
        }
    }
}

EXPORT void hestia_uart_tx_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    hestia_uart_hw_init();
    tm_printf((UB *)"[HESTIA:CORE0] UART TX task online (Processor %d, GP0 @ %u baud, default %u Hz, mode=%u)\n",
              tk_get_prc(), (unsigned)s_tx_baud, (unsigned)s_tx_rate_hz, (unsigned)s_tx_mode);
    uint32_t counter = 0;
    LOCAL char pkt_buf[HESTIA_BUF_SIZE];

    for (;;) {
        /* Process any runtime control commands */
        hestia_uart_check_rx_cmds();

        counter++;
        uint16_t checksum = (uint16_t)(~counter);
        int pkt_len = hestia_format_packet(pkt_buf, counter, checksum);

        uint8_t current_mode = s_tx_mode;
        switch (current_mode) {
        case HESTIA_MODE_CANONICAL:
            hestia_uart_send_string(pkt_buf, pkt_len);
            break;

        case HESTIA_MODE_PATTERN_AA:
            hestia_uart_send_burst(0xAAU, HESTIA_BURST_LEN);
            break;

        case HESTIA_MODE_PATTERN_55:
            hestia_uart_send_burst(0x55U, HESTIA_BURST_LEN);
            break;

        case HESTIA_MODE_ALT_BURST: {
            uint8_t pat = (counter & 1U) ? 0xAAU : 0x55U;
            hestia_uart_send_burst(pat, HESTIA_BURST_LEN);
            break;
        }

        case HESTIA_MODE_INTERLEAVED: {
            /* Send canonical packet followed by alternating edge stress burst */
            hestia_uart_send_string(pkt_buf, pkt_len);
            uint8_t pat = (counter & 1U) ? 0xAAU : 0x55U;
            hestia_uart_send_burst(pat, HESTIA_BURST_LEN);
            break;
        }

        default:
            hestia_uart_send_string(pkt_buf, pkt_len);
            break;
        }

        /* Sweep Maker Pi Base GP2..GP9 blue LEDs on every transmitted frame */
        uint32_t active_led_pin = 2U + (counter & 7U);
        out_w(GPIO_OUT_CLR, HESTIA_LED_MASK);
        out_w(GPIO_OUT_SET, (1UL << active_led_pin));


        /* Dynamic rate delay (e.g. 1000 ms @ 1 Hz, 500 ms @ 2 Hz, 100 ms @ 10 Hz) */
        tk_dly_tsk((RELTIM)s_tx_period_ms);
    }
}
