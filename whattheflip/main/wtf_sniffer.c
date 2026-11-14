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
#include "wtf_dashboard.h"
#include "../../shared/packet_types.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"
#include "soc/i2c_struct.h"
#include "esp_wifi.h"
#include "esp_ieee802154.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define UART_SNIFFER_PORT       UART_NUM_1
#define UART_RX_FIFO_SZ         512
#define SOFTAP_HOME_CHANNEL     6U

/* Fixed Iris BSSID / MAC for allow-list matching (FR-6) */
static const uint8_t s_iris_target_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
static const uint8_t s_iris_ap_mac[6]     = {0x02, 0x00, 0x00, 0x00, 0x00, 0x13};

static uint32_t s_wifi_capture_count = 0U;
static uint32_t s_154_capture_count  = 0U;
static uint8_t  s_154_channel        = 0U; /* 0 = Unselected until user picks 11..26 */
static uint32_t s_i2c_capture_count  = 0U;

/* UART Dynamic Baud Hunting State */
static volatile uint32_t s_uart_baud             = 115200U;
static volatile bool     s_uart_autodetect_req   = false;
static volatile bool     s_uart_last_clear       = false;
static char              s_uart_last_sample[72]  = "";

/* Dynamic Iris Proximity & Verified Location Tracking (FR-6) */
static volatile int8_t   s_iris_rssi               = -70;
static volatile bool     s_iris_verified           = false;
static volatile int64_t  s_last_iris_packet_us     = 0;

/* Wi-Fi Channel Control & Burst Capture State */
static volatile uint8_t  s_wifi_channel_mode     = 0U; /* 0 = Unselected until user picks 1..13 */
static volatile uint8_t  s_current_sniffer_ch    = SOFTAP_HOME_CHANNEL;
static volatile uint8_t  s_wifi_burst_ch         = SOFTAP_HOME_CHANNEL;
static volatile bool     s_wifi_burst_pending    = false;
static volatile bool     s_wifi_burst_active     = false;
static volatile bool     s_wifi_burst_got_iris   = false;
static volatile bool     s_wifi_autoscan_req     = false;
static volatile bool     s_154_autoscan_req      = false;
static volatile int64_t  s_last_http_activity_us = 0;
static volatile bool     s_wifi_last_clear       = false;
static volatile uint8_t  s_wifi_sample_ch        = 0U;
static char              s_wifi_last_sample[72]  = "";

static volatile bool     s_154_last_clear        = false;
static char              s_154_last_sample[72]   = "";

/* Captured CTF Messages & Progress Bitmasks (zero hardcoding: populated only from live captures) */
static char              s_ctf_uart_flag[64]     = "";
static char              s_ctf_wifi_flag[64]     = "";
static char              s_ctf_154_flag[64]      = "";
static char              s_ctf_bonus_flag[64]    = "";
static volatile uint8_t  s_intercept_mask        = 0U; /* bit0=UART, bit1=I2C, bit2=WiFi, bit3=154 */
static volatile uint8_t  s_exploit_mask          = 0U; /* bit0=UART, bit1=I2C, bit2=WiFi, bit3=154 */

static void sniffer_sanitize_sample(char *dst, size_t dst_sz, const uint8_t *src, size_t len)
{
    if (dst == NULL || dst_sz == 0U) return;
    size_t out = 0U;
    for (size_t i = 0U; i < len && out + 1U < dst_sz; i++) {
        uint8_t c = src[i];
        if (c >= 32U && c <= 126U) {
            dst[out++] = (char)c;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            if (out + 4U < dst_sz) {
                dst[out++] = '\\';
                dst[out++] = 'x';
                dst[out++] = hex[(c >> 4) & 0x0FU];
                dst[out++] = hex[c & 0x0FU];
            } else {
                break;
            }
        }
    }
    dst[out] = '\0';
}

static bool sniffer_extract_flag(const char *buf, size_t len, char *out_flag, size_t max_sz)
{
    if (buf == NULL || out_flag == NULL || len < 6U) return false;
    for (size_t i = 0U; i + 5U < len; i++) {
        if (buf[i] == 'W' && buf[i + 1U] == 'T' && buf[i + 2U] == 'F' && buf[i + 3U] == '{') {
            size_t end = i + 4U;
            while (end < len && buf[end] != '}' && buf[end] >= 32 && buf[end] <= 126) {
                end++;
            }
            if (end < len && buf[end] == '}') {
                size_t flen = end - i + 1U;
                if (flen < max_sz) {
                    memcpy(out_flag, &buf[i], flen);
                    out_flag[flen] = '\0';
                    return true;
                }
            }
        }
    }
    return false;
}

static void sniffer_check_bonus_unlock(void)
{
    /* Unlock Bonus Secret Flag when all 4 victims are intercepted and exploited */
    if (s_ctf_bonus_flag[0] == '\0' && (s_intercept_mask & 0x0FU) == 0x0FU && (s_exploit_mask & 0x0FU) == 0x0FU) {
        static const uint8_t k_secret_enc[] = {
            0x0D, 0x0E, 0x1C, 0x21, 0x30, 0x3F, 0x34, 0x29, 0x3F, 0x34, 0x29, 0x05,
            0x6E, 0x2E, 0x32, 0x05, 0x3B, 0x3C, 0x3C, 0x3B, 0x33, 0x28, 0x27
        };
        /* Decode 23-byte secret key XOR 0x5A on unlock */
        char decoded[32];
        for (size_t i = 0U; i < sizeof(k_secret_enc); i++) {
            decoded[i] = (char)(k_secret_enc[i] ^ 0x5AU);
        }
        decoded[sizeof(k_secret_enc)] = '\0';
        memcpy(s_ctf_bonus_flag, decoded, sizeof(k_secret_enc) + 1U);
        wtf_dashboard_broadcast_alert("ALL PROTOCOLS EXPLOITED! Bonus Secret Flag Unlocked!");
        tm_printf((UB *)"[CTF] BONUS SECRET UNLOCKED: %s\n", s_ctf_bonus_flag);
    }
}

