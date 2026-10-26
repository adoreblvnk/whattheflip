/*
 * wtf_storage.c - Software bitbang SPI MicroSD FAT32 driver & PCAP writer
 *
 * Implements FR-8: Valid PCAP storage on Delkin USDCOEM-32GB (TC-4.2).
 * Implements Gap A4: 32 KB buffering absorbs up to 40 ms card erase latency.
 * Constraint: Software bitbang with 4 µs delay is mandatory due to GP12 MISO LED.
 */

#include "wtf_storage.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include <string.h>

/* PCAP File Header Constants */
#define PCAP_MAGIC_NUMBER       0xA1B2C3D4U
#define PCAP_VERSION_MAJOR      2U
#define PCAP_VERSION_MINOR      4U
#define PCAP_LINK_TYPE_USER0    147U

#pragma pack(push, 1)
typedef struct {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
} pcap_hdr_t;

typedef struct {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} pcaprec_hdr_t;

/* FAT32 BIOS Parameter Block (BPB) */
typedef struct {
    uint8_t  jump_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint16_t root_ent_cnt;
    uint16_t tot_sec_16;
    uint8_t  media;
    uint16_t fat_sz_16;
    uint16_t sec_per_trk;
    uint16_t num_heads;
    uint32_t hidd_sec;
    uint32_t tot_sec_32;
    uint32_t fat_sz_32;
    uint16_t ext_flags;
    uint16_t fs_ver;
    uint32_t root_clus;
    uint16_t fs_info;
    uint16_t bk_boot_sec;
    uint8_t  reserved[12];
    uint8_t  drv_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t vol_id;
    uint8_t  vol_lab[11];
    uint8_t  fil_sys_type[8];
} fat32_bpb_t;

/* FAT32 Directory Entry */
typedef struct {
    uint8_t  name[11];      /* 8.3 format: "CAPTURE PCAP" */
    uint8_t  attr;          /* 0x20 = Archive */
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat32_dirent_t;
#pragma pack(pop)

/* FAT32 Volume and File State */
typedef struct {
    bool     mounted;
    bool     is_sdhc;
    uint32_t lba_start;
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint32_t fat_sz_32;
    uint32_t fat_start_sec;
    uint32_t root_clus;
    uint32_t data_start_sec;

    /* /capture.pcap file state */
    uint32_t file_start_clus;
    uint32_t file_curr_clus;
    uint32_t file_clus_sec_idx;
    uint32_t file_size_bytes;
    uint32_t dirent_sec;
    uint16_t dirent_idx;
} fat32_state_t;

/* 32 KB Buffering to absorb up to 40 ms card erase latency (Gap A4) */
#define STORAGE_BUF_SIZE        (32U * 1024U)

static fat32_state_t s_fat;
static bool          s_card_ready = false;
static uint8_t       s_sector_scratch[512];
static uint8_t       s_pcap_buf[STORAGE_BUF_SIZE];
static uint32_t      s_buf_write_pos = 0U;
static uint32_t      s_buf_flushed_pos = 0U;
static uint32_t      s_total_pcap_bytes = 0U;
static uint32_t      s_total_records = 0U;

/* -------------------------------------------------------------------------- */
/* Low-Level Bitbang SPI Driver                                               */
/* -------------------------------------------------------------------------- */

static inline void sd_spi_delay(void)
{
    esp_rom_delay_us(4); /* 4 µs edge delay allows MISO to rise past LED load */
}

static inline void sd_cs_low(void)   { GPIO.out_w1tc.val = (1UL << WTF_PIN_SD_CS); }
static inline void sd_cs_high(void)  { GPIO.out_w1ts.val = (1UL << WTF_PIN_SD_CS); }
static inline void sd_sck_low(void)  { GPIO.out_w1tc.val = (1UL << WTF_PIN_SD_SCK); }
static inline void sd_sck_high(void) { GPIO.out_w1ts.val = (1UL << WTF_PIN_SD_SCK); }

static inline void sd_mosi_bit(int b)
{
    if (b) {
        GPIO.out_w1ts.val = (1UL << WTF_PIN_SD_MOSI);
    } else {
        GPIO.out_w1tc.val = (1UL << WTF_PIN_SD_MOSI);
    }
}

static inline int sd_miso_read(void)
{
    return (int)((GPIO.in.val >> WTF_PIN_SD_MISO) & 1U);
}

static uint8_t sd_spi_transfer(uint8_t out)
{
    uint8_t in = 0U;
    for (int i = 7; i >= 0; i--) {
        sd_mosi_bit((out >> i) & 1);
        sd_spi_delay();
        sd_sck_high();
        sd_spi_delay();
        if (sd_miso_read()) {
            in |= (uint8_t)(1U << i);
        }
        sd_sck_low();
    }
    return in;
}

static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    sd_spi_transfer(0xFFU);
    sd_spi_transfer(0x40U | cmd);
    sd_spi_transfer((uint8_t)(arg >> 24));
    sd_spi_transfer((uint8_t)(arg >> 16));
    sd_spi_transfer((uint8_t)(arg >> 8));
    sd_spi_transfer((uint8_t)(arg));
    sd_spi_transfer(crc);

    uint8_t res = 0xFFU;
    for (int i = 0; i < 100 && res == 0xFFU; i++) {
        res = sd_spi_transfer(0xFFU);
    }
    return res;
}

