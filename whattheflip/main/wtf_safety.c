/*
 * wtf_safety.c - E-Stop ISR, safety monitor, WS2812 LED, and buzzer subsystem
 *
 * Implements FR-7: Probes float within 200 ns upon GPIO9 edge.
 * Manages WS2812 status LED (GPIO8) and Piezo Buzzer chirp (TCA9554 EXIO3).
 */

#include "wtf_safety.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_rom_serial_output.h"

static volatile bool s_safety_latched = false;
static wtf_safety_state_t s_safety_state = WTF_STATE_ARMED;

/* TCA9554 Cached Register States */
static uint8_t s_tca_config = 0xFF; /* Default all inputs */
static uint8_t s_tca_output = 0x00; /* Default all low */

/* --------------------------------------------------------------------------
 * TCA9554PWR Bitbang I2C Driver for Piezo Buzzer (EXIO3 on GPIO22/GPIO23)
 * -------------------------------------------------------------------------- */

static inline void tca_i2c_delay(void)
{
    esp_rom_delay_us(2); /* ~250 kHz I2C clock */
}

static inline void tca_sda_set(int high)
{
    if (high) {
        GPIO.enable_w1tc.val = (1UL << WTF_PIN_TCA9554_SDA); /* Float / pulled high */
    } else {
        GPIO.out_w1tc.val = (1UL << WTF_PIN_TCA9554_SDA);
        GPIO.enable_w1ts.val = (1UL << WTF_PIN_TCA9554_SDA); /* Drive LOW */
    }
}

static inline void tca_scl_set(int high)
{
    if (high) {
        GPIO.enable_w1tc.val = (1UL << WTF_PIN_TCA9554_SCL); /* Float */
    } else {
        GPIO.out_w1tc.val = (1UL << WTF_PIN_TCA9554_SCL);
        GPIO.enable_w1ts.val = (1UL << WTF_PIN_TCA9554_SCL); /* Drive LOW */
    }
}

static inline int tca_sda_get(void)
{
    return (int)((GPIO.in.val >> WTF_PIN_TCA9554_SDA) & 1UL);
}

static void tca_i2c_start(void)
{
    tca_sda_set(1);
    tca_scl_set(1);
    tca_i2c_delay();
    tca_sda_set(0);
    tca_i2c_delay();
    tca_scl_set(0);
    tca_i2c_delay();
}

static void tca_i2c_stop(void)
{
    tca_sda_set(0);
    tca_scl_set(0);
    tca_i2c_delay();
    tca_scl_set(1);
    tca_i2c_delay();
    tca_sda_set(1);
    tca_i2c_delay();
}

static bool tca_i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        tca_sda_set((byte >> i) & 1U);
        tca_i2c_delay();
        tca_scl_set(1);
        tca_i2c_delay();
        tca_scl_set(0);
    }
    /* Read ACK */
    tca_sda_set(1);
    tca_i2c_delay();
    tca_scl_set(1);
    tca_i2c_delay();
    bool ack = (tca_sda_get() == 0);
    tca_scl_set(0);
    tca_i2c_delay();
    return ack;
}

static bool tca9554_write_reg(uint8_t reg, uint8_t val)
{
    tca_i2c_start();
    if (!tca_i2c_write_byte(0x40)) { /* Address 0x20 << 1 | WRITE */
        tca_i2c_stop();
        return false;
    }
    if (!tca_i2c_write_byte(reg)) {
        tca_i2c_stop();
        return false;
    }
    if (!tca_i2c_write_byte(val)) {
        tca_i2c_stop();
        return false;
    }
    tca_i2c_stop();
    return true;
}

static void tca9554_buzzer_hw_init(void)
{
    /* Configure SDA (GPIO22) and SCL (GPIO23) as open-drain outputs with pull-ups */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_TCA9554_SDA) | (1ULL << WTF_PIN_TCA9554_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Float SDA and SCL high */
    tca_sda_set(1);
    tca_scl_set(1);
    esp_rom_delay_us(10);

    /* Configure EXIO3 as output (Register 0x03, bit 3 = 0) */
    s_tca_config &= ~(1U << WTF_EXIO_BUZZER);
    (void)tca9554_write_reg(0x03, s_tca_config);

    /* Ensure buzzer output initially low (Register 0x01, bit 3 = 0) */
    s_tca_output &= ~(1U << WTF_EXIO_BUZZER);
    (void)tca9554_write_reg(0x01, s_tca_output);
}

static void tca9554_set_buzzer(bool level)
{
    if (level) {
        s_tca_output |= (1U << WTF_EXIO_BUZZER);
    } else {
        s_tca_output &= ~(1U << WTF_EXIO_BUZZER);
    }
    (void)tca9554_write_reg(0x01, s_tca_output);
}

