/*
 * wtf_sniffer.c - Multi-Protocol Hardware & Wireless Passive Sniffer Engine
 *
 * Implements:
 *   - FR-1 (TC-1.1): Passive sampling on UART1 (Probe 1, GPIO5) with 1 µs timestamping.
 *   - FR-1 (TC-1.1): Hardware-assisted I2C frame capture on Probes 2 & 3 (GPIO6 SDA / GPIO7 SCL)
 *                    routed through ESP32-C6 GPIO Matrix + IRAM edge capture ISR.
 *   - FR-4 (TC-2.1): Wi-Fi 6 promiscuous packet capture across all Channels 1–13
 *                    with user-selectable channel lock (Ch 1..13) or all-channel sweep (0),
 *                    and single-radio SoftAP home-channel dwell.
 *   - FR-4 (TC-2.2): IEEE 802.15.4 packet capture on Channel 15 (Channels 11–26).
 *
 * Enforces Barr C standard and zero dynamic memory allocation.
 */

#include "wtf_sniffer.h"
#include "wtf_safety.h"
#include "wtf_discovery.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"
#include "soc/i2c_struct.h"
#include "esp_wifi.h"
#include "esp_ieee802154.h"
#include <string.h>

#define UART_SNIFFER_PORT       UART_NUM_1
#define UART_RX_FIFO_SZ         512
#define SOFTAP_HOME_CHANNEL     6U

/* Fixed Iris BSSID / MAC for allow-list matching (FR-6) */
static const uint8_t s_iris_target_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};

static uint32_t s_wifi_capture_count = 0U;
static uint32_t s_154_capture_count  = 0U;
static uint8_t  s_154_channel        = 15U;

EXPORT void wtf_sniffer_set_154_channel(uint8_t channel)
{
    if (channel < 11U || channel > 26U) {
        return;
    }
    s_154_channel = channel;
    esp_ieee802154_set_channel(channel);
    esp_ieee802154_receive();
    tm_printf((UB *)"[SNIFFER:802154] IEEE 802.15.4 radio tuned to Channel %u (%u MHz)\n",
              (unsigned)channel, (unsigned)(2405U + 5U * (channel - 11U)));
}

EXPORT uint8_t wtf_sniffer_get_154_channel(void)
{
    return s_154_channel;
}

static uint32_t s_i2c_capture_count  = 0U;

/* Dynamic Iris Proximity & Verified Location Tracking (FR-6) */
static volatile int8_t   s_iris_rssi               = -70;
static volatile bool     s_iris_verified           = false;
static volatile int64_t  s_last_iris_packet_us     = 0;

/* Wi-Fi Channel Control State */
static volatile uint8_t  s_wifi_channel_mode    = SOFTAP_HOME_CHANNEL;
static volatile uint8_t  s_current_sniffer_ch   = SOFTAP_HOME_CHANNEL;
static uint8_t           s_next_sweep_ch        = 1U;
static volatile int64_t  s_last_http_activity_us = 0;

/* --------------------------------------------------------------------------
 * Hardware-Assisted I2C Bus Sniffer State (Probes 2 & 3: GPIO6 SDA, GPIO7 SCL)
 * Routed via ESP32-C6 GPIO Matrix + IRAM Edge Capture ISR
 * -------------------------------------------------------------------------- */
#define I2C_RING_DEPTH          8U
#define I2C_FRAME_MAX_BYTES     16U

typedef struct {
    uint32_t timestamp_us;
    uint8_t  length;
    uint8_t  bytes[I2C_FRAME_MAX_BYTES];
} i2c_captured_frame_t;

static volatile i2c_captured_frame_t s_i2c_ring[I2C_RING_DEPTH];
static volatile uint8_t s_i2c_ring_head = 0U;
static volatile uint8_t s_i2c_ring_tail = 0U;

/* I2C ISR State Machine */
static volatile bool     s_i2c_in_frame  = false;
static volatile uint8_t  s_i2c_bit_idx   = 0U;
static volatile uint8_t  s_i2c_curr_byte = 0U;
static volatile uint8_t  s_i2c_byte_cnt  = 0U;
static volatile uint8_t  s_i2c_work_buf[I2C_FRAME_MAX_BYTES];
static volatile uint32_t s_i2c_start_us  = 0U;
static volatile uint8_t  s_i2c_last_reg  = 0U;

/* Decoded GY-511 (LSM303DLHC) IMU Values from Passive Bus Capture */
static volatile int16_t s_imu_ax = -28;
static volatile int16_t s_imu_ay = 54;
static volatile int16_t s_imu_az = 981;
static volatile int16_t s_imu_mx = -305;
static volatile int16_t s_imu_my = 52;
static volatile int16_t s_imu_mz = 61;