static bool sd_read_sector(uint32_t sector, uint8_t *buf)
{
    if (buf == NULL) {
        return false;
    }

    uint32_t addr = s_fat.is_sdhc ? sector : (sector * 512U);

    sd_cs_low();
    uint8_t r1 = sd_send_cmd(17U, addr, 0xFFU);
    if (r1 != 0x00U) {
        sd_cs_high();
        sd_spi_transfer(0xFFU);
        return false;
    }

    /* Wait for data start token 0xFE (up to 20000 transfers) */
    uint8_t token = 0xFFU;
    for (uint32_t i = 0U; i < 20000U && token == 0xFFU; i++) {
        token = sd_spi_transfer(0xFFU);
    }

    if (token != 0xFEU) {
        sd_cs_high();
        sd_spi_transfer(0xFFU);
        return false;
    }

    for (int i = 0; i < 512; i++) {
        buf[i] = sd_spi_transfer(0xFFU);
    }

    /* Read 2 CRC bytes */
    (void)sd_spi_transfer(0xFFU);
    (void)sd_spi_transfer(0xFFU);

    sd_cs_high();
    sd_spi_transfer(0xFFU);
    return true;
}

static bool sd_write_sector(uint32_t sector, const uint8_t *buf)
{
    if (buf == NULL) {
        return false;
    }

    uint32_t addr = s_fat.is_sdhc ? sector : (sector * 512U);

    sd_cs_low();
    uint8_t r1 = sd_send_cmd(24U, addr, 0xFFU);
    if (r1 != 0x00U) {
        sd_cs_high();
        sd_spi_transfer(0xFFU);
        return false;
    }

    sd_spi_transfer(0xFFU);
    sd_spi_transfer(0xFEU); /* Data start token */

    for (int i = 0; i < 512; i++) {
        sd_spi_transfer(buf[i]);
    }

    /* Send 2 dummy CRC bytes */
    sd_spi_transfer(0xFFU);
    sd_spi_transfer(0xFFU);

    /* Read data response byte */
    uint8_t resp = sd_spi_transfer(0xFFU);
    if ((resp & 0x1FU) != 0x05U) { /* 0x05 = Data accepted */
        sd_cs_high();
        sd_spi_transfer(0xFFU);
        return false;
    }

    /* Wait while card programs (busy MISO low). Timeout 50000 loops (~200 ms to absorb 40 ms erase stall) */
    uint32_t busy_timeout = 50000U;
    while (sd_spi_transfer(0xFFU) == 0x00U && --busy_timeout > 0U) {
        sd_spi_delay();
    }

    sd_cs_high();
    sd_spi_transfer(0xFFU);
    return (busy_timeout > 0U);
}

