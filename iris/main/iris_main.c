/*
 * iris_main.c - Node 3 Wireless Victim Firmware
 *
 * Runs µT-Kernel 3.0 on bare ESP32-C6 (RV32IMAC @ 160 MHz).
 * Implements:
 *   - SoftAP beacon broadcast on Channel 6 with BSSID 02:00:00:00:00:03 (target for FR-6).
 *   - Periodic IEEE 802.15.4 frame transmissions on Channel 15 (target for FR-4 / TC-2.2).
 * Enforces Barr C standard: zero malloc/free, static task stacks.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "riscv/encoding.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_ieee802154.h"
#include "esp_random.h"
#include "driver/gpio.h"

#define IRIS_WIFI_SSID          "iris-victim"
#define IRIS_PAN_ID             0x1234
#define IRIS_SHORT_ADDR         0x0003
#define IRIS_STATUS_GPIO        GPIO_NUM_8

#define IRIS_WIFI_FLAG          "WTF{tell_apollo_to_get_out}"
#define IRIS_WIFI_EVADE_FLAG    "WTF{nonchalant_and_6_ft}"
#define IRIS_154_FLAG           "WTF{i_carry_your_messages_not_your_sins}"
#define IRIS_154_EVADE_FLAG     "WTF{wheresmywater}"

/* Dynamic Cat-and-Mouse RF State */
static volatile uint32_t s_tx_period_ms    = 30;
static volatile uint8_t  s_true_wifi_ch    = 6;
static volatile uint8_t  s_true_154_ch     = 15;
static volatile uint32_t s_ota_cmd_count   = 0;
static volatile bool     s_wifi_jammed     = false;
static volatile bool     s_154_jammed      = false;
static volatile bool     s_wifi_evaded     = false;
static volatile bool     s_154_evaded      = false;
static volatile bool     s_espnow_done     = true;
static volatile uint32_t s_espnow_cb_cnt   = 0;
static volatile bool     s_154_tx_done     = true;

static void iris_espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    (void)status;
    s_espnow_cb_cnt++;
    s_espnow_done = true;
}

static void iris_update_beacon_ie(void)
{
    uint8_t vnd_buf[72];
    memset(vnd_buf, 0, sizeof(vnd_buf));
    vnd_buf[0] = 0xDD;
    vnd_buf[2] = 0x02; vnd_buf[3] = 0x00; vnd_buf[4] = 0x03;
    vnd_buf[5] = s_true_wifi_ch;
    const char *wflag = s_wifi_evaded ? IRIS_WIFI_EVADE_FLAG : IRIS_WIFI_FLAG;
    int n = snprintf((char *)&vnd_buf[6], sizeof(vnd_buf) - 6U, "IRIS:WIFI:%u:%s", (unsigned)s_true_wifi_ch, wflag);
    size_t plen = (n > 0) ? (size_t)n : 0U;
    vnd_buf[1] = (uint8_t)(4U + plen);
    (void)esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, NULL);
    (void)esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, vnd_buf);
}

/* Static Task Stacks (Zero malloc invariant) */
#define IDLE_STACK_SZ           1024
#define TX_STACK_SZ             6144
static UB s_idle_stack[IDLE_STACK_SZ];
static UB s_tx_stack[TX_STACK_SZ];

/* Fixed Locally Administered MAC for Iris (FR-6 target) */
static const uint8_t s_iris_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
/* Lowest priority idle task */
static void task_idle(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    for (;;) {
        asm volatile ("wfi" ::: "memory");
    }
}

/* Wi-Fi Promiscuous RX Callback: Detect targeted Wi-Fi Jam from WhatTheFlip */
static void iris_wifi_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (buf == NULL) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 36U || len > 256U) {
        return;
    }
    const uint8_t *p = pkt->payload;
    /* Check if addressed to Iris MAC (Addr1 at offset 4) and payload has WTF:JAM:WIFI */
    if (memcmp(&p[4], s_iris_mac, 6) == 0) {
        for (uint16_t i = 24U; i + 12U <= len; i++) {
            if (memcmp(&p[i], "WTF:JAM:WIFI", 12) == 0) {
                s_wifi_jammed = true;
                break;
            }
        }
    }
}

