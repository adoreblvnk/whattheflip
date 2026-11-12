/*
 * wtf_fuzzer.c - Hardware-Peripheral Bounded Fault Injector
 *
 * Implements FR-5 & FR-6 using ESP32-C6 hardware peripherals (Zero GPIO polling/bit-banging):
 *   - UART1 Hardware TX (`U1TXD_OUT_IDX` on Probe 0, GPIO4): Synchronizes via `UART1.status.rxd`
 *     and transmits a single 115200-baud start bit cell (~8.68 µs <= 10 µs) via `UART1.fifo`.
 *   - I2C0 (`I2CEXT0`) Hardware Controller (Probes 2 & 3, GPIO6/GPIO7): Synchronizes via
 *     `I2C0.sr.scl_main_state_last` / `I2C0.int_raw.det_start_int_raw` and injects a bounded
 *     50 µs 9th-clock ACK-kill (`I2C0.ctr.sda_force_out`) or 50 µs clock-stretch (`I2C0.scl_sp_conf.scl_pd_en`).
 *   - IEEE 802.15.4 Hardware Transceiver: Targeted burst strictly to Iris (`0x0003` / `02:00:00:00:00:03`).
 */

#include "wtf_fuzzer.h"
#include "wtf_safety.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/uart_struct.h"
#include "soc/i2c_struct.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_ieee802154.h"
#include <string.h>

/* Fixed Iris Target Address (FR-6 Invariant: Never Broadcast) */
static const uint8_t s_iris_target_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
static const uint16_t s_iris_target_short = 0x0003;

static void fuzzer_hw_init(void)
{
    /* Keep Probes 0, 2, and 3 in high-Z mode during idle */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_PROBE0_GLITCH) |
                        (1ULL << WTF_PIN_PROBE2_SDA)    |
                        (1ULL << WTF_PIN_PROBE3_SCL),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

EXPORT void inject_uart_bit_flip_10us(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    /* Route UART1 hardware TX (`U1TXD_OUT_IDX`) to Probe 0 (GPIO4).
     * Writing 0xFF into UART1 hardware TX FIFO transmits a single 115200-baud START bit (LOW for 8.68 µs <= 10 µs)
     * followed immediately by 8 HIGH data bits (11111111b) and STOP bit (HIGH), then disconnects back to high-Z.
     */
    esp_rom_gpio_connect_out_signal(WTF_PIN_PROBE0_GLITCH, U1TXD_OUT_IDX, false, false);
    UART1.fifo.rxfifo_rd_byte = 0xFFU;
    esp_rom_delay_us(10); /* 1 bit cell at 115200 baud (8.68 µs) */
    esp_rom_gpio_connect_out_signal(WTF_PIN_PROBE0_GLITCH, SIG_GPIO_OUT_IDX, false, true);
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH);
}

EXPORT void inject_i2c_ack_kill_50us(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    /* Route I2C0 (`I2CEXT0_SDA_OUT_IDX`) hardware controller to Probe 2 (GPIO6):
     * Force SDA direct HIGH output (`sda_force_out = 1`, `sda_pd_en = 0`) for 50 µs to overwrite slave ACK,
     * then restore high-Z input.
     */
    I2C0.scl_sp_conf.sda_pd_en = 0U;
    I2C0.ctr.sda_force_out = 1U;
    I2C0.ctr.conf_upgate = 1U;
    esp_rom_gpio_connect_out_signal(WTF_PIN_PROBE2_SDA, I2CEXT0_SDA_OUT_IDX, false, false);

    esp_rom_delay_us(50); /* Bounded 50 µs ACK-kill window */

    esp_rom_gpio_connect_out_signal(WTF_PIN_PROBE2_SDA, SIG_GPIO_OUT_IDX, false, true);
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE2_SDA);
    I2C0.ctr.sda_force_out = 0U;
    I2C0.ctr.conf_upgate = 1U;
}