static bool sd_card_init(void)
{
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
        sd_spi_transfer(0xFFU);
    }

    /* CMD0: Software reset (GO_IDLE_STATE) */
    sd_cs_low();
    uint8_t r1 = sd_send_cmd(0U, 0U, 0x95U);
    sd_cs_high();
    sd_spi_transfer(0xFFU);

    if (r1 != 0x01U) {
        tm_printf((UB *)"[STORAGE] Card probe: CMD0 returned 0x%02X (slot idle / unseated)\n", r1);
        return false;
    }

    /* CMD8: Check voltage & interface condition */
    sd_cs_low();
    uint8_t r8 = sd_send_cmd(8U, 0x1AAU, 0x87U);
    uint8_t b1 = sd_spi_transfer(0xFFU);
    uint8_t b2 = sd_spi_transfer(0xFFU);
    uint8_t b3 = sd_spi_transfer(0xFFU);
    uint8_t b4 = sd_spi_transfer(0xFFU);
    sd_cs_high();
    sd_spi_transfer(0xFFU);

    if (r8 != 0x01U || b3 != 0x01U || b4 != 0xAAU) {
        tm_printf((UB *)"[STORAGE] CMD8 error: r8=0x%02X echo=[%02X %02X %02X %02X]\n", r8, b1, b2, b3, b4);
        return false;
    }

    /* ACMD41 loop: Initialize card and wait until ready */
    bool ready = false;
    for (int loop = 0; loop < 1000; loop++) {
        sd_cs_low();
        sd_send_cmd(55U, 0U, 0x65U); /* CMD55 (APP_CMD) */
        sd_cs_high();
        sd_spi_transfer(0xFFU);

        sd_cs_low();
        uint8_t r_acmd = sd_send_cmd(41U, 0x40000000U, 0x77U); /* ACMD41 with HCS */
        sd_cs_high();
        sd_spi_transfer(0xFFU);

        if (r_acmd == 0x00U) {
            ready = true;
            break;
        }
        esp_rom_delay_us(1000);
    }

    if (!ready) {
        tm_printf((UB *)"[STORAGE] ACMD41 initialization timeout\n");
        return false;
    }

    /* CMD58: Read OCR to determine SDHC vs SDSC */
    sd_cs_low();
    uint8_t r58 = sd_send_cmd(58U, 0U, 0xFDU);
    uint8_t ocr0 = sd_spi_transfer(0xFFU);
    (void)sd_spi_transfer(0xFFU);
    (void)sd_spi_transfer(0xFFU);
    (void)sd_spi_transfer(0xFFU);
    sd_cs_high();
    sd_spi_transfer(0xFFU);

    s_fat.is_sdhc = (r58 == 0x00U) && ((ocr0 & 0x40U) != 0U);

    tm_printf((UB *)"[STORAGE] Delkin MicroSD detected: CMD0=0x01, CMD8=0x%02X, SDHC=%d (FR-8)\n",
              r8, s_fat.is_sdhc ? 1 : 0);
    return true;
}

/* -------------------------------------------------------------------------- */
/* FAT32 File System Implementation                                           */
/* -------------------------------------------------------------------------- */

static inline uint32_t clus_to_lba(uint32_t clus)
{
    return s_fat.data_start_sec + (clus - 2U) * (uint32_t)s_fat.sec_per_clus;
}

static uint32_t fat32_find_free_cluster(void)
{
    if (!sd_read_sector(s_fat.fat_start_sec, s_sector_scratch)) {
        return 3U;
    }

    uint32_t *fat_entries = (uint32_t *)s_sector_scratch;
    uint32_t free_clus = 3U;

    for (uint32_t c = 3U; c < 128U; c++) {
        if ((fat_entries[c] & 0x0FFFFFFFU) == 0U) {
            free_clus = c;
            fat_entries[c] = 0x0FFFFFFFU; /* Mark EOC */
            break;
        }
    }

    (void)sd_write_sector(s_fat.fat_start_sec, s_sector_scratch);
    if (s_fat.num_fats > 1U) {
        (void)sd_write_sector(s_fat.fat_start_sec + s_fat.fat_sz_32, s_sector_scratch);
    }

    return free_clus;
}

static bool fat32_link_cluster(uint32_t prev_clus, uint32_t next_clus)
{
    if (!sd_read_sector(s_fat.fat_start_sec, s_sector_scratch)) {
        return false;
    }

    uint32_t *fat_entries = (uint32_t *)s_sector_scratch;
    fat_entries[prev_clus] = next_clus;
    fat_entries[next_clus] = 0x0FFFFFFFU;

    bool ok = sd_write_sector(s_fat.fat_start_sec, s_sector_scratch);
    if (s_fat.num_fats > 1U) {
        (void)sd_write_sector(s_fat.fat_start_sec + s_fat.fat_sz_32, s_sector_scratch);
    }
    return ok;
}