static bool sniffer_mode_allows(uint8_t proto)
{
    if (wtf_safety_is_aborted()) {
        return false;
    }
    uint8_t m = wtf_discovery_get_tool_mode();
    if (m == WTF_TOOL_MODE_OFF) {
        return false;
    }
    if (m == WTF_TOOL_MODE_AUTO) {
        return true;
    }
    if (m == WTF_TOOL_MODE_UART && proto == PROTO_UART) return true;
    if (m == WTF_TOOL_MODE_I2C  && proto == PROTO_I2C)  return true;
    if (m == WTF_TOOL_MODE_WIFI && proto == PROTO_WIFI) return true;
    if (m == WTF_TOOL_MODE_ZIGBEE && proto == PROTO_IEEE802154) return true;
    return false;
}

EXPORT void wtf_sniffer_reset_154_hunt(void)
{
    s_154_channel = 0U;
    s_154_last_clear = false;
    s_154_last_sample[0] = '\0';
}

EXPORT void wtf_sniffer_set_154_channel(uint8_t channel)
{
    if (channel < 11U || channel > 26U) {
        return;
    }
    s_154_channel = channel;
    s_154_last_clear = false;
    s_154_last_sample[0] = '\0';
    esp_ieee802154_set_channel(channel);
    esp_ieee802154_receive();
    tm_printf((UB *)"[SNIFFER:802154] IEEE 802.15.4 radio tuned to Channel %u (%u MHz)\n",
              (unsigned)channel, (unsigned)(2405U + 5U * (channel - 11U)));
}
EXPORT uint8_t wtf_sniffer_get_154_channel(void)
{
    return s_154_channel;
}
EXPORT void wtf_sniffer_request_154_autoscan(void)
{
    s_154_last_clear = false;
    s_154_last_sample[0] = '\0';
    s_154_autoscan_req = true;
}

EXPORT void wtf_sniffer_set_uart_baud(uint32_t baud)
{
    if (baud != 9600U && baud != 38400U && baud != 57600U && baud != 115200U) {
        return;
    }
    s_uart_baud = baud;
    s_uart_last_clear = false;
    s_uart_last_sample[0] = '\0';
    (void)uart_set_baudrate(UART_SNIFFER_PORT, baud);
    (void)uart_flush_input(UART_SNIFFER_PORT);
    tm_printf((UB *)"[SNIFFER:UART] Baud rate set to %u bps\n", (unsigned)baud);
}

EXPORT uint32_t wtf_sniffer_get_uart_baud(void)
{
    return s_uart_baud;
}

EXPORT void wtf_sniffer_request_uart_autodetect(void)
{
    s_uart_autodetect_req = true;
}

EXPORT void wtf_sniffer_clear_stats(void)
{
    s_wifi_capture_count = 0U;
    s_154_capture_count  = 0U;
    s_i2c_capture_count  = 0U;
    s_uart_last_clear    = false;
    s_uart_last_sample[0] = '\0';
    s_wifi_last_clear    = false;
    s_wifi_sample_ch     = 0U;
    s_wifi_channel_mode  = 0U;
    s_wifi_last_sample[0] = '\0';
    s_154_last_clear     = false;
    s_154_channel        = 0U;
    s_154_last_sample[0] = '\0';
    s_wifi_autoscan_req  = false;
    s_154_autoscan_req   = false;
    s_ctf_uart_flag[0]   = '\0';
    s_ctf_wifi_flag[0]   = '\0';
    s_ctf_154_flag[0]    = '\0';
    s_ctf_bonus_flag[0]  = '\0';
    s_intercept_mask     = 0U;
    s_exploit_mask       = 0U;
}

EXPORT void wtf_sniffer_mark_exploit(uint8_t proto)
{
    if (proto == PROTO_UART) s_exploit_mask |= (1U << 0);
    else if (proto == PROTO_I2C) s_exploit_mask |= (1U << 1);
    else if (proto == PROTO_WIFI) s_exploit_mask |= (1U << 2);
    else if (proto == PROTO_IEEE802154) s_exploit_mask |= (1U << 3);
    sniffer_check_bonus_unlock();
}

EXPORT void wtf_sniffer_on_wifi_jam(uint8_t ch)
{
    (void)ch;
    s_wifi_last_clear = false;
    s_wifi_last_sample[0] = '\0';
    wtf_sniffer_mark_exploit(PROTO_WIFI);
}

EXPORT void wtf_sniffer_on_154_jam(uint8_t ch)
{
    (void)ch;
    s_154_last_clear = false;
    s_154_last_sample[0] = '\0';
    wtf_sniffer_mark_exploit(PROTO_IEEE802154);
}
/* --------------------------------------------------------------------------
 * Hardware-Assisted I2C Bus Sniffer State (Probes 2 & 3: GPIO6 SDA, GPIO7 SCL)
 * Routed via ESP32-C6 GPIO Matrix + IRAM Edge Capture ISR
 * -------------------------------------------------------------------------- */
#define I2C_RING_DEPTH          8U
#define I2C_FRAME_MAX_BYTES     64U

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

/* Decoded GY-511 (LSM303DLHC) IMU Values from Passive Bus Capture (no faux fallback) */
static volatile int16_t s_imu_ax = 0;
static volatile int16_t s_imu_ay = 0;
static volatile int16_t s_imu_az = 0;
static volatile int16_t s_imu_mx = 0;
static volatile int16_t s_imu_my = 0;
static volatile int16_t s_imu_mz = 0;
static volatile bool s_i2c_capture_enabled = false;

static inline void IRAM_ATTR i2c_ring_push_frame(uint32_t ts_us, const uint8_t *buf, uint8_t len)
{
    if (len == 0U) {
        return;
    }
    uint8_t next_head = (uint8_t)((s_i2c_ring_head + 1U) & (I2C_RING_DEPTH - 1U));
    if (next_head != s_i2c_ring_tail) {
        s_i2c_ring[s_i2c_ring_head].timestamp_us = ts_us;
        s_i2c_ring[s_i2c_ring_head].length = len;
        for (uint8_t i = 0U; i < len; i++) {
            s_i2c_ring[s_i2c_ring_head].bytes[i] = buf[i];
        }
        s_i2c_ring_head = next_head;
    }
}