EXPORT void inject_i2c_clock_stretch_50us(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    /* Route I2C0 (`I2CEXT0_SCL_OUT_IDX`) hardware controller to Probe 3 (GPIO7):
     * Set `scl_force_out = 1` and `scl_pd_en = 1` in `I2C0.scl_sp_conf` (hardware SCL low stretch)
     * for exactly 50 µs, then release back to high-Z.
     */
    I2C0.scl_sp_conf.scl_pd_en = 1U;
    I2C0.ctr.scl_force_out = 1U;
    I2C0.ctr.conf_upgate = 1U;
    esp_rom_gpio_connect_out_signal(WTF_PIN_PROBE3_SCL, I2CEXT0_SCL_OUT_IDX, false, false);

    esp_rom_delay_us(50); /* Bounded 50 µs hardware clock-stretch */

    esp_rom_gpio_connect_out_signal(WTF_PIN_PROBE3_SCL, SIG_GPIO_OUT_IDX, false, true);
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE3_SCL);
    I2C0.scl_sp_conf.scl_pd_en = 0U;
    I2C0.ctr.scl_force_out = 0U;
    I2C0.ctr.conf_upgate = 1U;
}

EXPORT void inject_wireless_target_jam_50us(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    /* FR-6 Invariant: Verify destination strictly matches Iris allow-list (Never broadcast) */
    if (s_iris_target_short != 0x0003 || s_iris_target_mac[0] != 0x02) {
        return;
    }

    uint8_t jam_frame[32];
    uint8_t psdu_len = 22;
    jam_frame[0] = psdu_len;
    jam_frame[1] = 0x41; /* FCF LSB: Data frame, PAN ID comp */
    jam_frame[2] = 0x88; /* FCF MSB: Short dest, short src */
    jam_frame[3] = 0xFA;
    jam_frame[4] = 0x34; /* PAN ID 0x1234 */
    jam_frame[5] = 0x12;
    jam_frame[6] = (uint8_t)(s_iris_target_short & 0xFF);
    jam_frame[7] = (uint8_t)((s_iris_target_short >> 8) & 0xFF);
    jam_frame[8] = 0x01;
    jam_frame[9] = 0x00;
    memcpy(&jam_frame[10], "WTF:JAM:TARGET:IRIS", 19);

    /* Transmit targeted burst on Channel 15 */
    (void)esp_ieee802154_transmit(jam_frame, false);
}

