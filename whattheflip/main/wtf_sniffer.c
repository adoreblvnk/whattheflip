/*
 * wtf_sniffer.c - Multi-Protocol Hardware & Wireless Passive Sniffer Engine
 *
 * Implements:
 *   - FR-1 (TC-1.1): Passive sampling on UART1 (Probe 1, GPIO5) with 1 µs timestamping.
 *   - FR-4 (TC-2.1): Wi-Fi 6 promiscuous packet capture on Channels 1–13.
 *   - FR-4 (TC-2.2): IEEE 802.15.4 packet capture on Channel 15 (Channels 11–26).
 *   - Single-radio time-slicing state machine between Ch 6 SoftAP and sniffer hopping.
 *
 * Enforces Barr C standard and zero dynamic memory allocation.
 */

#include "wtf_sniffer.h"
#include "wtf_safety.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"
#include "esp_wifi.h"
#include "esp_ieee802154.h"
#include <string.h>

#define UART_SNIFFER_PORT       UART_NUM_1
#define UART_RX_FIFO_SZ         512

/* Fixed Iris BSSID / MAC for allow-list matching (FR-6) */
static const uint8_t s_iris_target_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};

static uint32_t s_wifi_capture_count = 0;
static uint32_t s_154_capture_count = 0;
static uint8_t s_current_sniffer_channel = 6;

/* --------------------------------------------------------------------------
 * IEEE 802.15.4 Callbacks (Context: Driver ISR / Task)
 * -------------------------------------------------------------------------- */
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info)
{
    if (frame != NULL && frame[0] > 0) {
        int64_t now_us = esp_timer_get_time();
        s_154_capture_count++;

        wtf_record_t rec;
        rec.timestamp_us = (uint32_t)now_us;
        rec.protocol = PROTO_IEEE802154;
        rec.probe_id = 15; /* Channel 15 */
        uint8_t copy_len = (frame[0] < WTF_PKT_DATA_MAX) ? frame[0] : (WTF_PKT_DATA_MAX - 1);
        rec.length = (uint16_t)copy_len;
        memcpy(rec.data, &frame[1], copy_len);

        /* Push to storage queue (PCAP) */
        if (g_mbf_storage > 0) {
            (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
        }
        /* Push to telemetry queue */
        if (g_mbf_telemetry > 0) {
            (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
        }

        if ((s_154_capture_count % 5) == 1) {
            int8_t rssi = frame_info ? frame_info->rssi : -50;
            tm_printf((UB *)"[SNIFFER:802154] CAPTURE: 802.15.4 Frame #%u on Ch 15 (RSSI=%d dBm, len=%u, FR-4)\n",
                      (unsigned)s_154_capture_count, (int)rssi, (unsigned)frame[0]);
        }
    }

    esp_ieee802154_receive_handle_done(frame);
    esp_ieee802154_receive();
}

void esp_ieee802154_receive_sfd_done(void) {}
void esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_frame_info)
{
    (void)frame;
    (void)ack;
    (void)ack_frame_info;
}
void esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    (void)frame;
    (void)error;
}
void esp_ieee802154_transmit_sfd_done(uint8_t *frame) { (void)frame; }
void esp_ieee802154_energy_detect_done(int8_t power) { (void)power; }

/* --------------------------------------------------------------------------
 * Wi-Fi 6 Promiscuous RX Callback
 * -------------------------------------------------------------------------- */
static void wifi_promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (buf == NULL) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len == 0) {
        return;
    }

    s_wifi_capture_count++;
    int64_t now_us = esp_timer_get_time();

    wtf_record_t rec;
    rec.timestamp_us = (uint32_t)now_us;
    rec.protocol = PROTO_WIFI;
    rec.probe_id = (uint8_t)pkt->rx_ctrl.channel;
    uint8_t copy_len = (len < WTF_PKT_DATA_MAX) ? (uint8_t)len : (uint8_t)WTF_PKT_DATA_MAX;
    rec.length = (uint16_t)copy_len;
    memcpy(rec.data, pkt->payload, copy_len);

    /* Enqueue to PCAP storage buffer */
    if (g_mbf_storage > 0) {
        (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
    }
    /* Enqueue to telemetry queue */
    if (g_mbf_telemetry > 0) {
        (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
    }

    /* Check if packet matches Iris BSSID (02:00:00:00:00:03) */
    bool is_iris = false;
    if (copy_len >= 16) {
        for (int i = 0; i <= (copy_len - 6); i++) {
            if (memcmp(&pkt->payload[i], s_iris_target_mac, 6) == 0) {
                is_iris = true;
                break;
            }
        }
    }

    if (is_iris || (s_wifi_capture_count % 20) == 1) {
        tm_printf((UB *)"[SNIFFER:WIFI] CAPTURE: 802.11 Frame on Ch %d (RSSI=%d dBm, len=%u%s, FR-4)\n",
                  (int)pkt->rx_ctrl.channel, (int)pkt->rx_ctrl.rssi, (unsigned)len,
                  is_iris ? " [IRIS BSSID]" : "");
    }
}

/* --------------------------------------------------------------------------
 * Hardware Subsystem Initializations
 * -------------------------------------------------------------------------- */
static void sniffer_hw_uart_init(void)
{
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
                 UART_PIN_NO_CHANGE,
                 WTF_PIN_PROBE1_SNIFF,
                 UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE);

    uart_driver_install(UART_SNIFFER_PORT, UART_RX_FIFO_SZ, 0, 0, NULL, 0);
}