/* IEEE 802.15.4 Callbacks: Over-The-Air Control & Jam Handler */
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info)
{
    (void)frame_info;
    if (frame != NULL && frame[0] >= 14) {
        /* Check if addressed to Iris Short Addr 0x0003 and PAN 0x1234 */
        if (frame[4] == 0x34 && frame[5] == 0x12 && frame[6] == 0x03 && frame[7] == 0x00) {
            s_ota_cmd_count++;
            uint8_t len = frame[0];
            if (len >= 20 && memcmp(&frame[10], "WTF:CMD:RATE:20HZ", 17) == 0) {
                s_tx_period_ms = 80;
                tm_printf((UB *)"[IRIS:OTA] Command: High-density 20 Hz Burst\n");
                gpio_set_level(IRIS_STATUS_GPIO, 1);
            } else if (len >= 19 && memcmp(&frame[10], "WTF:CMD:RATE:2HZ", 16) == 0) {
                s_tx_period_ms = 180;
                tm_printf((UB *)"[IRIS:OTA] Command: Nominal Burst\n");
                gpio_set_level(IRIS_STATUS_GPIO, 0);
            } else if (len >= 20 && memcmp(&frame[10], "WTF:JAM:TARGET:IRIS", 19) == 0) {
                s_154_jammed = true;
                gpio_set_level(IRIS_STATUS_GPIO, 1);
            }
        }
    }
    esp_ieee802154_receive_handle_done(frame);
    (void)esp_ieee802154_receive();
}
void esp_ieee802154_receive_sfd_done(void)
{
}

void esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_frame_info)
{
    (void)frame;
    (void)ack;
    (void)ack_frame_info;
    s_154_tx_done = true;
}

void esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    (void)frame;
    (void)error;
    s_154_tx_done = true;
}

void esp_ieee802154_transmit_sfd_done(uint8_t *frame)
{
    (void)frame;
}

void esp_ieee802154_energy_detect_done(int8_t power)
{
    (void)power;
}

static void iris_init_wifi_softap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_netif_create_default_wifi_sta();

    /* Use distinct local MAC for AP vs STA so ESP-NOW doesn't collide */
    static const uint8_t s_iris_sta_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
    static const uint8_t s_iris_ap_mac[6]  = {0x02, 0x00, 0x00, 0x00, 0x00, 0x13};
    esp_wifi_set_mac(WIFI_IF_AP, s_iris_ap_mac);
    esp_wifi_set_mac(WIFI_IF_STA, s_iris_sta_mac);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = IRIS_WIFI_SSID,
            .ssid_len = strlen(IRIS_WIFI_SSID),
            .channel = s_true_wifi_ch,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_promiscuous_rx_cb(iris_wifi_rx_cb);
    esp_wifi_set_promiscuous(true);

    esp_err_t en_err = esp_now_init();
    esp_err_t peer_err = ESP_OK;
        (void)esp_now_register_send_cb(iris_espnow_send_cb);
    if (en_err == ESP_OK) {
        esp_now_peer_info_t bcast = {0};
        memset(bcast.peer_addr, 0xFF, 6);
        bcast.channel = 0;
        bcast.ifidx = WIFI_IF_STA;
        bcast.encrypt = false;
        peer_err = esp_now_add_peer(&bcast);
    }
    iris_update_beacon_ie();

    tm_printf((UB *)"[IRIS:BEACON] SoftAP+ESPNOW(init=%d peer=%d) heap=%lu TrueCh=%u\n",
              (int)en_err, (int)peer_err, (unsigned long)esp_get_free_heap_size(), (unsigned)s_true_wifi_ch);
}

static void iris_init_ieee802154(void)
{
    esp_ieee802154_enable();
    esp_ieee802154_set_promiscuous(false);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_set_channel(s_true_154_ch);
    esp_ieee802154_set_panid(IRIS_PAN_ID);
    esp_ieee802154_set_short_address(IRIS_SHORT_ADDR);
    esp_ieee802154_set_txpower(20);
    (void)esp_ieee802154_receive();

    tm_printf((UB *)"[IRIS:802154] Subsystem active: TrueCh=%u PAN=0x%04X ADDR=0x%04X\n",
              (unsigned)s_true_154_ch, IRIS_PAN_ID, IRIS_SHORT_ADDR);
}

static uint8_t iris_pick_new_channel(uint8_t min_ch, uint8_t max_ch, uint8_t avoid_ch)
{
    uint8_t span = (uint8_t)(max_ch - min_ch + 1U);
    uint8_t ch = (uint8_t)(min_ch + (esp_random() % span));
    if (ch == avoid_ch && span > 1U) {
        ch = (uint8_t)(min_ch + ((ch - min_ch + 1U + (esp_random() % (span - 1U))) % span));
    }
    return ch;
}

static void iris_fill_scrambled(uint8_t *dst, size_t len, uint8_t ch, uint32_t seq)
{
    static const char k_scram[] = "~!@#$%^&*?/|\\+=<>[]{}";
    uint32_t seed = (esp_random() ^ (seq * 2654435761UL) ^ ((uint32_t)ch << 8));
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1664525UL + 1013904223UL;
        if ((i & 3U) == 0U) {
            dst[i] = (uint8_t)k_scram[(seed >> 16) % (sizeof(k_scram) - 1U)];
        } else {
            dst[i] = (uint8_t)(0x80U | ((seed >> 20) & 0x7FU));
        }
    }
}