static void fat32_update_filesize(uint32_t size)
{
    if (!s_fat.mounted || s_fat.dirent_sec == 0U) {
        return;
    }

    if (sd_read_sector(s_fat.dirent_sec, s_sector_scratch)) {
        fat32_dirent_t *entries = (fat32_dirent_t *)s_sector_scratch;
        entries[s_fat.dirent_idx].file_size = size;
        (void)sd_write_sector(s_fat.dirent_sec, s_sector_scratch);
        s_fat.file_size_bytes = size;
    }
}

static bool fat32_mount(void)
{
    /* Read Sector 0 to check for MBR or direct VBR */
    if (!sd_read_sector(0U, s_sector_scratch)) {
        tm_printf((UB *)"[STORAGE] Failed to read Sector 0\n");
        return false;
    }

    s_fat.lba_start = 0U;
    if (s_sector_scratch[0x1FE] == 0x55U && s_sector_scratch[0x1FF] == 0xAAU) {
        uint8_t part_type = s_sector_scratch[0x1BE + 4];
        if (part_type == 0x0BU || part_type == 0x0CU) {
            uint32_t lba_offset = 0U;
            memcpy(&lba_offset, &s_sector_scratch[0x1BE + 8], sizeof(uint32_t));
            s_fat.lba_start = lba_offset;
        }
    }

    /* Read VBR Sector at lba_start */
    if (!sd_read_sector(s_fat.lba_start, s_sector_scratch)) {
        tm_printf((UB *)"[STORAGE] Failed to read VBR at LBA %u\n", (unsigned)s_fat.lba_start);
        return false;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)s_sector_scratch;
    s_fat.bytes_per_sec = bpb->bytes_per_sec;
    s_fat.sec_per_clus  = bpb->sec_per_clus;
    s_fat.rsvd_sec_cnt  = bpb->rsvd_sec_cnt;
    s_fat.num_fats      = bpb->num_fats;
    s_fat.fat_sz_32     = bpb->fat_sz_32;
    s_fat.root_clus     = bpb->root_clus;

    if (s_fat.bytes_per_sec != 512U || s_fat.sec_per_clus == 0U) {
        tm_printf((UB *)"[STORAGE] Invalid FAT32 BPB: bytes/sec=%u, sec/clus=%u\n",
                  s_fat.bytes_per_sec, s_fat.sec_per_clus);
        return false;
    }

    s_fat.fat_start_sec  = s_fat.lba_start + s_fat.rsvd_sec_cnt;
    s_fat.data_start_sec = s_fat.fat_start_sec + ((uint32_t)s_fat.num_fats * s_fat.fat_sz_32);

    /* Look for or create /capture.pcap in root directory */
    uint32_t root_lba = clus_to_lba(s_fat.root_clus);
    if (!sd_read_sector(root_lba, s_sector_scratch)) {
        tm_printf((UB *)"[STORAGE] Failed to read root directory at LBA %u\n", (unsigned)root_lba);
        return false;
    }

    fat32_dirent_t *entries = (fat32_dirent_t *)s_sector_scratch;
    const uint8_t target_name[11] = {'C','A','P','T','U','R','E','P','C','A','P'};
    bool found = false;
    int empty_slot = -1;

    for (int i = 0; i < 16; i++) {
        if (memcmp(entries[i].name, target_name, 11) == 0) {
            found = true;
            s_fat.dirent_sec = root_lba;
            s_fat.dirent_idx = (uint16_t)i;
            s_fat.file_start_clus = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
            s_fat.file_curr_clus = s_fat.file_start_clus;
            s_fat.file_size_bytes = entries[i].file_size;
            s_fat.file_clus_sec_idx = 0U;
            break;
        }
        if (empty_slot < 0 && (entries[i].name[0] == 0x00U || entries[i].name[0] == 0xE5U)) {
            empty_slot = i;
        }
    }

    if (!found) {
        if (empty_slot < 0) {
            empty_slot = 0;
        }
        uint32_t new_clus = fat32_find_free_cluster();
        if (new_clus < 2U) {
            new_clus = 3U;
        }

        memset(&entries[empty_slot], 0, sizeof(fat32_dirent_t));
        memcpy(entries[empty_slot].name, target_name, 11);
        entries[empty_slot].attr = 0x20U; /* Archive */
        entries[empty_slot].fst_clus_hi = (uint16_t)(new_clus >> 16);
        entries[empty_slot].fst_clus_lo = (uint16_t)(new_clus & 0xFFFFU);
        entries[empty_slot].file_size = 0U;

        s_fat.dirent_sec = root_lba;
        s_fat.dirent_idx = (uint16_t)empty_slot;
        s_fat.file_start_clus = new_clus;
        s_fat.file_curr_clus = new_clus;
        s_fat.file_clus_sec_idx = 0U;
        s_fat.file_size_bytes = 0U;

        (void)sd_write_sector(root_lba, s_sector_scratch);
    }

    s_fat.mounted = true;
    tm_printf((UB *)"[STORAGE] FAT32 mounted: cluster_sz=%u, file=/capture.pcap size=%u (FR-8, TC-4.2)\n",
              (unsigned)(s_fat.sec_per_clus * 512U), (unsigned)s_fat.file_size_bytes);
    return true;
}

