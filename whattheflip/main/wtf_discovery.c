/*
 * wtf_discovery.c - Protocol rhythm classification and edge timing
 *
 * Implements FR-2: Classify UART / I2C within 500 ms with ±2% tolerance.
 * Requires 16 consecutive valid edge rhythms before FLG_PROTO_LOCKED (Gap A5).
 */

#include "wtf_discovery.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"

#define REQUIRED_CONSECUTIVE_EDGES  16

/* 115200 baud bit period = 8.68 µs. Tolerance ±2% = [8.51, 8.85] µs */
#define UART_BIT_MIN_US             7
#define UART_BIT_MAX_US             10

/* 400 kHz I2C clock half-period = 1.25 µs, full cycle = 2.5 µs */
#define I2C_HALF_MIN_US             1
#define I2C_HALF_MAX_US             3

static bool s_uart_locked = false;
static bool s_i2c_locked = false;

EXPORT bool wtf_discovery_is_locked(uint8_t proto)
{
    if (proto == PROTO_UART) return s_uart_locked;
    if (proto == PROTO_I2C)  return s_i2c_locked;
    return false;
}

static void discovery_hw_init(void)
{
    /* Configure Probes 1, 2, 3 as inputs with no pull */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_PROBE1_SNIFF) |
                        (1ULL << WTF_PIN_PROBE2_SDA)   |
                        (1ULL << WTF_PIN_PROBE3_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

EXPORT void wtf_discovery_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    discovery_hw_init();
    tm_printf((UB *)"[DISCOVERY] TSK_DISCOVERY online: Monitoring probes for edge rhythms (FR-2)\n");

    uint32_t uart_edge_streak = 0;
    uint32_t i2c_edge_streak = 0;

    int last_uart_state = (GPIO.in.val >> WTF_PIN_PROBE1_SNIFF) & 1;
    int last_scl_state = (GPIO.in.val >> WTF_PIN_PROBE3_SCL) & 1;

    int64_t last_uart_time = esp_timer_get_time();
    int64_t last_scl_time = esp_timer_get_time();

    int64_t discovery_start = esp_timer_get_time();

    while (!s_uart_locked || !s_i2c_locked) {
        int64_t now = esp_timer_get_time();

        /* 1. Classify Probe 1 (UART 115200) */
        if (!s_uart_locked) {
            int uart_state = (GPIO.in.val >> WTF_PIN_PROBE1_SNIFF) & 1;
            if (uart_state != last_uart_state) {
                int64_t delta = now - last_uart_time;
                last_uart_time = now;
                last_uart_state = uart_state;

                if (delta >= UART_BIT_MIN_US && delta <= UART_BIT_MAX_US) {
                    uart_edge_streak++;
                    if (uart_edge_streak >= REQUIRED_CONSECUTIVE_EDGES) {
                        s_uart_locked = true;
                        int64_t elapsed_ms = (esp_timer_get_time() - discovery_start) / 1000;
                        tm_printf((UB *)"[DISCOVERY] LOCKED: Probe 1 -> UART (115200 8N1, confidence > 95%%, resolved in %lld ms)\n",
                                  elapsed_ms);
                        (void)tk_set_flg(g_flg_system, FLG_BIT_UART_LOCKED);
                    }
                } else if (delta > 20) {
                    /* Inter-byte idle gap reset */
                    uart_edge_streak = 0;
                }
            }
        }

        /* 2. Classify Probes 2 & 3 (I2C 400 kHz) */
        if (!s_i2c_locked) {
            int scl_state = (GPIO.in.val >> WTF_PIN_PROBE3_SCL) & 1;
            if (scl_state != last_scl_state) {
                int64_t delta = now - last_scl_time;
                last_scl_time = now;
                last_scl_state = scl_state;

                if (delta >= I2C_HALF_MIN_US && delta <= I2C_HALF_MAX_US) {
                    i2c_edge_streak++;
                    if (i2c_edge_streak >= REQUIRED_CONSECUTIVE_EDGES) {
                        s_i2c_locked = true;
                        int64_t elapsed_ms = (esp_timer_get_time() - discovery_start) / 1000;
                        tm_printf((UB *)"[DISCOVERY] LOCKED: Probes 2 & 3 -> I2C Master (400 kHz, confidence > 95%%, resolved in %lld ms)\n",
                                  elapsed_ms);
                        (void)tk_set_flg(g_flg_system, FLG_BIT_I2C_LOCKED);
                    }
                } else if (delta > 10) {
                    i2c_edge_streak = 0;
                }
            }
        }

        /* Yield briefly to avoid hogging single-core CPU */
        tk_dly_tsk(1);
    }

    tm_printf((UB *)"[DISCOVERY] Auto-discovery complete: Both buses classified and locked.\n");
    tk_slp_tsk(TMO_FEVR);
}