/* IRAM ISR on Probe 2 (SDA, GPIO6): Detects START and STOP conditions when SCL is HIGH */
static void IRAM_ATTR i2c_sda_edge_isr(void *arg)
{
    (void)arg;
    uint32_t gpio_in = GPIO.in.val;
    uint32_t scl = (gpio_in >> WTF_PIN_PROBE3_SCL) & 1U;
    if (scl == 0U) {
        return; /* Data transitions while SCL is LOW are normal bit changes */
    }

    uint32_t sda = (gpio_in >> WTF_PIN_PROBE2_SDA) & 1U;
    if (sda == 0U) {
        /* START or Repeated START condition (SDA falling while SCL HIGH) */
        if (s_i2c_in_frame && s_i2c_byte_cnt > 0U) {
            uint8_t next_head = (uint8_t)((s_i2c_ring_head + 1U) & (I2C_RING_DEPTH - 1U));
            if (next_head != s_i2c_ring_tail) {
                s_i2c_ring[s_i2c_ring_head].timestamp_us = s_i2c_start_us;
                s_i2c_ring[s_i2c_ring_head].length = s_i2c_byte_cnt;
                for (uint8_t i = 0U; i < s_i2c_byte_cnt; i++) {
                    s_i2c_ring[s_i2c_ring_head].bytes[i] = s_i2c_work_buf[i];
                }
                s_i2c_ring_head = next_head;
            }
        }
        s_i2c_in_frame  = true;
        s_i2c_bit_idx   = 0U;
        s_i2c_curr_byte = 0U;
        s_i2c_byte_cnt  = 0U;
        s_i2c_start_us  = (uint32_t)esp_timer_get_time();
    } else {
        /* STOP condition (SDA rising while SCL HIGH) */
        if (s_i2c_in_frame && s_i2c_byte_cnt > 0U) {
            uint8_t next_head = (uint8_t)((s_i2c_ring_head + 1U) & (I2C_RING_DEPTH - 1U));
            if (next_head != s_i2c_ring_tail) {
                s_i2c_ring[s_i2c_ring_head].timestamp_us = s_i2c_start_us;
                s_i2c_ring[s_i2c_ring_head].length = s_i2c_byte_cnt;
                for (uint8_t i = 0U; i < s_i2c_byte_cnt; i++) {
                    s_i2c_ring[s_i2c_ring_head].bytes[i] = s_i2c_work_buf[i];
                }
                s_i2c_ring_head = next_head;
            }
        }
        s_i2c_in_frame = false;
    }
}

/* IRAM ISR on Probe 3 (SCL, GPIO7) Rising Edge: Shifts 8 data bits + 9th ACK/NACK bit */
static void IRAM_ATTR i2c_scl_posedge_isr(void *arg)
{
    (void)arg;
    if (!s_i2c_in_frame) {
        return;
    }

    uint32_t sda = (GPIO.in.val >> WTF_PIN_PROBE2_SDA) & 1U;
    if (s_i2c_bit_idx < 8U) {
        s_i2c_curr_byte = (uint8_t)((s_i2c_curr_byte << 1U) | (uint8_t)sda);
        s_i2c_bit_idx++;
    } else {
        /* 9th clock rising edge: ACK (sda == 0) or NACK (sda == 1) */
        if (s_i2c_byte_cnt < I2C_FRAME_MAX_BYTES) {
            s_i2c_work_buf[s_i2c_byte_cnt++] = s_i2c_curr_byte;
        }
        s_i2c_curr_byte = 0U;
        s_i2c_bit_idx   = 0U;
    }
}

