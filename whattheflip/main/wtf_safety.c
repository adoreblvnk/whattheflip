/*
 * wtf_safety.c - E-Stop ISR, safety monitor, WS2812 LED, and buzzer subsystem
 *
 * Implements FR-7: Probes float within 200 ns upon GPIO9 edge.
 * Manages WS2812 status LED (GPIO8) and Piezo Buzzer chirp (TCA9554 EXIO3).
 */

#include "wtf_safety.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_rom_serial_output.h"
#include "wtf_fuzzer.h"
#include "wtf_sniffer.h"
#include "wtf_discovery.h"
#include "wtf_dashboard.h"
#include <string.h>
#include <stdlib.h>

static volatile bool s_safety_latched = false;
static wtf_safety_state_t s_safety_state = WTF_STATE_ARMED;
/* --------------------------------------------------------------------------
 * Hardware Pinout Note (ESP32-C6-Pico on Maker Pi Base)
 * Only blue GPIO indicator LEDs and jumpered microSD (GPIO18..21) work on
 * whattheflip; the piezo buzzer is driven directly by Daphne on GP18.
 * -------------------------------------------------------------------------- */

EXPORT void wtf_safety_chirp_buzzer(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        duration_ms = 20;
    }
    tm_printf((UB *)"[SAFETY] Alert logged (%lu ms; piezo alarm is on Daphne GP18)\n", (unsigned long)duration_ms);
}

/* --------------------------------------------------------------------------
 * WS2812 Neopixel Driver on GPIO8
 * -------------------------------------------------------------------------- */

static inline uint32_t disable_cpu_interrupts(void)
{
    uint32_t prev_mstatus;
    asm volatile ("csrrc %0, mstatus, %1" : "=r"(prev_mstatus) : "r"(MSTATUS_MIE) : "memory");
    return prev_mstatus;
}

static inline void restore_cpu_interrupts(uint32_t prev_mstatus)
{
    if (prev_mstatus & MSTATUS_MIE) {
        asm volatile ("csrrs zero, mstatus, %0" :: "r"(MSTATUS_MIE) : "memory");
    }
}

static void ws2812_send_byte(uint8_t val)
{
    for (int i = 7; i >= 0; i--) {
        if ((val >> i) & 1U) {
            /* Bit 1: High ~700 ns (112 cycles @ 160 MHz), Low ~600 ns (96 cycles) */
            GPIO.out_w1ts.val = (1UL << WTF_PIN_NEOPIXEL);
            uint32_t t_start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - t_start) < 112U) {}
            GPIO.out_w1tc.val = (1UL << WTF_PIN_NEOPIXEL);
            t_start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - t_start) < 96U) {}
        } else {
            /* Bit 0: High ~180 ns (22 cycles @ 160 MHz + APB write), Low ~800 ns */
            GPIO.out_w1ts.val = (1UL << WTF_PIN_NEOPIXEL);
            uint32_t t_start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - t_start) < 22U) {}
            GPIO.out_w1tc.val = (1UL << WTF_PIN_NEOPIXEL);
            t_start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - t_start) < 128U) {}
        }
    }
}

EXPORT void wtf_safety_neopixel_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    /* WS2812 protocol requires GRB ordering */
    uint32_t mstatus = disable_cpu_interrupts();
    ws2812_send_byte(g);
    ws2812_send_byte(r);
    ws2812_send_byte(b);
    restore_cpu_interrupts(mstatus);

    /* Reset latch: hold line low for > 80 µs */
    esp_rom_delay_us(80);
}