static esp_err_t iris_send_wifi_frame(uint8_t ch, bool is_true_ch, uint32_t seq)
{
    uint8_t payload[72];
    size_t pay_len = 0;

    if (is_true_ch) {
        const char *wflag = s_wifi_evaded ? IRIS_WIFI_EVADE_FLAG : IRIS_WIFI_FLAG;
        int n = snprintf((char *)payload, sizeof(payload), "IRIS:WIFI:%u:%s", (unsigned)ch, wflag);
        pay_len = (n > 0) ? (size_t)n : 0U;
    } else {
        int n = snprintf((char *)payload, sizeof(payload), "IRIS:SCRM:%u:", (unsigned)ch);
        size_t hlen = (n > 0) ? (size_t)n : 0U;
        iris_fill_scrambled(payload + hlen, 32, ch, seq);
        pay_len = hlen + 32;
    }

    (void)esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    s_espnow_done = false;
    static const uint8_t k_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_err_t tx_err = esp_now_send(k_bcast, payload, pay_len);
    if (tx_err == ESP_OK) {
        for (int w = 0; w < 15 && !s_espnow_done; w++) {
            tk_dly_tsk(2);
        }
    }
    return tx_err;
}
static esp_err_t iris_send_154_frame(uint8_t ch, bool is_true_ch, uint8_t seq)
{
    uint8_t frame_buf[96];
    char payload[72];
    size_t plen = 0;

    if (is_true_ch) {
        const char *zflag = s_154_evaded ? IRIS_154_EVADE_FLAG : IRIS_154_FLAG;
        int n = snprintf(payload, sizeof(payload), "IRIS:154:%u:%s", (unsigned)ch, zflag);
        plen = (n > 0) ? (size_t)n : 0U;
    } else {
        int n = snprintf(payload, sizeof(payload), "IRIS:SCRM:%u:", (unsigned)ch);
        size_t hlen = (n > 0) ? (size_t)n : 0U;
        iris_fill_scrambled((uint8_t *)(payload + hlen), 32, ch, seq);
        plen = hlen + 32;
    }

    uint8_t psdu_len = (uint8_t)(9U + plen + 2U); /* 9B MHR + payload + 2B HW FCS */
    frame_buf[0] = psdu_len;
    frame_buf[1] = 0x41; /* FCF LSB: Data frame, PAN ID comp */
    frame_buf[2] = 0x88; /* FCF MSB: Short dest, short src */
    frame_buf[3] = seq;
    frame_buf[4] = (uint8_t)(IRIS_PAN_ID & 0xFF);
    frame_buf[5] = (uint8_t)((IRIS_PAN_ID >> 8) & 0xFF);
    frame_buf[6] = 0xFF; /* Dst broadcast */
    frame_buf[7] = 0xFF;
    frame_buf[8] = (uint8_t)(IRIS_SHORT_ADDR & 0xFF);
    frame_buf[9] = (uint8_t)((IRIS_SHORT_ADDR >> 8) & 0xFF);
    memcpy(&frame_buf[10], payload, plen);

    (void)esp_ieee802154_set_channel(ch);
    s_154_tx_done = false;
    esp_err_t err = esp_ieee802154_transmit(frame_buf, false);
    if (err == ESP_OK) {
        for (int w = 0; w < 3 && !s_154_tx_done; w++) {
            tk_dly_tsk(1);
        }
    }
    return err;
}