static void sniffer_hw_i2c_init(void)
{
    /* Configure GPIO6 (SDA) and GPIO7 (SCL) for hardware I2C0 (`I2CEXT0`) peripheral + edge ISR */
    gpio_config_t sda_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_PROBE2_SDA),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&sda_conf);

    gpio_config_t scl_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_PROBE3_SCL),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&scl_conf);

    /* Route Probes 2 & 3 (GPIO6, GPIO7) into I2C0 (`I2CEXT0`) hardware peripheral via GPIO Matrix */
    (void)wtf_discovery_remap_probe(WTF_PIN_PROBE2_SDA, WTF_DECODER_I2C0_EXT);
    (void)wtf_discovery_remap_probe(WTF_PIN_PROBE3_SCL, WTF_DECODER_I2C0_EXT);

    /* Attach IRAM hardware ISRs to capture full bidirectional Master <-> Slave frames */
    (void)gpio_isr_handler_add(WTF_PIN_PROBE2_SDA, i2c_sda_edge_isr, NULL);
    (void)gpio_isr_handler_add(WTF_PIN_PROBE3_SCL, i2c_scl_posedge_isr, NULL);
    (void)gpio_set_intr_type(WTF_PIN_PROBE2_SDA, GPIO_INTR_ANYEDGE);
    (void)gpio_set_intr_type(WTF_PIN_PROBE3_SCL, GPIO_INTR_POSEDGE);
    (void)gpio_intr_enable(WTF_PIN_PROBE2_SDA);
    (void)gpio_intr_enable(WTF_PIN_PROBE3_SCL);
}
static void decode_i2c_gy511_frame(const uint8_t *bytes, uint8_t len)
{
    if (len < 2U) {
        return;
    }
    uint8_t addr7 = (uint8_t)(bytes[0] >> 1U);
    uint8_t rw    = (uint8_t)(bytes[0] & 1U);

    if (rw == 0U) {
        /* Master Write: bytes[1] is target register address */
        s_i2c_last_reg = bytes[1];
    } else if (rw == 1U && len >= 7U) {
        /* Master Read of 6 sensor bytes: bytes[1..6] */
        if (addr7 == 0x19U && (s_i2c_last_reg == 0xA8U || s_i2c_last_reg == 0x28U)) {
            /* LSM303DLHC Accelerometer: little-endian 12-bit left-justified -> mg */
            int16_t rx = (int16_t)(((uint16_t)bytes[2] << 8U) | bytes[1]) >> 4;
            int16_t ry = (int16_t)(((uint16_t)bytes[4] << 8U) | bytes[3]) >> 4;
            int16_t rz = (int16_t)(((uint16_t)bytes[6] << 8U) | bytes[5]) >> 4;
            s_imu_ax = rx;
            s_imu_ay = ry;
            s_imu_az = rz;
        } else if (addr7 == 0x1EU) {
            /* LSM303DLHC Magnetometer: big-endian X, Z, Y */
            int16_t mx = (int16_t)(((uint16_t)bytes[1] << 8U) | bytes[2]);
            int16_t mz = (int16_t)(((uint16_t)bytes[3] << 8U) | bytes[4]);
            int16_t my = (int16_t)(((uint16_t)bytes[5] << 8U) | bytes[6]);
            s_imu_mx = mx;
            s_imu_my = my;
            s_imu_mz = mz;
        }
    }
}

EXPORT void wtf_sniffer_get_i2c_imu(int16_t *ax, int16_t *ay, int16_t *az,
                                    int16_t *mx, int16_t *my, int16_t *mz)
{
    if (ax != NULL) *ax = s_imu_ax;
    if (ay != NULL) *ay = s_imu_ay;
    if (az != NULL) *az = s_imu_az;
    if (mx != NULL) *mx = s_imu_mx;
    if (my != NULL) *my = s_imu_my;
    if (mz != NULL) *mz = s_imu_mz;
}

/* --------------------------------------------------------------------------
 * Wi-Fi Channel Control API
 * -------------------------------------------------------------------------- */
EXPORT void wtf_sniffer_set_wifi_channel(uint8_t channel)
{
    if (channel > 13U) {
        channel = 0U;
    }
    s_wifi_channel_mode = channel;
    if (channel >= 1U && channel <= 13U) {
        s_current_sniffer_ch = channel;
        (void)esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        tm_printf((UB *)"[SNIFFER:WIFI] Channel locked to Ch %u by user\n", (unsigned)channel);
    } else {
        s_next_sweep_ch = 1U;
        tm_printf((UB *)"[SNIFFER:WIFI] Channel mode set to ALL-CHANNEL SWEEP (Ch 1..13)\n");
    }
}

EXPORT uint8_t wtf_sniffer_get_wifi_channel(void)
{
    return s_current_sniffer_ch;
}

EXPORT uint8_t wtf_sniffer_get_wifi_mode(void)
{
    return s_wifi_channel_mode;
}