static void safety_apply_stealth_leds(bool stealth)
{
    /* Float Probes 0..3 (GPIO4..7) without pull-ups so blue GPIO indicator LEDs turn completely off */
    if (stealth) {
        GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) |
                               (1UL << WTF_PIN_PROBE1_SNIFF)  |
                               (1UL << WTF_PIN_PROBE2_SDA)    |
                               (1UL << WTF_PIN_PROBE3_SCL)    |
                               (1UL << WTF_PIN_SD_SCK)        |
                               (1UL << WTF_PIN_SD_MOSI)       |
                               (1UL << WTF_PIN_SD_CS)         |
                               (1UL << WTF_PIN_TCA9554_SDA)   |
                               (1UL << WTF_PIN_TCA9554_SCL);
        static const uint8_t k_stealth_pins[] = {
            0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23
        };
        for (size_t i = 0; i < sizeof(k_stealth_pins); i++) {
            gpio_num_t p = (gpio_num_t)k_stealth_pins[i];
            gpio_set_direction(p, GPIO_MODE_INPUT);
            gpio_pullup_dis(p);
            gpio_pulldown_dis(p);
        }
        /* Send (0,0,0) twice and drive NeoPixel data line LOW */
        wtf_safety_neopixel_set_color(0, 0, 0);
        wtf_safety_neopixel_set_color(0, 0, 0);
        GPIO.out_w1tc.val = (1UL << WTF_PIN_NEOPIXEL);
    } else {
        /* Restore SD SPI pins when leaving stealth mode */
        gpio_set_direction((gpio_num_t)WTF_PIN_SD_CS, GPIO_MODE_OUTPUT);
        gpio_set_direction((gpio_num_t)WTF_PIN_SD_SCK, GPIO_MODE_OUTPUT);
        gpio_set_direction((gpio_num_t)WTF_PIN_SD_MOSI, GPIO_MODE_OUTPUT);
        GPIO.out_w1ts.val = (1UL << WTF_PIN_SD_CS) | (1UL << WTF_PIN_SD_MOSI);
        GPIO.out_w1tc.val = (1UL << WTF_PIN_SD_SCK);
    }
}

EXPORT void wtf_safety_set_state(wtf_safety_state_t state)
{
    s_safety_state = state;

    switch (state) {
        case WTF_STATE_ARMED:
            wtf_safety_neopixel_set_color(0, 48, 0);
            break;
        case WTF_STATE_INJECTING:
            wtf_safety_neopixel_set_color(48, 36, 0);
            break;
        case WTF_STATE_ABORTED:
            /* Stealth mode: turn off NeoPixel and all blue GPIO indicator LEDs */
            safety_apply_stealth_leds(true);
            break;
        default:
            break;
    }
}

/* --------------------------------------------------------------------------
 * Hardware E-Stop ISR & Control
 * -------------------------------------------------------------------------- */

/* Hardware ISR executed on GPIO9 falling edge (BOOT button press) */
static void IRAM_ATTR estop_gpio_isr(void *arg)
{
    (void)arg;

    /* 1. Hardware atomic output disable across all 4 probe pins.
     * Direct register write executes in ~6.25 ns at 160 MHz, well below the 200 ns gate.
     */
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) |
                           (1UL << WTF_PIN_PROBE1_SNIFF)  |
                           (1UL << WTF_PIN_PROBE2_SDA)    |
                           (1UL << WTF_PIN_PROBE3_SCL);

    s_safety_latched = true;

    /* 2. Wake safety task via event flag */
    if (g_flg_system > 0) {
        (void)tk_set_flg(g_flg_system, FLG_BIT_SAFETY_ABORT);
    }
}

EXPORT void wtf_safety_hw_init(void)
{
    /* Configure E-Stop pin: GPIO9, Input, Pull-up, Negative edge interrupt */
    gpio_config_t estop_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_ESTOP),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&estop_conf);

    /* Configure Neopixel pin: GPIO8, Output */
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_NEOPIXEL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    GPIO.out_w1tc.val = (1UL << WTF_PIN_NEOPIXEL);

    /* Maker Pi buttons/expander lines remain high-Z inputs */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(WTF_PIN_ESTOP, estop_gpio_isr, NULL);
}

EXPORT bool wtf_safety_is_aborted(void)
{
    return s_safety_latched;
}