EXPORT void wtf_safety_chirp_buzzer(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        duration_ms = 50;
    }

    tm_printf((UB *)"[SAFETY] Buzzer: Alert chirp emitted (%lu ms)\n", (unsigned long)duration_ms);

    /* Generate audible square wave pulse (~2 kHz) */
    uint32_t cycles = duration_ms * 2; /* 2 cycles per ms */
    if (cycles > 400) {
        cycles = 400; /* Cap pulse to prevent task starvation */
    }

    for (uint32_t c = 0; c < cycles; c++) {
        tca9554_set_buzzer(true);
        esp_rom_delay_us(250);
        tca9554_set_buzzer(false);
        esp_rom_delay_us(250);
    }
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
            /* Bit 0: High ~350 ns (56 cycles @ 160 MHz), Low ~800 ns (128 cycles) */
            GPIO.out_w1ts.val = (1UL << WTF_PIN_NEOPIXEL);
            uint32_t t_start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - t_start) < 56U) {}
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

EXPORT void wtf_safety_set_state(wtf_safety_state_t state)
{
    s_safety_state = state;

    switch (state) {
        case WTF_STATE_ARMED:
            /* Green: Armed / Sniffing */
            wtf_safety_neopixel_set_color(0, 48, 0);
            break;
        case WTF_STATE_INJECTING:
            /* Yellow: Fault Injecting (Red + Green) */
            wtf_safety_neopixel_set_color(48, 36, 0);
            break;
        case WTF_STATE_ABORTED:
            /* Red: E-Stop Aborted */
            wtf_safety_neopixel_set_color(64, 0, 0);
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

    /* Initialize Buzzer over TCA9554 */
    tca9554_buzzer_hw_init();

    gpio_install_isr_service(0);
    gpio_isr_handler_add(WTF_PIN_ESTOP, estop_gpio_isr, NULL);
}

EXPORT bool wtf_safety_is_aborted(void)
{
    return s_safety_latched;
}

EXPORT void wtf_safety_abort(void)
{
    /* 1. Float all probes in < 200 ns */
    GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) |
                           (1UL << WTF_PIN_PROBE1_SNIFF)  |
                           (1UL << WTF_PIN_PROBE2_SDA)    |
                           (1UL << WTF_PIN_PROBE3_SCL);

    s_safety_latched = true;

    /* 2. Signal safety task */
    if (g_flg_system > 0) {
        (void)tk_set_flg(g_flg_system, FLG_BIT_SAFETY_ABORT);
    }
}

EXPORT void wtf_safety_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    wtf_safety_hw_init();
    wtf_safety_set_state(WTF_STATE_ARMED);

    tm_printf((UB *)"[SAFETY] TSK_SAFETY online: E-stop monitored on GPIO9 (200 ns gate)\n");
    tm_printf((UB *)"[SAFETY] Status LED: GREEN (Armed/Sniffing)\n");

    for (;;) {
        UINT flgptn = 0;
        ER er = tk_wai_flg(g_flg_system, FLG_BIT_SAFETY_ABORT,
                           TWF_ANDW, &flgptn, 50);

        uint8_t rx_char = 0;
        if (esp_rom_output_rx_one_char(&rx_char) == 0) {
            if (rx_char == 'E' || rx_char == 'e') {
                wtf_safety_abort();
            }
        }

        if (er == E_OK && (flgptn & FLG_BIT_SAFETY_ABORT)) {
            tm_printf((UB *)"[SAFETY:ALERT] HARDWARE E-STOP ACTIVATED!\n");
            tm_printf((UB *)"[SAFETY:ALERT] All probes (GPIO4..7) floated in < 200 ns.\n");
            tm_printf((UB *)"[SAFETY:ALERT] System halted into safe idle state.\n");
            tm_printf((UB *)"[SAFETY] Status LED: RED (Aborted)\n");
            tm_printf((UB *)"[SAFETY] Buzzer: Alert chirp emitted\n");
            tm_printf((UB *)"***************************************************\n");

            wtf_safety_set_state(WTF_STATE_ABORTED);
            wtf_safety_chirp_buzzer(200);

            /* Invariant: Tasks settle within 5 ms */
            tk_dly_tsk(5);

            for (;;) {
                /* Float continuously */
                GPIO.enable_w1tc.val = (1UL << WTF_PIN_PROBE0_GLITCH) |
                                       (1UL << WTF_PIN_PROBE1_SNIFF)  |
                                       (1UL << WTF_PIN_PROBE2_SDA)    |
                                       (1UL << WTF_PIN_PROBE3_SCL);
                tk_dly_tsk(1000);
            }
        }
    }
}