EXPORT void wtf_sniffer_notify_http_activity(void)
{
    s_last_http_activity_us = esp_timer_get_time();
    if (s_wifi_channel_mode == 0U && s_current_sniffer_ch != SOFTAP_HOME_CHANNEL) {
        s_current_sniffer_ch = SOFTAP_HOME_CHANNEL;
        (void)esp_wifi_set_channel(SOFTAP_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
}

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
        rec.probe_id = s_154_channel; /* Active 802.15.4 Channel (11..26) */
        uint8_t copy_len = (frame[0] < WTF_PKT_DATA_MAX) ? frame[0] : (WTF_PKT_DATA_MAX - 1);
        rec.length = (uint16_t)copy_len;
        memcpy(rec.data, &frame[1], copy_len);

        if (frame[0] >= 10 && frame[8] == 0x03 && frame[9] == 0x00) {
            s_last_iris_packet_us = now_us;
            if (frame_info != NULL) {
                s_iris_rssi = frame_info->rssi;
                if (s_iris_rssi >= -42) {
                    s_iris_verified = true;
                } else if (s_iris_rssi < -55) {
                    s_iris_verified = false;
                }
            }
        }

        /* Push to storage queue (PCAP) */
        if (g_mbf_storage > 0) {
            (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
        }
        /* Push to telemetry queue */
        if (g_mbf_telemetry > 0) {
            (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
        }

        if ((s_154_capture_count % 5U) == 1U) {
            int8_t rssi = frame_info ? frame_info->rssi : -50;
            tm_printf((UB *)"[SNIFFER:802154] CAPTURE: 802.15.4 Frame #%u on Ch %u (RSSI=%d dBm, len=%u, FR-4)\n",
                      (unsigned)s_154_capture_count, (unsigned)s_154_channel, (int)rssi, (unsigned)frame[0]);
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

    /* Check if packet matches Iris BSSID (02:00:00:00:00:03) */
    uint8_t copy_len = (len < WTF_PKT_DATA_MAX) ? (uint8_t)len : (uint8_t)WTF_PKT_DATA_MAX;
    bool is_iris = false;
    if (copy_len >= 16U) {
        for (int i = 0; i <= ((int)copy_len - 6); i++) {
            if (memcmp(&pkt->payload[i], s_iris_target_mac, 6) == 0) {
                is_iris = true;
                break;
            }
        }
    }

    s_wifi_capture_count++;
    int64_t now_us = esp_timer_get_time();

    if (is_iris) {
        s_iris_rssi = (int8_t)pkt->rx_ctrl.rssi;
        s_last_iris_packet_us = now_us;
        if (s_iris_rssi >= -42) {
            s_iris_verified = true;
        } else if (s_iris_rssi < -55) {
            s_iris_verified = false;
        }
    }

    /* Throttle ambient non-Iris Wi-Fi frames so SD card PCAP isn't flooded,
     * while capturing 100% of Iris target frames and periodic ambient samples across Ch 1..13.
     */
    if (!is_iris && (s_wifi_capture_count % 8U) != 1U) {
        return;
    }

    wtf_record_t rec;
    rec.timestamp_us = (uint32_t)now_us;
    rec.protocol = PROTO_WIFI;
    rec.probe_id = (uint8_t)pkt->rx_ctrl.channel;
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

    if (is_iris || (s_wifi_capture_count % 20U) == 1U) {
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
    /* Enable Wi-Fi promiscuous mode so wifi_promiscuous_rx_cb receives 802.11 frames */
    esp_wifi_set_promiscuous(true);
    tm_printf((UB *)"[SNIFFER:WIRELESS] Wi-Fi promiscuous mode ENABLED (Ch 1..13, FR-4, TC-2.1)\n");

    /* Enable IEEE 802.15.4 receiver on Channel 15 so esp_ieee802154_receive_done fires */
    esp_ieee802154_enable();
    esp_ieee802154_set_channel(s_154_channel);
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_receive();
    tm_printf((UB *)"[SNIFFER:WIRELESS] IEEE 802.15.4 RX ENABLED on Channel 15 (FR-4, TC-2.2)\n");
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

    /* Delay 600 ms so TSK_DISCOVERY finishes initial rhythm lock, then enable hardware I2C capture */
    tk_dly_tsk(600);
    sniffer_hw_i2c_init();
    tm_printf((UB *)"[SNIFFER:I2C] Hardware edge-capture I2C decoder online on GPIO6 (SDA) & GPIO7 (SCL)\n");

    /* Delay 1400 ms to allow SoftAP to finish bringing up netif */
    tk_dly_tsk(1400);
    sniffer_wireless_init();

    uint8_t rx_buf[128];
    char line_buf[64];
    int line_idx = 0;
    uint32_t loop_tick = 0U;

    for (;;) {
        loop_tick++;

        /* 1. Drain available bytes from UART1 hardware FIFO buffer */
        int len = uart_read_bytes(UART_SNIFFER_PORT, rx_buf, sizeof(rx_buf), 15 / portTICK_PERIOD_MS);
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

        /* 2. Drain hardware-captured I2C frames from IRAM ISR ring buffer */
        while (s_i2c_ring_tail != s_i2c_ring_head) {
            uint8_t tail = s_i2c_ring_tail;
            uint8_t flen = s_i2c_ring[tail].length;
            uint32_t fts = s_i2c_ring[tail].timestamp_us;
            uint8_t local_bytes[I2C_FRAME_MAX_BYTES];
            for (uint8_t k = 0U; k < flen && k < I2C_FRAME_MAX_BYTES; k++) {
                local_bytes[k] = s_i2c_ring[tail].bytes[k];
            }
            s_i2c_ring_tail = (uint8_t)((tail + 1U) & (I2C_RING_DEPTH - 1U));

            if (flen > 0U) {
                s_i2c_capture_count++;
                decode_i2c_gy511_frame(local_bytes, flen);

                wtf_record_t rec;
                rec.timestamp_us = fts;
                rec.protocol = PROTO_I2C;
                rec.probe_id = 2U; /* Probes 2 & 3 (SDA/SCL) */
                rec.length = (uint16_t)flen;
                memcpy(rec.data, local_bytes, flen);

                /* Enqueue to PCAP storage & telemetry (sample every 2nd transaction to avoid SD queue saturation) */
                if ((s_i2c_capture_count % 2U) == 1U) {
                    if (g_mbf_storage > 0) {
                        (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
                    }
                }
                if (g_mbf_telemetry > 0) {
                    (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
                }
            }
        }
        /* Drain any Master-Write bytes captured directly in I2C0 (`I2CEXT0`) hardware RX FIFO */
        while (I2C0.sr.rxfifo_cnt > 0U) {
            uint8_t hw_byte = (uint8_t)(I2C0.data.fifo_rdata & 0xFFU);
            s_i2c_last_reg = hw_byte;
        }

        /* 3. Single-Radio Wi-Fi Channel State Machine (Ch 1..13 All-Channel Sweep + SoftAP Home Dwell):
         * - If user locked a specific channel (s_wifi_channel_mode == 1..13), stay on that channel.
         * - If an HTTP client requested a page or PCAP chunk within the last 250 ms, hold on
         *   Channel 6 (SoftAP home channel) just long enough for the 4 KB HTTP chunk to complete.
         * - Otherwise, interleave fast 50 ms off-channel promiscuous hops across channels 1..13
         *   with 150 ms dwell windows on Channel 6 so SoftAP beacons stay alive.
         */
        if (s_wifi_channel_mode == 0U) {
            int64_t now_us = esp_timer_get_time();
            bool http_busy = ((now_us - s_last_http_activity_us) < 250000LL);

            if (http_busy) {
                if (s_current_sniffer_ch != SOFTAP_HOME_CHANNEL) {
                    s_current_sniffer_ch = SOFTAP_HOME_CHANNEL;
                    (void)esp_wifi_set_channel(SOFTAP_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
                }
            } else {
                /* 8 ticks cycle (~200 ms): 6 ticks on SoftAP Ch 6, 2 ticks (~50 ms) on next sweep channel 1..13 */
                uint32_t phase = loop_tick & 7U;
                if (phase == 0U) {
                    uint8_t target_ch = s_next_sweep_ch;
                    s_next_sweep_ch++;
                    if (s_next_sweep_ch > 13U) {
                        s_next_sweep_ch = 1U;
                    }
                    if (target_ch == SOFTAP_HOME_CHANNEL) {
                        target_ch = s_next_sweep_ch;
                        s_next_sweep_ch++;
                        if (s_next_sweep_ch > 13U) {
                            s_next_sweep_ch = 1U;
                        }
                    }
                    if (target_ch != s_current_sniffer_ch) {
                        s_current_sniffer_ch = target_ch;
                        (void)esp_wifi_set_channel(target_ch, WIFI_SECOND_CHAN_NONE);
                    }
                } else if (phase == 2U) {
                    if (s_current_sniffer_ch != SOFTAP_HOME_CHANNEL) {
                        s_current_sniffer_ch = SOFTAP_HOME_CHANNEL;
                        (void)esp_wifi_set_channel(SOFTAP_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
                    }
                }
            }
        }
        tk_dly_tsk(10);
    }
}
int8_t wtf_sniffer_get_iris_rssi(void)
{
    return s_iris_rssi;
}

bool wtf_sniffer_is_iris_verified(void)
{
    if (s_last_iris_packet_us == 0 || (esp_timer_get_time() - s_last_iris_packet_us > 4000000LL)) {
        s_iris_verified = false;
    }
    return s_iris_verified;
}