EXPORT void wtf_safety_abort(void)
{
    /* 1. Float all probes in < 200 ns and cancel any active glitching */
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) |
                           (1UL << WTF_PIN_PROBE1_SNIFF)  |
                           (1UL << WTF_PIN_PROBE2_SDA)    |
                           (1UL << WTF_PIN_PROBE3_SCL);

    s_safety_latched = true;
    wtf_fuzzer_cancel();
    safety_apply_stealth_leds(true);

    /* 2. Signal safety task */
    if (g_flg_system > 0) {
        (void)tk_set_flg(g_flg_system, FLG_BIT_SAFETY_ABORT);
    }
}

EXPORT void wtf_safety_rearm(void)
{
    s_safety_latched = false;
    if (g_flg_system > 0) {
        (void)tk_clr_flg(g_flg_system, ~FLG_BIT_SAFETY_ABORT);
    }
    safety_apply_stealth_leds(false);
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[SAFETY] SYSTEM ACTIVE: Probes restored, monitoring resumed\n");
}

static void safety_handle_serial_line(const char *line)
{
    tm_printf((UB *)"[SAFETY:CMD] '%s'\n", line);
    if (strncmp(line, "MODE:", 5) == 0) {
        const char *m = line + 5;
        if (strcasecmp(m, "off") == 0) wtf_discovery_set_tool_mode(WTF_TOOL_MODE_OFF);
        else if (strcasecmp(m, "auto") == 0) wtf_discovery_set_tool_mode(WTF_TOOL_MODE_AUTO);
        else if (strcasecmp(m, "uart") == 0) wtf_discovery_set_tool_mode(WTF_TOOL_MODE_UART);
        else if (strcasecmp(m, "i2c") == 0) wtf_discovery_set_tool_mode(WTF_TOOL_MODE_I2C);
        else if (strcasecmp(m, "wifi") == 0) wtf_discovery_set_tool_mode(WTF_TOOL_MODE_WIFI);
        else if (strcasecmp(m, "zigbee") == 0) wtf_discovery_set_tool_mode(WTF_TOOL_MODE_ZIGBEE);
    } else if (strcmp(line, "BAUD:AUTO") == 0 || strcmp(line, "UARTAUTO") == 0) {
        wtf_sniffer_request_uart_autodetect();
    } else if (strncmp(line, "BAUD:", 5) == 0) {
        uint32_t b = (uint32_t)strtoul(line + 5, NULL, 10);
        wtf_sniffer_set_uart_baud(b);
    } else if (strncmp(line, "CH", 2) == 0) {
        int ch = atoi(line + 2);
        if (ch >= 1 && ch <= 13) wtf_sniffer_request_wifi_burst((uint8_t)ch);
    } else if (strncmp(line, "JAMWIFI:", 8) == 0) {
        int ch = atoi(line + 8);
        if (ch >= 1 && ch <= 13) wtf_fuzzer_inject_wifi_jam((uint8_t)ch);
    } else if (strncmp(line, "ZCH", 3) == 0) {
        int zch = atoi(line + 3);
        if (zch >= 11 && zch <= 26) wtf_sniffer_set_154_channel((uint8_t)zch);
    } else if (strcmp(line, "CLEAR") == 0) {
        wtf_fuzzer_cancel();
        wtf_sniffer_clear_stats();
        wtf_dashboard_clear_stats();
    } else if (strcmp(line, "ESTOP") == 0) {
        wtf_safety_abort();
    } else if (strcmp(line, "REARM") == 0) {
        wtf_safety_rearm();
    }
}