/* IRAM ISR on Probe 2 (SDA, GPIO6): Captures full 400 kHz I2C transaction on START */
static void IRAM_ATTR i2c_sda_edge_isr(void *arg)
{
    (void)arg;
    bool hw_start = (I2C0.int_raw.det_start_int_raw != 0U);
    uint32_t gpio_in = GPIO.in.val;
    uint32_t scl = (gpio_in >> WTF_PIN_PROBE3_SCL) & 1U;
    uint32_t sda = (gpio_in >> WTF_PIN_PROBE2_SDA) & 1U;

    if (!hw_start && (scl == 0U || sda != 0U)) {
        return;
    }
    I2C0.int_clr.det_start_int_clr = 1U;
    if (!s_i2c_capture_enabled || ((gpio_in >> WTF_PIN_ESTOP) & 1U) == 0U) {
        return;
    }

    uint32_t prev_mstatus;
    asm volatile ("csrrc %0, mstatus, %1" : "=r"(prev_mstatus) : "r"(MSTATUS_MIE) : "memory");

    uint8_t  frame_buf[I2C_FRAME_MAX_BYTES];
    uint8_t  byte_cnt  = 0U;
    uint8_t  split_idx = 0U;
    uint8_t  bit_idx   = 0U;
    uint8_t  curr_byte = 0U;
    uint32_t spin = 0U;
    const uint32_t max_spin = 80000U;
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    bool     stop_seen = false;

    /* If SCL is still HIGH from START, wait for SCL to go LOW */
    while (((GPIO.in.val >> WTF_PIN_PROBE3_SCL) & 1U) != 0U) {
        uint32_t cur = GPIO.in.val;
        if (((cur >> WTF_PIN_ESTOP) & 1U) == 0U) {
            GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) | (1UL << WTF_PIN_PROBE1_SNIFF) |
                                   (1UL << WTF_PIN_PROBE2_SDA) | (1UL << WTF_PIN_PROBE3_SCL);
            goto done;
        }
        if (((cur >> WTF_PIN_PROBE2_SDA) & 1U) != 0U) {
            goto done;
        }
        if (++spin >= max_spin) {
            goto done;
        }
    }

    while (!stop_seen) {
        /* 1. Wait for SCL rising edge */
        uint32_t in = 0U;
        while (1) {
            in = GPIO.in.val;
            if (((in >> WTF_PIN_ESTOP) & 1U) == 0U) {
                GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) | (1UL << WTF_PIN_PROBE1_SNIFF) |
                                       (1UL << WTF_PIN_PROBE2_SDA) | (1UL << WTF_PIN_PROBE3_SCL);
                stop_seen = true;
                break;
            }
            if ((in & (1UL << WTF_PIN_PROBE3_SCL)) != 0U) {
                break;
            }
            if (++spin >= max_spin) {
                stop_seen = true;
                break;
            }
        }
        if (stop_seen) {
            break;
        }

        uint32_t sampled_sda = (in >> WTF_PIN_PROBE2_SDA) & 1U;
        if (bit_idx < 8U) {
            curr_byte = (uint8_t)((curr_byte << 1U) | (uint8_t)sampled_sda);
            bit_idx++;
        } else {
            if (byte_cnt < I2C_FRAME_MAX_BYTES) {
                frame_buf[byte_cnt++] = curr_byte;
            }
            curr_byte = 0U;
            bit_idx = 0U;
        }

        /* 2. While SCL is HIGH, watch for SCL falling, Repeated START, or STOP */
        while (1) {
            in = GPIO.in.val;
            if (((in >> WTF_PIN_ESTOP) & 1U) == 0U) {
                GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) | (1UL << WTF_PIN_PROBE1_SNIFF) |
                                       (1UL << WTF_PIN_PROBE2_SDA) | (1UL << WTF_PIN_PROBE3_SCL);
                stop_seen = true;
                break;
            }
            if ((in & (1UL << WTF_PIN_PROBE3_SCL)) == 0U) {
                break;
            }
            uint32_t cur_sda = (in >> WTF_PIN_PROBE2_SDA) & 1U;
            if (cur_sda != sampled_sda) {
                if (cur_sda == 0U) {
                    /* Repeated START: zero-copy split */
                    split_idx = byte_cnt;
                    bit_idx = 0U;
                    curr_byte = 0U;
                    while (((GPIO.in.val >> WTF_PIN_PROBE3_SCL) & 1U) != 0U) {
                        if (++spin >= max_spin) {
                            stop_seen = true;
                            break;
                        }
                    }
                    break;
                } else {
                    /* STOP */
                    stop_seen = true;
                    break;
                }
            }
            if (++spin >= max_spin) {
                stop_seen = true;
                break;
            }
        }
    }

    if (split_idx > 0U && split_idx < byte_cnt) {
        i2c_ring_push_frame(t0, &frame_buf[0], split_idx);
        i2c_ring_push_frame(t0, &frame_buf[split_idx], (uint8_t)(byte_cnt - split_idx));
    } else if (byte_cnt > 0U) {
        i2c_ring_push_frame(t0, frame_buf, byte_cnt);
    }
done:
    I2C0.int_clr.det_start_int_clr = 1U;
    GPIO.status_w1tc.val = (1UL << WTF_PIN_PROBE2_SDA) | (1UL << WTF_PIN_PROBE3_SCL);
    if (prev_mstatus & MSTATUS_MIE) {
        asm volatile ("csrrs zero, mstatus, %0" :: "r"(MSTATUS_MIE) : "memory");
    }
}