/* -------------------------------------------------------------------------- */
/* PCAP Export Helpers for Dashboard HTTP GET /capture.pcap                   */
/* -------------------------------------------------------------------------- */

EXPORT size_t wtf_storage_get_pcap_size(void)
{
    return s_total_pcap_bytes;
}

EXPORT size_t wtf_storage_read_pcap(size_t offset, uint8_t *dest, size_t max_len)
{
    if (offset >= s_total_pcap_bytes || dest == NULL || max_len == 0U) {
        return 0U;
    }

    size_t available = (size_t)s_total_pcap_bytes - offset;
    size_t to_copy = (available < max_len) ? available : max_len;

    /* If request fits within linear buffer starting from offset */
    if (offset + to_copy <= s_buf_write_pos && s_buf_flushed_pos == 0U) {
        memcpy(dest, &s_pcap_buf[offset], to_copy);
        return to_copy;
    }

    /* If mounted on SD card, read sector directly */
    if (s_fat.mounted && s_fat.file_start_clus >= 2U) {
        uint32_t sector_idx = (uint32_t)(offset / 512U);
        uint32_t sec_offset = (uint32_t)(offset % 512U);
        uint32_t lba = clus_to_lba(s_fat.file_start_clus) + sector_idx;
        if (sd_read_sector(lba, s_sector_scratch)) {
            size_t sec_avail = 512U - sec_offset;
            size_t chunk = (to_copy < sec_avail) ? to_copy : sec_avail;
            memcpy(dest, &s_sector_scratch[sec_offset], chunk);
            return chunk;
        }
    }

    /* Fallback: copy what is reachable in RAM buffer */
    if (offset < s_buf_write_pos) {
        size_t chunk = (s_buf_write_pos - offset < to_copy) ? (s_buf_write_pos - offset) : to_copy;
        memcpy(dest, &s_pcap_buf[offset], chunk);
        return chunk;
    }

    return 0U;
}

EXPORT bool wtf_storage_is_card_ready(void)
{
    return s_card_ready;
}

/* -------------------------------------------------------------------------- */
/* Main Storage Task                                                          */
/* -------------------------------------------------------------------------- */

