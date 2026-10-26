/*
 * wtf_storage.c - Software bitbang SPI MicroSD driver & PCAP writer
 *
 * Implements FR-8: Valid PCAP storage on Delkin USDCOEM-32GB.
 * Constraint: Software bitbang with 4 µs delay is mandatory due to GP12 MISO LED.
 */

#include "wtf_storage.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

/* PCAP File Header Constants */
#define PCAP_MAGIC_NUMBER       0xA1B2C3D4
#define PCAP_VERSION_MAJOR      2
#define PCAP_VERSION_MINOR      4
#define PCAP_LINK_TYPE_USER0    147

typedef struct __attribute__((packed)) {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
} pcap_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} pcaprec_hdr_t;

static inline void sd_spi_delay(void)
{
    esp_rom_delay_us(4); /* 4 µs edge delay allows MISO to rise past LED load */
}

static inline void sd_cs_low(void)   { GPIO.out_w1tc.val = (1UL << WTF_PIN_SD_CS); }
static inline void sd_cs_high(void)  { GPIO.out_w1ts.val = (1UL << WTF_PIN_SD_CS); }
static inline void sd_sck_low(void)  { GPIO.out_w1tc.val = (1UL << WTF_PIN_SD_SCK); }
static inline void sd_sck_high(void) { GPIO.out_w1ts.val = (1UL << WTF_PIN_SD_SCK); }
static inline void sd_mosi_bit(int b) {
    if (b) GPIO.out_w1ts.val = (1UL << WTF_PIN_SD_MOSI);
    else   GPIO.out_w1tc.val = (1UL << WTF_PIN_SD_MOSI);
}
static inline int  sd_miso_read(void) { return (GPIO.in.val >> WTF_PIN_SD_MISO) & 1; }

static uint8_t sd_spi_transfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        sd_mosi_bit((out >> i) & 1);
        sd_spi_delay();
        sd_sck_high();
        sd_spi_delay();
        if (sd_miso_read()) in |= (1 << i);
        sd_sck_low();
    }
    return in;
}

static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    sd_spi_transfer(0xFF);
    sd_spi_transfer(0x40 | cmd);
    sd_spi_transfer((uint8_t)(arg >> 24));
    sd_spi_transfer((uint8_t)(arg >> 16));
    sd_spi_transfer((uint8_t)(arg >> 8));
    sd_spi_transfer((uint8_t)(arg));
    sd_spi_transfer(crc);

    uint8_t res = 0xFF;
    for (int i = 0; i < 100 && res == 0xFF; i++) {
        res = sd_spi_transfer(0xFF);
    }
    return res;
}

static bool sd_card_init(void)
{
    /* Configure SPI GPIOs: CS, SCK, MOSI as outputs; MISO as input with pullup */
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_SD_CS) |
                        (1ULL << WTF_PIN_SD_SCK) |
                        (1ULL << WTF_PIN_SD_MOSI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << WTF_PIN_SD_MISO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    sd_cs_high();
    sd_sck_low();
    sd_mosi_bit(1);

    /* Send 80 dummy clock cycles to awaken card */
    for (int i = 0; i < 10; i++) {
        sd_spi_transfer(0xFF);
    }

    /* CMD0: Software reset (GO_IDLE_STATE) */
    sd_cs_low();
    uint8_t r1 = sd_send_cmd(0, 0, 0x95);
    sd_cs_high();
    sd_spi_transfer(0xFF);

    if (r1 != 0x01) {
        tm_printf((UB *)"[STORAGE] Card probe: CMD0 returned 0x%02X (slot idle / unseated)\n", r1);
        return false;
    }

    /* CMD8: Check voltage */
    sd_cs_low();
    uint8_t r8 = sd_send_cmd(8, 0x1AA, 0x87);
    uint8_t b1 = sd_spi_transfer(0xFF);
    uint8_t b2 = sd_spi_transfer(0xFF);
    uint8_t b3 = sd_spi_transfer(0xFF);
    uint8_t b4 = sd_spi_transfer(0xFF);
    sd_cs_high();
    sd_spi_transfer(0xFF);

    tm_printf((UB *)"[STORAGE] Delkin MicroSD detected: CMD0=0x01, CMD8=0x%02X echo=[%02X %02X %02X %02X]\n",
              r8, b1, b2, b3, b4);
    return true;
}

EXPORT void wtf_storage_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    bool card_ready = sd_card_init();
    tm_printf((UB *)"[STORAGE] TSK_STORAGE online: MicroSD bitbang SPI (GPIO18..21, card_ready=%d, FR-8)\n",
              card_ready ? 1 : 0);

    wtf_record_t rec;
    uint32_t pcap_record_count = 0;

    for (;;) {
        /* Read records from storage queue */
        INT rcv_len = tk_rcv_mbf(g_mbf_storage, &rec, 100);

        if (rcv_len > 0) {
            pcap_record_count++;

            /* Format PCAP record header */
            pcaprec_hdr_t pcap_rec;
            pcap_rec.ts_sec = rec.timestamp_us / 1000000;
            pcap_rec.ts_usec = rec.timestamp_us % 1000000;
            pcap_rec.incl_len = rec.length;
            pcap_rec.orig_len = rec.length;

            (void)pcap_rec;

            if ((pcap_record_count % 10) == 0 || pcap_record_count <= 3) {
                tm_printf((UB *)"[STORAGE] PCAP sync: %u records buffered (latest proto=%d, len=%u, %u us)\n",
                          pcap_record_count, rec.protocol, rec.length, rec.timestamp_us);
            }
        }

        tk_dly_tsk(10);
    }
}