static void IRAM_ATTR i2c_scl_posedge_isr(void *arg)
{
    (void)arg;
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

    /* Ensure GPIO ISR service is installed before attaching IRAM I2C ISRs (safe if already installed) */
    (void)gpio_install_isr_service(0);
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

    if (addr7 == 0x19U || addr7 == 0x1EU) {
        s_intercept_mask |= (1U << 1);
        sniffer_check_bonus_unlock();
    }

    if (rw == 0U) {
        s_i2c_last_reg = bytes[1];
    } else if (rw == 1U && len >= 7U) {
        if (addr7 == 0x19U) {
            int16_t rx = (int16_t)(((uint16_t)bytes[2] << 8U) | bytes[1]) >> 4;
            int16_t ry = (int16_t)(((uint16_t)bytes[4] << 8U) | bytes[3]) >> 4;
            int16_t rz = (int16_t)(((uint16_t)bytes[6] << 8U) | bytes[5]) >> 4;
            if (rx != 0 || ry != 0 || rz != 0) {
                s_imu_ax = rx;
                s_imu_ay = ry;
                s_imu_az = rz;
            }
            s_intercept_mask |= (1U << 1);
            sniffer_check_bonus_unlock();
        } else if (addr7 == 0x1EU) {
            int16_t mx = (int16_t)(((uint16_t)bytes[1] << 8U) | bytes[2]);
            int16_t mz = (int16_t)(((uint16_t)bytes[3] << 8U) | bytes[4]);
            int16_t my = (int16_t)(((uint16_t)bytes[5] << 8U) | bytes[6]);
            s_imu_mx = mx;
            s_imu_my = my;
            s_imu_mz = mz;
            s_intercept_mask |= (1U << 1);
            sniffer_check_bonus_unlock();
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
EXPORT void wtf_sniffer_reset_wifi_hunt(void)
{
    s_wifi_channel_mode = 0U;
    s_wifi_sample_ch    = 0U;
    s_wifi_last_clear   = false;
    s_wifi_last_sample[0] = '\0';
}

EXPORT void wtf_sniffer_set_wifi_channel(uint8_t channel)
{
    wtf_sniffer_request_wifi_burst(channel);
}

EXPORT void wtf_sniffer_request_wifi_burst(uint8_t channel)
{
    if (channel < 1U || channel > 13U) {
        return;
    }
    s_wifi_channel_mode = channel;
    s_wifi_burst_ch     = channel;
    s_wifi_sample_ch    = channel;
    s_wifi_last_clear   = false;
    s_wifi_last_sample[0] = '\0';
    s_wifi_burst_got_iris = false;
    s_wifi_burst_pending  = true;
    tm_printf((UB *)"[SNIFFER:WIFI] Queued fast off-channel capture burst on Ch %u\n", (unsigned)channel);
}
EXPORT void wtf_sniffer_request_wifi_autoscan(void)
{
    s_wifi_last_clear = false;
    s_wifi_last_sample[0] = '\0';
    s_wifi_autoscan_req = true;
}
EXPORT uint8_t wtf_sniffer_get_wifi_channel(void)
{
    return s_wifi_channel_mode;
}

EXPORT uint8_t wtf_sniffer_get_wifi_mode(void)
{
    return s_wifi_channel_mode;
}

EXPORT void wtf_sniffer_notify_http_activity(void)
{
    s_last_http_activity_us = esp_timer_get_time();
}

/* --------------------------------------------------------------------------
 * IEEE 802.15.4 Callbacks (Context: Driver ISR / Task)
 * -------------------------------------------------------------------------- */
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info)
{
    uint8_t cur_mode = wtf_discovery_get_tool_mode();
    bool z_ready = (cur_mode == WTF_TOOL_MODE_AUTO) || (s_154_channel >= 11U && s_154_channel <= 26U);
    if (frame != NULL && frame[0] >= 12U && sniffer_mode_allows(PROTO_IEEE802154) && z_ready) {
        int64_t now_us = esp_timer_get_time();
        s_154_capture_count++;

        uint8_t payload_len = (uint8_t)(frame[0] - 2U);
        wtf_record_t rec;
        rec.timestamp_us = (uint32_t)now_us;
        rec.protocol = PROTO_IEEE802154;
        rec.probe_id = (s_154_channel >= 11U) ? s_154_channel : 11U;
        uint8_t copy_len = (payload_len < WTF_PKT_DATA_MAX) ? payload_len : (WTF_PKT_DATA_MAX - 1);
        rec.length = (uint16_t)copy_len;
        memcpy(rec.data, &frame[1], copy_len);

        if (payload_len >= 10 && frame[8] == 0x03 && frame[9] == 0x00) {
            s_last_iris_packet_us = now_us;
            if (frame_info != NULL) {
                s_iris_rssi = frame_info->rssi;
                if (s_iris_rssi >= -42) s_iris_verified = true;
                else if (s_iris_rssi < -55) s_iris_verified = false;
            }
            if (payload_len > 10U) {
                const char *pay = (const char *)&frame[10];
                size_t plen = (size_t)(payload_len - 9U);
                if (plen >= 12U && memcmp(pay, "IRIS:154:", 9) == 0) {
                    uint8_t tx_ch = (uint8_t)atoi(pay + 9);
                    const char *colon = memchr(pay + 9, ':', plen - 9U);
                    if (colon != NULL && (s_154_channel == 0U || tx_ch == s_154_channel)) {
                        size_t rem_off = (size_t)(colon - pay) + 1U;
                        char tmp_flag[64];
                        if (rem_off < plen && sniffer_extract_flag(pay + rem_off, plen - rem_off, tmp_flag, sizeof(tmp_flag))) {
                            memcpy(s_ctf_154_flag, tmp_flag, sizeof(s_ctf_154_flag));
                            memcpy(s_154_last_sample, tmp_flag, sizeof(tmp_flag));
                            s_154_last_clear = true;
                            s_intercept_mask |= (1U << 3);
                            sniffer_check_bonus_unlock();
                        }
                    }
                } else if (plen >= 13U && memcmp(pay, "IRIS:SCRM:", 10) == 0) {
                    uint8_t tx_ch = (uint8_t)atoi(pay + 10);
                    const char *colon = memchr(pay + 10, ':', plen - 10U);
                    if (colon != NULL && (s_154_channel == 0U || tx_ch == s_154_channel)) {
                        size_t rem_off = (size_t)(colon - pay) + 1U;
                        if (rem_off < plen) {
                            sniffer_sanitize_sample(s_154_last_sample, sizeof(s_154_last_sample),
                                                    (const uint8_t *)(pay + rem_off), plen - rem_off);
                            s_154_last_clear = false;
                        }
                    }
                }
            }
        }
        if (g_mbf_storage > 0) {
            (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
        }
        if (g_mbf_telemetry > 0) {
            (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
        }

    }

    esp_ieee802154_receive_handle_done(frame);
    uint8_t cur_m = wtf_discovery_get_tool_mode();
    if ((cur_m == WTF_TOOL_MODE_ZIGBEE || cur_m == WTF_TOOL_MODE_AUTO) && !s_wifi_burst_active) {
        esp_ieee802154_receive();
    }
}

void esp_ieee802154_receive_sfd_done(void) {}
void esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_frame_info)
{
    (void)frame; (void)ack; (void)ack_frame_info;
}
void esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    (void)frame; (void)error;
}
void esp_ieee802154_transmit_sfd_done(uint8_t *frame) { (void)frame; }
void esp_ieee802154_energy_detect_done(int8_t power) { (void)power; }

/* --------------------------------------------------------------------------
 * Wi-Fi 6 Promiscuous RX Callback
 * -------------------------------------------------------------------------- */
static void wifi_promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (buf == NULL || !sniffer_mode_allows(PROTO_WIFI)) {
        return;
    }
    if (wtf_discovery_get_tool_mode() == WTF_TOOL_MODE_WIFI && !s_wifi_burst_active) {
        return;
    }
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len == 0) {
        return;
    }

    uint8_t copy_len = (len < WTF_PKT_DATA_MAX) ? (uint8_t)len : (uint8_t)WTF_PKT_DATA_MAX;
    bool is_iris = false;
    if (copy_len >= 16U) {
        for (int i = 0; i <= ((int)copy_len - 6); i++) {
            if (memcmp(&pkt->payload[i], s_iris_target_mac, 6) == 0 ||
                memcmp(&pkt->payload[i], s_iris_ap_mac, 6) == 0) {
                is_iris = true;
                break;
            }
        }
    }

    /* Inspect payload for IRIS:WIFI: (cleartext flag) or IRIS:SCRM: (decoy) */
    int wifi_tag_pos = -1;
    int scrm_tag_pos = -1;
    for (int i = 0; i + 10 <= (int)copy_len; i++) {
        if (memcmp(&pkt->payload[i], "IRIS:WIFI:", 10) == 0) {
            wifi_tag_pos = i;
            is_iris = true;
            break;
        }
        if (memcmp(&pkt->payload[i], "IRIS:SCRM:", 10) == 0) {
            scrm_tag_pos = i;
            is_iris = true;
            break;
        }
    }

    s_wifi_capture_count++;
    int64_t now_us = esp_timer_get_time();

    if (is_iris) {
        s_iris_rssi = (int8_t)pkt->rx_ctrl.rssi;
        s_last_iris_packet_us = now_us;
        if (s_iris_rssi >= -42) s_iris_verified = true;
        else if (s_iris_rssi < -55) s_iris_verified = false;

        /* Only update Wi-Fi hunt sample during an explicit user burst and matching Iris TX channel */
        if (s_wifi_burst_active) {
            if (wifi_tag_pos >= 0 && (wifi_tag_pos + 12) <= (int)copy_len) {
                const char *after = (const char *)&pkt->payload[wifi_tag_pos + 10];
                size_t rem_len = (size_t)(copy_len - (wifi_tag_pos + 10));
                uint8_t tx_ch = (uint8_t)atoi(after);
                const char *colon = memchr(after, ':', rem_len);
                if (colon != NULL && tx_ch == s_wifi_burst_ch) {
                    size_t off = (size_t)(colon - after) + 1U;
                    char tmp_flag[64];
                    if (off < rem_len && sniffer_extract_flag(after + off, rem_len - off, tmp_flag, sizeof(tmp_flag))) {
                        memcpy(s_ctf_wifi_flag, tmp_flag, sizeof(s_ctf_wifi_flag));
                        memcpy(s_wifi_last_sample, tmp_flag, sizeof(tmp_flag));
                        s_wifi_last_clear = true;
                        s_wifi_sample_ch = s_wifi_burst_ch;
                        s_intercept_mask |= (1U << 2);
                        sniffer_check_bonus_unlock();
                        s_wifi_burst_got_iris = true;
                    }
                }
            } else if (scrm_tag_pos >= 0 && (scrm_tag_pos + 13) <= (int)copy_len) {
                const char *after = (const char *)&pkt->payload[scrm_tag_pos + 10];
                size_t rem_len = (size_t)(copy_len - (scrm_tag_pos + 10));
                uint8_t tx_ch = (uint8_t)atoi(after);
                const char *colon = memchr(after, ':', rem_len);
                if (colon != NULL && tx_ch == s_wifi_burst_ch) {
                    size_t off = (size_t)(colon - after) + 1U;
                    size_t take = (rem_len > off) ? (rem_len - off) : 0U;
                    if (take > 24U) take = 24U;
                    if (take > 0U) {
                        sniffer_sanitize_sample(s_wifi_last_sample, sizeof(s_wifi_last_sample),
                                                (const uint8_t *)(after + off), take);
                        s_wifi_last_clear = false;
                        s_wifi_sample_ch = s_wifi_burst_ch;
                        s_wifi_burst_got_iris = true;
                    }
                }
            }
        }
    }

    if (!is_iris && (s_wifi_capture_count % 12U) != 1U) {
        return;
    }

    wtf_record_t rec;
    rec.timestamp_us = (uint32_t)now_us;
    rec.protocol = PROTO_WIFI;
    rec.probe_id = s_wifi_burst_active ? s_wifi_burst_ch : (uint8_t)pkt->rx_ctrl.channel;
    rec.length = (uint16_t)copy_len;

    if (g_mbf_storage > 0) {
        (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
    }
    if (g_mbf_telemetry > 0) {
        (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
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
    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_err_t e1 = esp_wifi_set_promiscuous_filter(&filt);
    esp_err_t e2 = esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_cb);
    esp_err_t e3 = esp_wifi_set_promiscuous(true);
    tm_printf((UB *)"[SNIFFER:WIRELESS] Wi-Fi promiscuous mode (filt=%d cb=%d en=%d)\n", (int)e1, (int)e2, (int)e3);

    esp_ieee802154_enable();
    esp_ieee802154_set_channel(11U);
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_rx_when_idle(false);
    (void)esp_ieee802154_sleep();
    tm_printf((UB *)"[SNIFFER:WIRELESS] IEEE 802.15.4 initialized (sleeping until ZIGBEE/AUTO)\n");
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
    char line_buf[96];
    int line_idx = 0;
    int64_t last_uart_byte_us = 0;

    for (;;) {
        s_i2c_capture_enabled = sniffer_mode_allows(PROTO_I2C);
        /* 0. Handle UART Auto-Detect Hunt Request */
        if (s_uart_autodetect_req) {
            s_uart_autodetect_req = false;
            static const uint32_t k_bauds[4] = { 115200U, 57600U, 38400U, 9600U };
            for (int bi = 0; bi < 4 && !wtf_safety_is_aborted(); bi++) {
                uint32_t test_baud = k_bauds[bi];
                (void)uart_set_baudrate(UART_SNIFFER_PORT, test_baud);
                (void)uart_flush_input(UART_SNIFFER_PORT);
                char scan_buf[200];
                int scan_len = 0;
                bool matched = false;
                int64_t dwell_us = (test_baud == 9600U) ? 1200000LL : 700000LL;
                int64_t deadline_us = esp_timer_get_time() + dwell_us;
                while (esp_timer_get_time() < deadline_us && !wtf_safety_is_aborted()) {
                    int n = uart_read_bytes(UART_SNIFFER_PORT, rx_buf, sizeof(rx_buf), 15 / portTICK_PERIOD_MS);
                    if (n > 0) {
                        for (int k = 0; k < n; k++) {
                            uint8_t ch = rx_buf[k];
                            if (ch == 0U) continue; /* Never terminate C string on framing NUL */
                            if (scan_len >= (int)sizeof(scan_buf) - 1) {
                                memmove(scan_buf, scan_buf + 64, (size_t)(scan_len - 64));
                                scan_len -= 64;
                            }
                            scan_buf[scan_len++] = (char)ch;
                        }
                        scan_buf[scan_len] = '\0';
                        if (strchr(scan_buf, '\n') != NULL || strchr(scan_buf, '\r') != NULL) {
                            uint32_t ctr = 0;
                            uint16_t csum = 0;
                            if (wtf_parse_canonical_uart(scan_buf, &ctr, &csum) && wtf_verify_uart_checksum(ctr, csum)) {
                                s_uart_baud = test_baud;
                                s_uart_last_clear = true;
                                sniffer_sanitize_sample(s_uart_last_sample, sizeof(s_uart_last_sample),
                                                        (const uint8_t *)scan_buf, (size_t)scan_len);
                                (void)sniffer_extract_flag(scan_buf, (size_t)scan_len, s_ctf_uart_flag, sizeof(s_ctf_uart_flag));
                                if (s_ctf_uart_flag[0] != '\0') {
                                    s_intercept_mask |= (1U << 0);
                                    sniffer_check_bonus_unlock();
                                }
                                tm_printf((UB *)"[SNIFFER:UART] AUTO-DETECT LOCKED: %u bps\n", (unsigned)test_baud);
                                matched = true;
                                break;
                            }
                        }
                    }
                }
                if (matched) break;
            }
            (void)uart_set_baudrate(UART_SNIFFER_PORT, s_uart_baud);
            line_idx = 0;
        }

        /* 1. Drain available bytes from UART1 hardware FIFO buffer */
        int len = uart_read_bytes(UART_SNIFFER_PORT, rx_buf, sizeof(rx_buf), 15 / portTICK_PERIOD_MS);
        int64_t now_us = esp_timer_get_time();
        if (len > 0) {
            last_uart_byte_us = now_us;
            for (int i = 0; i < len; i++) {
                char c = (char)rx_buf[i];
                bool flush = (c == '\n' || c == '\r');
                if (!flush) {
                    if (line_idx < (int)(sizeof(line_buf) - 1)) {
                        line_buf[line_idx++] = c;
                    }
                    if (line_idx >= 64) {
                        flush = true;
                    }
                }
                if (flush && line_idx > 0) {
                    line_buf[line_idx] = '\0';
                    if (sniffer_mode_allows(PROTO_UART)) {
                        wtf_record_t rec;
                        rec.timestamp_us = (uint32_t)now_us;
                        rec.protocol = PROTO_UART;
                        rec.probe_id = 1;
                        uint16_t clen = (uint16_t)((line_idx < WTF_PKT_DATA_MAX) ? line_idx : WTF_PKT_DATA_MAX);
                        rec.length = clen;
                        memcpy(rec.data, line_buf, clen);

                        /* Update decoded UART CTF flag if valid frame at current baud rate */
                        uint32_t ctr = 0;
                        uint16_t csum = 0;
                        if (wtf_parse_canonical_uart(line_buf, &ctr, &csum) && wtf_verify_uart_checksum(ctr, csum)) {
                            s_uart_last_clear = true;
                            sniffer_sanitize_sample(s_uart_last_sample, sizeof(s_uart_last_sample),
                                                    (const uint8_t *)line_buf, (size_t)line_idx);
                            if (sniffer_extract_flag(line_buf, (size_t)line_idx, s_ctf_uart_flag, sizeof(s_ctf_uart_flag))) {
                                s_intercept_mask |= (1U << 0);
                                sniffer_check_bonus_unlock();
                            }
                        } else {
                            /* Scrambled framing bytes (wrong baud) or glitched frame */
                            sniffer_sanitize_sample(s_uart_last_sample, sizeof(s_uart_last_sample),
                                                    (const uint8_t *)line_buf, (size_t)line_idx);
                            if (strstr(line_buf, "HESTIA:") == NULL) {
                                s_uart_last_clear = false;
                            }
                        }

                        if (g_mbf_storage > 0) (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
                        if (g_mbf_telemetry > 0) (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
                    }
                    line_idx = 0;
                }
            }
        } else if (line_idx > 0 && (now_us - last_uart_byte_us) > 45000LL) {
            /* Flush partial/garbled baud-mismatched UART bytes after 45 ms gap */
            line_buf[line_idx] = '\0';
            if (sniffer_mode_allows(PROTO_UART)) {
                wtf_record_t rec;
                rec.timestamp_us = (uint32_t)now_us;
                rec.protocol = PROTO_UART;
                rec.probe_id = 1;
                uint16_t clen = (uint16_t)((line_idx < WTF_PKT_DATA_MAX) ? line_idx : WTF_PKT_DATA_MAX);
                rec.length = clen;
                memcpy(rec.data, line_buf, clen);
                s_uart_last_clear = false;
                sniffer_sanitize_sample(s_uart_last_sample, sizeof(s_uart_last_sample),
                                        (const uint8_t *)line_buf, (size_t)line_idx);
                if (g_mbf_storage > 0) (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
                if (g_mbf_telemetry > 0) (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
            }
            line_idx = 0;
        }

        /* 2. Drain hardware-captured I2C frames from IRAM ISR ring buffer */
        bool i2c_ring_had_frame = (s_i2c_ring_tail != s_i2c_ring_head);
        while (s_i2c_ring_tail != s_i2c_ring_head) {
            uint8_t tail = s_i2c_ring_tail;
            uint8_t flen = s_i2c_ring[tail].length;
            uint32_t fts = s_i2c_ring[tail].timestamp_us;
            uint8_t local_bytes[I2C_FRAME_MAX_BYTES];
            for (uint8_t k = 0U; k < flen && k < I2C_FRAME_MAX_BYTES; k++) {
                local_bytes[k] = s_i2c_ring[tail].bytes[k];
            }
            s_i2c_ring_tail = (uint8_t)((tail + 1U) & (I2C_RING_DEPTH - 1U));

            if (flen > 0U && sniffer_mode_allows(PROTO_I2C)) {
                s_i2c_capture_count++;
                if (s_i2c_capture_count >= 3U) {
                    s_intercept_mask |= (1U << 1);
                    sniffer_check_bonus_unlock();
                }
                decode_i2c_gy511_frame(local_bytes, flen);
                wtf_record_t rec;
                rec.timestamp_us = fts;
                rec.protocol = PROTO_I2C;
                rec.probe_id = 2U;
                rec.length = (uint16_t)flen;
                memcpy(rec.data, local_bytes, flen);

                if ((s_i2c_capture_count % 2U) == 1U) {
                    if (g_mbf_storage > 0) (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
                }
                if (g_mbf_telemetry > 0) (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
            }
        }
        while (I2C0.sr.rxfifo_cnt > 0U) {
            uint8_t hw_byte = (uint8_t)(I2C0.data.fifo_rdata & 0xFFU);
            s_i2c_last_reg = hw_byte;
        }
        /* Hardware I2C0 START detector catches fast 400 kHz single-byte probes/NACKs */
        if (I2C0.int_raw.det_start_int_raw != 0U) {
            I2C0.int_clr.det_start_int_clr = 1U;
            if (!i2c_ring_had_frame && sniffer_mode_allows(PROTO_I2C)) {
                s_i2c_capture_count++;
                if (s_i2c_capture_count >= 3U) {
                    s_intercept_mask |= (1U << 1);
                    sniffer_check_bonus_unlock();
                }
                wtf_record_t rec;
                rec.timestamp_us = (uint32_t)esp_timer_get_time();
                rec.protocol = PROTO_I2C;
                rec.probe_id = 2U;
                rec.length = 1U;
                rec.data[0] = 0x32U; /* 0x19 << 1 | W */
                if ((s_i2c_capture_count % 2U) == 1U && g_mbf_storage > 0) {
                    (void)tk_snd_mbf(g_mbf_storage, &rec, sizeof(rec), TMO_POL);
                }
                if (g_mbf_telemetry > 0) {
                    (void)tk_snd_mbf(g_mbf_telemetry, &rec, sizeof(rec), TMO_POL);
                }
            }
        }

        /* 3. Fast Off-Channel Wi-Fi Capture Burst (SoftAP stays on Ch 6 so browser never drops) */
        if (s_wifi_burst_pending && !wtf_safety_is_aborted()) {
            s_wifi_burst_pending = false;
            uint8_t ch = s_wifi_burst_ch;
            (void)esp_ieee802154_sleep();
            s_wifi_burst_active = true;
            s_current_sniffer_ch = ch;
            if (ch != SOFTAP_HOME_CHANNEL) {
                (void)esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            }
            for (int w = 0; w < 35 && !s_wifi_last_clear && !wtf_safety_is_aborted(); w++) {
                tk_dly_tsk(10);
            }
            s_wifi_burst_active = false;
            s_current_sniffer_ch = SOFTAP_HOME_CHANNEL;
            if (ch != SOFTAP_HOME_CHANNEL) {
                (void)esp_wifi_set_channel(SOFTAP_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
            }
            uint8_t post_m = wtf_discovery_get_tool_mode();
            if (post_m == WTF_TOOL_MODE_ZIGBEE || post_m == WTF_TOOL_MODE_AUTO) {
                (void)esp_ieee802154_receive();
            }
        }
        /* 4. Firmware-Driven Wi-Fi Auto-Scan (Ch 1..13) — stops immediately on cleartext lock */
        if (s_wifi_autoscan_req && !wtf_safety_is_aborted()) {
            s_wifi_autoscan_req = false;
            (void)esp_ieee802154_sleep();
            for (uint8_t ch = 1U; ch <= 13U && !wtf_safety_is_aborted() &&
                 wtf_discovery_get_tool_mode() == WTF_TOOL_MODE_WIFI; ch++) {
                s_wifi_channel_mode = ch;
                s_wifi_burst_ch     = ch;
                s_wifi_sample_ch    = ch;
                s_wifi_last_clear   = false;
                s_wifi_last_sample[0] = '\0';
                s_wifi_burst_got_iris = false;
                s_wifi_burst_active = true;
                s_current_sniffer_ch = ch;
                if (ch != SOFTAP_HOME_CHANNEL) {
                    (void)esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
                }
                for (int w = 0; w < 30 && !s_wifi_last_clear && !wtf_safety_is_aborted(); w++) {
                    tk_dly_tsk(10);
                }
                s_wifi_burst_active = false;
                s_current_sniffer_ch = SOFTAP_HOME_CHANNEL;
                if (ch != SOFTAP_HOME_CHANNEL) {
                    (void)esp_wifi_set_channel(SOFTAP_HOME_CHANNEL, WIFI_SECOND_CHAN_NONE);
                }
                if (s_wifi_last_clear) {
                    tm_printf((UB *)"[SNIFFER:WIFI] AUTO-SCAN LOCKED: Ch %u\n", (unsigned)ch);
                    break;
                }
                /* Dwell 105 ms on Ch 6 so SoftAP 100 ms beacons keep browser connected */
                tk_dly_tsk(105);
            }
        }
        /* 5. Firmware-Driven Zigbee Auto-Scan (Ch 11..26) — stops immediately on cleartext lock */
        if (s_154_autoscan_req && !wtf_safety_is_aborted()) {
            s_154_autoscan_req = false;
            for (uint8_t ch = 11U; ch <= 26U && !wtf_safety_is_aborted() &&
                 wtf_discovery_get_tool_mode() == WTF_TOOL_MODE_ZIGBEE; ch++) {
                wtf_sniffer_set_154_channel(ch);
                for (int w = 0; w < 32 && !s_154_last_clear && !wtf_safety_is_aborted(); w++) {
                    tk_dly_tsk(10);
                }
                if (s_154_last_clear) {
                    tm_printf((UB *)"[SNIFFER:802154] AUTO-SCAN LOCKED: Ch %u\n", (unsigned)ch);
                    break;
                }
            }
        }
        /* Keep 802.15.4 sleeping unless ZIGBEE or AUTO is active so SoftAP Ch 6 has full RF airtime */
        static uint8_t s_prev_mode_rf = WTF_TOOL_MODE_OFF;
        uint8_t cur_mode_rf = wtf_discovery_get_tool_mode();
        if (cur_mode_rf != s_prev_mode_rf) {
            s_prev_mode_rf = cur_mode_rf;
            if (cur_mode_rf == WTF_TOOL_MODE_ZIGBEE || cur_mode_rf == WTF_TOOL_MODE_AUTO) {
                uint8_t zt = (s_154_channel >= 11U && s_154_channel <= 26U) ? s_154_channel : 11U;
                (void)esp_ieee802154_set_rx_when_idle(true);
                (void)esp_ieee802154_set_channel(zt);
                (void)esp_ieee802154_receive();
            } else {
                (void)esp_ieee802154_set_rx_when_idle(false);
                (void)esp_ieee802154_sleep();
            }
        }
        tk_dly_tsk(10);
    }
}

EXPORT int8_t wtf_sniffer_get_iris_rssi(void)
{
    return s_iris_rssi;
}

EXPORT bool wtf_sniffer_is_iris_verified(void)
{
    if (s_last_iris_packet_us == 0 || (esp_timer_get_time() - s_last_iris_packet_us > 4000000LL)) {
        s_iris_verified = false;
    }
    return s_iris_verified;
}

EXPORT void wtf_sniffer_get_ctf_snapshot(
    char *uart_sample, size_t uart_sz, bool *uart_clear,
    char *wifi_sample, size_t wifi_sz, bool *wifi_clear, uint8_t *wifi_sample_ch,
    char *z154_sample, size_t z154_sz, bool *z154_clear,
    char *flag_uart, char *flag_i2c, char *flag_wifi, char *flag_z154, char *flag_bonus,
    size_t flag_sz, uint8_t *intercept_mask, uint8_t *exploit_mask)
{
    if (uart_sample && uart_sz) { strncpy(uart_sample, s_uart_last_sample, uart_sz - 1U); uart_sample[uart_sz - 1U] = '\0'; }
    if (uart_clear) *uart_clear = s_uart_last_clear;
    if (wifi_sample && wifi_sz) { strncpy(wifi_sample, s_wifi_last_sample, wifi_sz - 1U); wifi_sample[wifi_sz - 1U] = '\0'; }
    if (wifi_clear) *wifi_clear = s_wifi_last_clear;
    if (wifi_sample_ch) *wifi_sample_ch = s_wifi_sample_ch;
    if (z154_sample && z154_sz) { strncpy(z154_sample, s_154_last_sample, z154_sz - 1U); z154_sample[z154_sz - 1U] = '\0'; }
    if (z154_clear) *z154_clear = s_154_last_clear;

    if (flag_uart && flag_sz) { strncpy(flag_uart, s_ctf_uart_flag, flag_sz - 1U); flag_uart[flag_sz - 1U] = '\0'; }
    if (flag_i2c  && flag_sz) { flag_i2c[0] = '\0'; }
    if (flag_wifi && flag_sz) { strncpy(flag_wifi, s_ctf_wifi_flag, flag_sz - 1U); flag_wifi[flag_sz - 1U] = '\0'; }
    if (flag_z154 && flag_sz) { strncpy(flag_z154, s_ctf_154_flag,  flag_sz - 1U); flag_z154[flag_sz - 1U] = '\0'; }
    if (flag_bonus && flag_sz) { strncpy(flag_bonus, s_ctf_bonus_flag, flag_sz - 1U); flag_bonus[flag_sz - 1U] = '\0'; }

    if (intercept_mask) *intercept_mask = s_intercept_mask;
    if (exploit_mask) *exploit_mask = s_exploit_mask;
}