/* Cat-and-Mouse Dual-Radio Transmitter & Jam-Hop Coordinator */
static void task_iris_transmitter(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    /* Pick random initial true channels on boot (zero hardcoding) */
    s_true_wifi_ch = iris_pick_new_channel(1U, 13U, 0U);
    s_true_154_ch  = iris_pick_new_channel(11U, 26U, 0U);

    iris_init_wifi_softap();
    iris_init_ieee802154();

    uint32_t seq = 0;
    esp_err_t last_wifi_err = ESP_OK;
    esp_err_t last_154_err  = ESP_OK;

    tm_printf((UB *)"[IRIS:RF] Cat-and-mouse RF active: WiFi TrueCh=%u, Zigbee TrueCh=%u\n",
              (unsigned)s_true_wifi_ch, (unsigned)s_true_154_ch);

    for (;;) {
        seq++;

        /* Handle jam-triggered channel hopping */
        if (s_wifi_jammed) {
            s_wifi_jammed = false;
            s_wifi_evaded = true;
            uint8_t old_ch = s_true_wifi_ch;
            s_true_wifi_ch = iris_pick_new_channel(1U, 13U, old_ch);
            (void)esp_wifi_set_channel(s_true_wifi_ch, WIFI_SECOND_CHAN_NONE);
            iris_update_beacon_ie();
            tm_printf((UB *)"[IRIS:WIFI] Jam detected on Ch %u! Hopped to new random Ch %u\n",
                      (unsigned)old_ch, (unsigned)s_true_wifi_ch);
        }
        if (s_154_jammed) {
            s_154_jammed = false;
            s_154_evaded = true;
            uint8_t old_ch = s_true_154_ch;
            s_true_154_ch = iris_pick_new_channel(11U, 26U, old_ch);
            (void)esp_ieee802154_set_channel(s_true_154_ch);
            (void)esp_ieee802154_receive();
            tm_printf((UB *)"[IRIS:802154] Jam detected on Ch %u! Hopped to new random Ch %u\n",
                      (unsigned)old_ch, (unsigned)s_true_154_ch);
            gpio_set_level(IRIS_STATUS_GPIO, 0);
        }

        /* 1. Wi-Fi Phase: transmit cleartext on s_true_wifi_ch, then sweep 12 decoys cleanly */
        (void)esp_ieee802154_sleep();
        last_wifi_err = iris_send_wifi_frame(s_true_wifi_ch, true, seq);
        for (uint8_t wch = 1U; wch <= 13U; wch++) {
            if (wch != s_true_wifi_ch) {
                (void)iris_send_wifi_frame(wch, false, seq);
            }
        }
        (void)iris_send_wifi_frame(s_true_wifi_ch, true, seq);
        (void)esp_wifi_set_channel(s_true_wifi_ch, WIFI_SECOND_CHAN_NONE);

        /* 2. 802.15.4 Phase: transmit cleartext twice on s_true_154_ch + decoys */
        last_154_err = iris_send_154_frame(s_true_154_ch, true, (uint8_t)seq);
        for (uint8_t zch = 11U; zch <= 26U; zch++) {
            if (zch != s_true_154_ch) {
                (void)iris_send_154_frame(zch, false, (uint8_t)seq);
            }
        }
        (void)iris_send_154_frame(s_true_154_ch, true, (uint8_t)seq);
        (void)esp_ieee802154_set_channel(s_true_154_ch);
        (void)esp_ieee802154_receive();
        if ((seq % 10U) == 1U) {
            tm_printf((UB *)"[IRIS:RF] Seq #%lu | WiFi_Ch=%u (err=%d, cb=%lu) | 802154_Ch=%u (err=%d)\n",
                      (unsigned long)seq, (unsigned)s_true_wifi_ch, (int)last_wifi_err,
                      (unsigned long)s_espnow_cb_cnt,
                      (unsigned)s_true_154_ch, (int)last_154_err);
        }

        tk_dly_tsk(s_tx_period_ms);
    }
}

EXPORT INT usermain(void)
{
    tk_dly_tsk(50);

    tm_printf((UB *)"\n============================================================\n");
    tm_printf((UB *)"  iris - Node 3 Wireless Victim (SoftAP & 802.15.4)         \n");
    tm_printf((UB *)"  µT-Kernel 3.0 (IEEE 2050-2018) on ESP32-C6 RISC-V        \n");
    tm_printf((UB *)"============================================================\n");

    /* Create lowest-priority idle task with static stack */
    T_CTSK ctsk_idle = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG0 | TA_USERBUF,
        .task    = (FP)task_idle,
        .itskpri = CNF_MAX_TSKPRI,
        .stksz   = sizeof(s_idle_stack),
        .bufptr  = s_idle_stack,
    };
    ID tid_idle = tk_cre_tsk(&ctsk_idle);
    if (tid_idle > E_OK) {
        tk_sta_tsk(tid_idle, 0);
    }
    /* Configure GPIO8 Status Pin */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << IRIS_STATUS_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(IRIS_STATUS_GPIO, 0);

    /* Start IEEE 802.15.4 on Channel 15 */
    T_CTSK ctsk_tx = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG0 | TA_USERBUF,
        .task    = (FP)task_iris_transmitter,
        .itskpri = 8,
        .stksz   = sizeof(s_tx_stack),
        .bufptr  = s_tx_stack,
    };
    ID tid_tx = tk_cre_tsk(&ctsk_tx);
    if (tid_tx > E_OK) {
        tk_sta_tsk(tid_tx, 0);
    }

    tm_printf((UB *)"[IRIS:INIT] Initialization complete. All tasks running.\n\n");
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