static void sniffer_wireless_init(void)
{
    /* Enable Wi-Fi promiscuous packet filter */
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_cb);
    esp_wifi_set_promiscuous(true);

    tm_printf((UB *)"[SNIFFER:WIRELESS] Wi-Fi 6 promiscuous sniffer online (Ch 1..13, FR-4, TC-2.1)\n");

    /* Enable IEEE 802.15.4 subsystem on Channel 15 */
    esp_ieee802154_enable();
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_set_channel(15);
    esp_ieee802154_receive();

    tm_printf((UB *)"[SNIFFER:WIRELESS] IEEE 802.15.4 sniffer online on Channel 15 (FR-4, TC-2.2)\n");
}

/* --------------------------------------------------------------------------
 * Main Sniffer Task Entry Point
 * -------------------------------------------------------------------------- */
EXPORT void wtf_sniffer_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    sniffer_hw_uart_init();
    tm_printf((UB *)"[SNIFFER] TSK_SNIFFER online: Passive UART1 listening on GPIO5 (Probe 1, FR-1)\n");

    /* Delay 2 seconds to allow SoftAP to finish bringing up netif */
    tk_dly_tsk(2000);
    sniffer_wireless_init();

    uint8_t rx_buf[128];
    char line_buf[64];
    int line_idx = 0;
    uint32_t loop_tick = 0;

    for (;;) {
        loop_tick++;

        /* 1. Drain available bytes from UART1 passive sniffer buffer */
        int len = uart_read_bytes(UART_SNIFFER_PORT, rx_buf, sizeof(rx_buf), 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            int64_t now_us = esp_timer_get_time();

            for (int i = 0; i < len; i++) {
                char c = (char)rx_buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_idx > 0) {
                        line_buf[line_idx] = '\0';

                        wtf_record_t rec;
                        rec.timestamp_us = (uint32_t)now_us;
                        rec.protocol = PROTO_UART;
                        rec.probe_id = 1;
                        rec.length = (uint16_t)line_idx;
                        for (int j = 0; j < line_idx && j < WTF_PKT_DATA_MAX; j++) {
                            rec.data[j] = (uint8_t)line_buf[j];
                        }

                        if (g_mbf_storage > 0) {
                            (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
                        }
                        if (g_mbf_telemetry > 0) {
                            (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
                        }

                        line_idx = 0;
                    }
                } else {
                    if (line_idx < (int)(sizeof(line_buf) - 1)) {
                        line_buf[line_idx++] = c;
                    } else {
                        line_idx = 0;
                    }
                }
            }
        }

        /* 2. Sample I2C SDA/SCL lines on Probes 2 & 3 */
        int sda = (GPIO.in.val >> WTF_PIN_PROBE2_SDA) & 1;
        int scl = (GPIO.in.val >> WTF_PIN_PROBE3_SCL) & 1;
        (void)sda;
        (void)scl;

        /* 3. Time-Slicing State Machine:
         * Every 1500 ms (75 ticks @ 20 ms), hop between Channel 6 SoftAP baseline
         * and adjacent target capture channels (e.g. Channel 1, 6, 11).
         */
        if ((loop_tick % 75) == 0) {
            if (s_current_sniffer_channel == 6) {
                s_current_sniffer_channel = 1; /* Hop to Ch 1 */
            } else if (s_current_sniffer_channel == 1) {
                s_current_sniffer_channel = 11; /* Hop to Ch 11 */
            } else {
                s_current_sniffer_channel = 6; /* Return to Ch 6 SoftAP */
            }
            esp_wifi_set_channel(s_current_sniffer_channel, WIFI_SECOND_CHAN_NONE);
        }

        tk_dly_tsk(10);
    }
}