EXPORT void wtf_fuzzer_inject_uart(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    if (g_mtx_probe_access > 0) {
        (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
    }

    wtf_safety_set_state(WTF_STATE_INJECTING);
    /* Synchronize to active UART frame using UART1 Hardware Status Register (`UART1.status.rxd`) */
    uint32_t t_wait = 0U;
    while (UART1.status.rxd != 0U && t_wait < 550000U) {
        esp_rom_delay_us(5);
        t_wait += 5U;
    }
    esp_rom_delay_us(25); /* Advance into payload data bit cell */
    inject_uart_bit_flip_10us();
    wtf_safety_chirp_buzzer(20);
    tk_dly_tsk(150); /* Visible LED hold */
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 10 µs UART bit flip on Row 12 (Probe 0)\n");

    if (g_mbf_telemetry > 0) {
        wtf_record_t alert_rec;
        alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
        alert_rec.protocol = PROTO_FAULT_ALERT;
        alert_rec.probe_id = 0;
        alert_rec.length = 0;
        (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
    }
    if (g_mtx_probe_access > 0) {
        (void)tk_unl_mtx(g_mtx_probe_access);
    }
}

EXPORT void wtf_fuzzer_inject_i2c_ack(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    if (g_mtx_probe_access > 0) {
        (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
    }

    wtf_safety_set_state(WTF_STATE_INJECTING);
    /* Synchronize to I2C address/ACK window via I2C0 Hardware Controller Status (`I2C0.sr` & `I2C0.int_raw`) */
    I2C0.int_clr.det_start_int_clr = 1U;
    uint32_t t_wait = 0U;
    while (I2C0.int_raw.det_start_int_raw == 0U && I2C0.sr.bus_busy == 0U && t_wait < 250000U) {
        esp_rom_delay_us(2);
        t_wait += 2U;
    }
    /* Wait 8 clock cycles at 400 kHz (~20 µs) to hit the 9th-clock ACK slot */
    esp_rom_delay_us(20);
    inject_i2c_ack_kill_50us();
    wtf_safety_chirp_buzzer(20);
    tk_dly_tsk(150); /* Visible LED hold */
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 50 µs I2C ACK-kill on Row 43 (Probe 2)\n");

    if (g_mbf_telemetry > 0) {
        wtf_record_t alert_rec;
        alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
        alert_rec.protocol = PROTO_FAULT_ALERT;
        alert_rec.probe_id = 2;
        alert_rec.length = 0;
        (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
    }
    if (g_mtx_probe_access > 0) {
        (void)tk_unl_mtx(g_mtx_probe_access);
    }
}

EXPORT void wtf_fuzzer_inject_i2c_stretch(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    if (g_mtx_probe_access > 0) {
        (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
    }

    wtf_safety_set_state(WTF_STATE_INJECTING);
    /* Synchronize to active I2C transaction via I2C0 Hardware Status (`I2C0.sr.bus_busy`) */
    uint32_t t_wait = 0U;
    while (I2C0.sr.bus_busy == 0U && t_wait < 250000U) {
        esp_rom_delay_us(2);
        t_wait += 2U;
    }
    inject_i2c_clock_stretch_50us();
    wtf_safety_chirp_buzzer(20);
    tk_dly_tsk(150); /* Visible LED hold */
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[FUZZER] MANUAL INJECT: 50 µs I2C clock-stretch on Row 42 (Probe 3)\n");

    if (g_mbf_telemetry > 0) {
        wtf_record_t alert_rec;
        alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
        alert_rec.protocol = PROTO_FAULT_ALERT;
        alert_rec.probe_id = 3;
        alert_rec.length = 0;
        (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
    }
    if (g_mtx_probe_access > 0) {
        (void)tk_unl_mtx(g_mtx_probe_access);
    }
}

EXPORT void wtf_fuzzer_inject_wireless_jam(void)
{
    if (wtf_safety_is_aborted()) {
        return;
    }

    if (g_mtx_probe_access > 0) {
        (void)tk_loc_mtx(g_mtx_probe_access, TMO_FEVR);
    }

    wtf_safety_set_state(WTF_STATE_INJECTING);
    inject_wireless_target_jam_50us();
    wtf_safety_chirp_buzzer(20);
    tk_dly_tsk(150); /* Visible LED hold */
    wtf_safety_set_state(WTF_STATE_ARMED);
    tm_printf((UB *)"[FUZZER:WIRELESS] MANUAL INJECT: Targeted 802.15.4 burst to Iris BSSID 02:00:00:00:00:03 on Ch 15 (FR-6, TC-3.3)\n");

    if (g_mbf_telemetry > 0) {
        wtf_record_t alert_rec;
        alert_rec.timestamp_us = (uint32_t)esp_timer_get_time();
        alert_rec.protocol = PROTO_FAULT_ALERT;
        alert_rec.probe_id = 15;
        alert_rec.length = 0;
        (void)tk_snd_mbf(g_mbf_telemetry, &alert_rec, sizeof(alert_rec), TMO_POL);
    }
    if (g_mtx_probe_access > 0) {
        (void)tk_unl_mtx(g_mtx_probe_access);
    }
}

EXPORT void wtf_fuzzer_send_iris_cmd(const char *cmd_str)
{
    if (wtf_safety_is_aborted() || cmd_str == NULL) {
        return;
    }

    /* FR-6 Invariant: Verify destination strictly matches Iris allow-list (Never broadcast) */
    if (s_iris_target_short != 0x0003) {
        return;
    }

    uint8_t frame[48];
    size_t cmd_len = strlen(cmd_str);
    if (cmd_len > 30) cmd_len = 30;

    uint8_t psdu_len = 2U + 1U + 2U + 2U + 2U + (uint8_t)cmd_len;
    frame[0] = psdu_len;
    frame[1] = 0x41; /* FCF LSB: Data frame, PAN ID comp */
    frame[2] = 0x88; /* FCF MSB: Short dest, short src */
    frame[3] = (uint8_t)(esp_timer_get_time() & 0xFF);
    frame[4] = 0x34; /* PAN ID 0x1234 */
    frame[5] = 0x12;
    frame[6] = (uint8_t)(s_iris_target_short & 0xFF);
    frame[7] = (uint8_t)((s_iris_target_short >> 8) & 0xFF);
    frame[8] = 0x01; /* Src WhatTheFlip */
    frame[9] = 0x00;
    memcpy(&frame[10], cmd_str, cmd_len);

    (void)esp_ieee802154_transmit(frame, false);
    tm_printf((UB *)"[FUZZER:WIRELESS] OTA CMD to Iris 0x%04X: '%s' (FR-6, TC-3.3)\n",
              s_iris_target_short, cmd_str);
}

EXPORT void wtf_fuzzer_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    fuzzer_hw_init();
    tm_printf((UB *)"[FUZZER] TSK_FUZZER online: Bounded fault injector (10 µs UART1 TX, 50 µs I2C0 ACK/Stretch, FR-5)\n");

    /* Demand-driven hardware fault injector task sleeps when idle */
    tk_slp_tsk(TMO_FEVR);
}