EXPORT void wtf_safety_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    wtf_safety_hw_init();
    wtf_safety_set_state(WTF_STATE_ARMED);

    tm_printf((UB *)"[SAFETY] TSK_SAFETY online: E-stop monitored on GPIO9 (200 ns gate)\n");

    char ser_line[48];
    int ser_idx = 0;

    for (;;) {
        UINT flgptn = 0;
        ER er = tk_wai_flg(g_flg_system, FLG_BIT_SAFETY_ABORT,
                           TWF_ANDW | TWF_CLR, &flgptn, 50);
        uint8_t rx_char = 0;
        while ((REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL) != 0 ||
               esp_rom_output_rx_one_char(&rx_char) == 0) {
            if ((REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL) != 0) {
                rx_char = (uint8_t)(REG_READ(USB_SERIAL_JTAG_EP1_REG) & 0xFF);
            }
            if (rx_char == '\n' || rx_char == '\r') {
                if (ser_idx == 1) {
                    char c = ser_line[0];
                    if (c == 'E' || c == 'e') wtf_safety_abort();
                    else if (c == 'R' || c == 'r') wtf_safety_rearm();
                    else if (c == 'C' || c == 'c') {
                        wtf_fuzzer_cancel();
                        wtf_sniffer_clear_stats();
                        wtf_dashboard_clear_stats();
                    }
                    else if (c == 'U' || c == 'u') wtf_fuzzer_inject_uart();
                    else if (c == 'A' || c == 'a') wtf_fuzzer_inject_i2c_ack();
                    else if (c == 'S' || c == 's') wtf_fuzzer_inject_i2c_stretch();
                    else if (c == 'W' || c == 'w') wtf_fuzzer_inject_wireless_jam();
                    else if (c == 'J' || c == 'j') wtf_fuzzer_inject_wifi_jam(wtf_sniffer_get_wifi_channel());
                } else if (ser_idx > 1) {
                    ser_line[ser_idx] = '\0';
                    safety_handle_serial_line(ser_line);
                }
                ser_idx = 0;
            } else if (ser_idx < (int)(sizeof(ser_line) - 1)) {
                ser_line[ser_idx++] = (char)rx_char;
            } else {
                ser_idx = 0;
            }
        }
        /* Bare single-char trigger only if not followed by another byte within 20 ms */
        if (ser_idx == 1) {
            tk_dly_tsk(20);
            if ((REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL) == 0) {
                char c = ser_line[0];
                if (c == 'E' || c == 'e' || c == 'R' || c == 'r' || c == 'C' || c == 'c' ||
                    c == 'U' || c == 'u' || c == 'A' || c == 'a' || c == 'S' || c == 's' ||
                    c == 'W' || c == 'w' || c == 'J' || c == 'j') {
                    if (c == 'E' || c == 'e') wtf_safety_abort();
                    else if (c == 'R' || c == 'r') wtf_safety_rearm();
                    else if (c == 'C' || c == 'c') {
                        wtf_fuzzer_cancel();
                        wtf_sniffer_clear_stats();
                        wtf_dashboard_clear_stats();
                    }
                    else if (c == 'U' || c == 'u') wtf_fuzzer_inject_uart();
                    else if (c == 'A' || c == 'a') wtf_fuzzer_inject_i2c_ack();
                    else if (c == 'S' || c == 's') wtf_fuzzer_inject_i2c_stretch();
                    else if (c == 'W' || c == 'w') wtf_fuzzer_inject_wireless_jam();
                    else if (c == 'J' || c == 'j') wtf_fuzzer_inject_wifi_jam(wtf_sniffer_get_wifi_channel());
                    ser_idx = 0;
                }
            }
        }
        if (er == E_OK && (flgptn & FLG_BIT_SAFETY_ABORT)) {
            tm_printf((UB *)"[SAFETY:ALERT] HARDWARE E-STOP ACTIVATED (Stealth Mode: LEDs OFF, Probes High-Z)!\n");
            wtf_safety_set_state(WTF_STATE_ABORTED);
            tk_dly_tsk(5);

            while (s_safety_latched) {
                GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) |
                                       (1UL << WTF_PIN_PROBE1_SNIFF)  |
                                       (1UL << WTF_PIN_PROBE2_SDA)    |
                                       (1UL << WTF_PIN_PROBE3_SCL);
                uint8_t c = 0;
                bool got_rearm = false;
                if ((REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL) != 0) {
                    c = (uint8_t)(REG_READ(USB_SERIAL_JTAG_EP1_REG) & 0xFF);
                    got_rearm = (c == 'R' || c == 'r');
                } else if (esp_rom_output_rx_one_char(&c) == 0) {
                    got_rearm = (c == 'R' || c == 'r');
                }
                if (got_rearm) {
                    wtf_safety_rearm();
                    break;
                }
                tk_dly_tsk(50);
            }
        }
    }
}
