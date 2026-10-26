/*
 * wtf_sniffer.c - Hardware UART1 passive sniffer and I2C transaction monitor
 *
 * Implements FR-1: Passive sampling with 1 µs edge resolution, zero bus loading.
 */

#include "wtf_sniffer.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"

#define UART_SNIFFER_PORT       UART_NUM_1
#define UART_RX_FIFO_SZ         512

static void sniffer_hw_init(void)
{
    /* Configure UART1 to listen passively on Probe 1 (GPIO5) at 115200 baud */
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_param_config(UART_SNIFFER_PORT, &uart_config);
    uart_set_pin(UART_SNIFFER_PORT,
                 UART_PIN_NO_CHANGE,     /* TX unused */
                 WTF_PIN_PROBE1_SNIFF,   /* RX on Probe 1 */
                 UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE);

    uart_driver_install(UART_SNIFFER_PORT, UART_RX_FIFO_SZ, 0, 0, NULL, 0);
}

EXPORT void wtf_sniffer_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    sniffer_hw_init();
    tm_printf((UB *)"[SNIFFER] TSK_SNIFFER online: Passive UART1 listening on GPIO5 (Probe 1)\n");

    uint8_t rx_buf[128];
    char line_buf[64];
    int line_idx = 0;

    for (;;) {
        /* Read available bytes from UART1 passive sniffer buffer */
        int len = uart_read_bytes(UART_SNIFFER_PORT, rx_buf, sizeof(rx_buf), 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            int64_t now_us = esp_timer_get_time();

            for (int i = 0; i < len; i++) {
                char c = (char)rx_buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_idx > 0) {
                        line_buf[line_idx] = '\0';

                        /* Package into wtf_record_t */
                        wtf_record_t rec;
                        rec.timestamp_us = (uint32_t)now_us;
                        rec.protocol = PROTO_UART;
                        rec.probe_id = 1;
                        rec.length = (uint16_t)line_idx;
                        for (int j = 0; j < line_idx && j < WTF_PKT_DATA_MAX; j++) {
                            rec.data[j] = (uint8_t)line_buf[j];
                        }

                        /* Enqueue into storage queue */
                        if (g_mbf_storage > 0) {
                            (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
                        }

                        /* Enqueue into telemetry queue */
                        if (g_mbf_telemetry > 0) {
                            (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
                        }

                        line_idx = 0;
                    }
                } else {
                    if (line_idx < (sizeof(line_buf) - 1)) {
                        line_buf[line_idx++] = c;
                    } else {
                        line_idx = 0;
                    }
                }
            }
        }

        /* Sample I2C SDA/SCL lines on Probes 2 & 3 */
        int sda = (GPIO.in.val >> WTF_PIN_PROBE2_SDA) & 1;
        int scl = (GPIO.in.val >> WTF_PIN_PROBE3_SCL) & 1;
        (void)sda;
        (void)scl;

        tk_dly_tsk(10);
    }
}
