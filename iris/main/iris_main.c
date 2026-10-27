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
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_ieee802154.h"

#define IRIS_WIFI_CHANNEL       6
#define IRIS_WIFI_SSID          "iris-victim"
#define IRIS_PAN_ID             0x1234
#define IRIS_SHORT_ADDR         0x0003
#define IRIS_154_CHANNEL        15

/* Static Task Stacks (Zero malloc invariant) */
#define IDLE_STACK_SZ           1024
#define TX_STACK_SZ             3072

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

/* IEEE 802.15.4 Callbacks required by driver */
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info)
{
    (void)frame_info;
    esp_ieee802154_receive_handle_done(frame);
}

void esp_ieee802154_receive_sfd_done(void)
{
}

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

    /* Set custom BSSID 02:00:00:00:00:03 */
    esp_wifi_set_mac(WIFI_IF_AP, s_iris_mac);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = IRIS_WIFI_SSID,
            .ssid_len = strlen(IRIS_WIFI_SSID),
            .channel = IRIS_WIFI_CHANNEL,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    tm_printf((UB *)"[IRIS:BEACON] SoftAP started: SSID='%s' Ch=%d BSSID=02:00:00:00:00:03\n",
              IRIS_WIFI_SSID, IRIS_WIFI_CHANNEL);
}

static void iris_init_ieee802154(void)
{
    esp_ieee802154_enable();
    esp_ieee802154_set_promiscuous(false);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_set_channel(IRIS_154_CHANNEL);
    esp_ieee802154_set_panid(IRIS_PAN_ID);
    esp_ieee802154_set_short_address(IRIS_SHORT_ADDR);
    esp_ieee802154_set_txpower(20);

    tm_printf((UB *)"[IRIS:802154] Subsystem active: Ch=%d PAN=0x%04X ADDR=0x%04X\n",
              IRIS_154_CHANNEL, IRIS_PAN_ID, IRIS_SHORT_ADDR);
}

/* Periodic 802.15.4 frame transmitter (2 Hz / 500 ms period) */
static void task_iris_transmitter(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    uint8_t seq = 0;
    uint8_t frame_buf[128];

    tm_printf((UB *)"[IRIS:802154] Periodic frame transmitter online (2 Hz on Ch %d)\n", IRIS_154_CHANNEL);

    for (;;) {
        seq++;

        char payload[32];
        int plen = snprintf(payload, sizeof(payload), "IRIS:PKT:%u", (unsigned)seq);

        uint8_t psdu_len = 2U + 1U + 2U + 2U + 2U + (uint8_t)plen;
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

        esp_err_t err = esp_ieee802154_transmit(frame_buf, false);
        if (err == ESP_OK) {
            tm_printf((UB *)"[IRIS:802154] TX Frame #%u on Ch %d (len=%u)\n",
                      (unsigned)seq, IRIS_154_CHANNEL, (unsigned)psdu_len);
        } else {
            tm_printf((UB *)"[IRIS:802154] TX Error %d on Frame #%u\n", (int)err, (unsigned)seq);
        }

        tk_dly_tsk(500);
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

    /* Start Wi-Fi SoftAP on Channel 6 with BSSID 02:00:00:00:00:03 */
    iris_init_wifi_softap();

    /* Start IEEE 802.15.4 on Channel 15 */
    iris_init_ieee802154();

    /* Launch 2 Hz transmitter task with static stack */
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