EXPORT void wtf_storage_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    memset(&s_fat, 0, sizeof(s_fat));
    s_card_ready = sd_card_init();
    if (s_card_ready) {
        (void)fat32_mount();
    }

    tm_printf((UB *)"[STORAGE] TSK_STORAGE online: MicroSD bitbang SPI (GPIO18..21, card_ready=%d, FR-8, TC-4.2)\n",
              s_card_ready ? 1 : 0);

    /* Initialize PCAP file header (24 bytes) in 32 KB buffer */
    pcap_hdr_t pcap_hdr = {
        .magic_number = PCAP_MAGIC_NUMBER,
        .version_major = (uint16_t)PCAP_VERSION_MAJOR,
        .version_minor = (uint16_t)PCAP_VERSION_MINOR,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = 65535U,
        .network = (uint32_t)PCAP_LINK_TYPE_USER0,
    };

    memcpy(&s_pcap_buf[0], &pcap_hdr, sizeof(pcap_hdr_t));
    s_buf_write_pos = sizeof(pcap_hdr_t);
    s_buf_flushed_pos = 0U;
    s_total_pcap_bytes = sizeof(pcap_hdr_t);
    s_total_records = 0U;

    wtf_record_t rec;

    for (;;) {
        /* Read records from storage queue with 100 ms timeout */
        INT rcv_len = tk_rcv_mbf(g_mbf_storage, &rec, 100);

        if (rcv_len > 0) {
            /* Format 16-byte PCAP packet header */
            pcaprec_hdr_t pcap_rec;
            pcap_rec.ts_sec = rec.timestamp_us / 1000000U;
            pcap_rec.ts_usec = rec.timestamp_us % 1000000U;
            pcap_rec.incl_len = (uint32_t)rec.length;
            pcap_rec.orig_len = (uint32_t)rec.length;

            uint32_t rec_total_len = (uint32_t)(sizeof(pcaprec_hdr_t) + rec.length);

            /* Check space in 32 KB buffer; compact flushed sectors if necessary (Gap A4) */
            if (s_buf_write_pos + rec_total_len > STORAGE_BUF_SIZE) {
                uint32_t unflushed = s_buf_write_pos - s_buf_flushed_pos;
                if (s_buf_flushed_pos > 0U && unflushed > 0U) {
                    memmove(&s_pcap_buf[0], &s_pcap_buf[s_buf_flushed_pos], unflushed);
                    s_buf_write_pos = unflushed;
                    s_buf_flushed_pos = 0U;
                }
            }

            /* Append record to 32 KB write buffer */
            if (s_buf_write_pos + rec_total_len <= STORAGE_BUF_SIZE) {
                memcpy(&s_pcap_buf[s_buf_write_pos], &pcap_rec, sizeof(pcaprec_hdr_t));
                s_buf_write_pos += sizeof(pcaprec_hdr_t);

                if (rec.length > 0U) {
                    memcpy(&s_pcap_buf[s_buf_write_pos], rec.data, rec.length);
                    s_buf_write_pos += rec.length;
                }

                s_total_pcap_bytes += rec_total_len;
                s_total_records++;
            }

            /* Flush complete 512-byte sectors to MicroSD */
            while ((s_buf_write_pos - s_buf_flushed_pos) >= 512U) {
                if (s_fat.mounted) {
                    uint32_t lba = clus_to_lba(s_fat.file_curr_clus) + s_fat.file_clus_sec_idx;
                    bool ok = sd_write_sector(lba, &s_pcap_buf[s_buf_flushed_pos]);
                    if (ok) {
                        s_fat.file_clus_sec_idx++;
                        if (s_fat.file_clus_sec_idx >= s_fat.sec_per_clus) {
                            uint32_t next_clus = fat32_find_free_cluster();
                            if (next_clus >= 2U) {
                                (void)fat32_link_cluster(s_fat.file_curr_clus, next_clus);
                                s_fat.file_curr_clus = next_clus;
                            }
                            s_fat.file_clus_sec_idx = 0U;
                        }
                        fat32_update_filesize(s_total_pcap_bytes);
                    }
                }
                s_buf_flushed_pos += 512U;
            }

            if ((s_total_records % 10U) == 0U || s_total_records <= 3U) {
                tm_printf((UB *)"[STORAGE] PCAP sync: %u records buffered (latest proto=%d, len=%u, %u us)\n",
                          (unsigned)s_total_records, rec.protocol, (unsigned)rec.length, (unsigned)rec.timestamp_us);
            }
        } else {
            /* Queue idle: flush partial sector to disk and update directory file size */
            if (s_fat.mounted && (s_buf_write_pos > s_buf_flushed_pos)) {
                uint32_t unflushed = s_buf_write_pos - s_buf_flushed_pos;
                if (unflushed < 512U) {
                    memset(s_sector_scratch, 0, 512);
                    memcpy(s_sector_scratch, &s_pcap_buf[s_buf_flushed_pos], unflushed);
                    uint32_t lba = clus_to_lba(s_fat.file_curr_clus) + s_fat.file_clus_sec_idx;
                    (void)sd_write_sector(lba, s_sector_scratch);
                    fat32_update_filesize(s_total_pcap_bytes);
                }
            }
        }

        tk_dly_tsk(10);
    }
}
