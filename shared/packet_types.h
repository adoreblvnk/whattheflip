/*
 * packet_types.h - Unified Protocol and Packet Type Definitions for whattheflip
 *
 * Course: INF2004 Embedded Systems Programming
 * Targets: RP2040 (Hestia, Daphne) & ESP32-C6 (whattheflip)
 * Standard: Strict Barr C compliance, zero dynamic memory allocation.
 *
 * Defines:
 * - wtf_uart_pkt_t: UART telemetry frame format and metadata
 * - wtf_i2c_pkt_t:  I2C sensor telemetry and filter record
 * - Standard checksum computation: (uint16_t)(~counter)
 * - CRC-16-CCITT algorithm for sensor validation
 */

#ifndef WTF_PACKET_TYPES_H
#define WTF_PACKET_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Protocol Constants                                                         */
/* -------------------------------------------------------------------------- */

#define WTF_UART_BAUDRATE           115200U
#define WTF_I2C_FAST_BAUDRATE       400000U
#define WTF_I2C_STANDARD_BAUDRATE   100000U

/* Canonical ASCII prefixes and separators */
#define WTF_UART_PREFIX             "HESTIA:"
#define WTF_UART_PREFIX_LEN         7U
#define WTF_UART_DELIMITER          ':'
#define WTF_UART_TERMINATOR         '\n'

/* Stress test bit pattern bytes */
#define WTF_PATTERN_AA_BYTE         0xAAU /* 10101010b: max edge transition density */
#define WTF_PATTERN_55_BYTE         0x55U /* 01010101b: inverted phase transition */

/* Transmission rates */
#define WTF_UART_RATE_1HZ           1U
#define WTF_UART_RATE_2HZ           2U
#define WTF_UART_RATE_5HZ           5U
#define WTF_UART_RATE_10HZ          10U
#define WTF_UART_RATE_DEFAULT       WTF_UART_RATE_2HZ

/* Maximum length of ASCII telemetry packet: "HESTIA:4294967295:FFFF\n\0" = 24 chars */
#define WTF_UART_RAW_MAX            32U

/* -------------------------------------------------------------------------- */
/* Checksum and CRC Functions                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Macro computing 16-bit bitwise NOT checksum of counter.
 */
#define WTF_UART_CHECKSUM(counter)  ((uint16_t)(~(uint32_t)(counter)))

/**
 * @brief Compute canonical UART telemetry checksum: ~counter.
 *
 * @param counter 32-bit sequence counter.
 * @return 16-bit checksum.
 */
static inline uint16_t wtf_compute_uart_checksum(uint32_t counter)
{
    return (uint16_t)(~counter);
}

/**
 * @brief Verify UART telemetry checksum against counter.
 */
static inline bool wtf_verify_uart_checksum(uint32_t counter, uint16_t checksum)
{
    return checksum == wtf_compute_uart_checksum(counter);
}

/**
 * @brief Compute standard CRC-16-CCITT (poly 0x1021, init 0xFFFF).
 */
static inline uint16_t wtf_compute_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;
    if (data == NULL) {
        return 0U;
    }

    for (uint32_t i = 0U; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* -------------------------------------------------------------------------- */
/* Packet Type Definitions                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @enum wtf_uart_mode_t
 * @brief Operating transmission modes for UART telemetry.
 */
typedef enum {
    WTF_UART_MODE_CANONICAL   = 0, /* "HESTIA:<counter>:<checksum>\n" */
    WTF_UART_MODE_PATTERN_AA  = 1, /* Continuous 0xAA burst (10101010b) */
    WTF_UART_MODE_PATTERN_55  = 2, /* Continuous 0x55 burst (01010101b) */
    WTF_UART_MODE_ALT_BURST   = 3, /* Alternating 0xAA and 0x55 bursts */
    WTF_UART_MODE_INTERLEAVED = 4  /* Canonical frame followed by 0xAA/0x55 stress burst */
} wtf_uart_mode_t;

/**
 * @struct wtf_uart_pkt_t
 * @brief Structured representation of UART telemetry packet.
 * Fits within 40 bytes (<= 64-byte wtf_record_t buffer limit).
 */
#pragma pack(push, 1)
typedef struct {
    uint32_t counter;           /* Sequence counter */
    uint16_t checksum;          /* Computed checksum = ~counter */
    uint8_t  mode;              /* Operating mode (wtf_uart_mode_t) */
    uint8_t  raw_len;           /* Length of valid string in raw_ascii */
    char     raw_ascii[WTF_UART_RAW_MAX]; /* Canonical ASCII string */
} wtf_uart_pkt_t;
#pragma pack(pop)

/**
 * @struct wtf_i2c_pkt_t
 * @brief Structured representation of I2C dual-sensor record.
 * Fits within 32 bytes (<= 64-byte wtf_record_t buffer limit).
 */
#define WTF_I2C_FLAG_SENSOR_OK      (1U << 0)
#define WTF_I2C_FLAG_RECOVERY_DONE  (1U << 1)
#define WTF_I2C_FLAG_ABORT_OCCURRED (1U << 2)

#pragma pack(push, 1)
typedef struct {
    uint32_t timestamp_us;      /* Microsecond timestamp */
    uint32_t sample_seq;        /* Sample sequence counter */
    int16_t  accel_x;           /* Instantaneous accelerometer X (mg) */
    int16_t  accel_y;           /* Instantaneous accelerometer Y (mg) */
    int16_t  accel_z;           /* Instantaneous accelerometer Z (mg) */
    int16_t  accel_filt_x;      /* Moving-average filtered X (mg) */
    int16_t  accel_filt_y;      /* Moving-average filtered Y (mg) */
    int16_t  accel_filt_z;      /* Moving-average filtered Z (mg) */
    int16_t  mag_x;             /* Instantaneous magnetometer X (mG) */
    int16_t  mag_y;             /* Instantaneous magnetometer Y (mG) */
    int16_t  mag_z;             /* Instantaneous magnetometer Z (mG) */
    uint8_t  bus_id;            /* I2C peripheral bus (0 = GP4/5, 1 = GP6/7) */
    uint8_t  status_flags;      /* Bitmask of WTF_I2C_FLAG_* */
    uint16_t crc16;             /* CRC-16 of packet contents */
} wtf_i2c_pkt_t;
#pragma pack(pop)

/* -------------------------------------------------------------------------- */
/* Packet Formatting & Parsing Helpers (Zero Malloc / Zero Libc printf)      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Deterministically formats canonical telemetry line into buffer:
 *        "HESTIA:<counter>:<checksum>\n"
 *
 * @param buf Output char buffer (must be at least 28 bytes).
 * @param max_len Size of buffer.
 * @param counter Sequence counter.
 * @param checksum 16-bit checksum (normally ~counter).
 * @return Number of characters written (excluding null terminator), or -1 if truncated.
 */
static inline int wtf_format_canonical_uart(char *buf, uint32_t max_len, uint32_t counter, uint16_t checksum)
{
    if (buf == NULL || max_len < 28U) {
        return -1;
    }

    const char prefix[] = WTF_UART_PREFIX;
    uint32_t idx = 0U;

    for (uint32_t i = 0U; prefix[i] != '\0'; i++) {
        buf[idx++] = prefix[i];
    }

    char num_rev[12];
    uint32_t num_len = 0U;
    uint32_t c = counter;
    if (c == 0U) {
        num_rev[num_len++] = '0';
    } else {
        while (c > 0U) {
            num_rev[num_len++] = (char)('0' + (c % 10U));
            c /= 10U;
        }
    }
    while (num_len > 0U) {
        buf[idx++] = num_rev[--num_len];
    }

    buf[idx++] = WTF_UART_DELIMITER;

    const char hex_chars[] = "0123456789ABCDEF";
    buf[idx++] = hex_chars[(checksum >> 12) & 0x0FU];
    buf[idx++] = hex_chars[(checksum >> 8)  & 0x0FU];
    buf[idx++] = hex_chars[(checksum >> 4)  & 0x0FU];
    buf[idx++] = hex_chars[checksum & 0x0FU];

    buf[idx++] = WTF_UART_TERMINATOR;
    buf[idx] = '\0';

    return (int)idx;
}

/**
 * @brief Parses canonical telemetry line: "HESTIA:<counter>:<checksum>\n"
 *
 * @param buf Null-terminated or line-terminated input buffer.
 * @param out_ctr Output pointer for parsed counter.
 * @param out_csum Output pointer for parsed checksum.
 * @return true if successfully parsed, false otherwise.
 */
static inline bool wtf_parse_canonical_uart(const char *buf, uint32_t *out_ctr, uint16_t *out_csum)
{
    if (buf == NULL || out_ctr == NULL || out_csum == NULL) {
        return false;
    }

    /* Locate "HESTIA:" */
    const char *p = buf;
    while (*p != '\0') {
        if (p[0] == 'H' && p[1] == 'E' && p[2] == 'S' &&
            p[3] == 'T' && p[4] == 'I' && p[5] == 'A' && p[6] == ':') {
            p += 7;
            break;
        }
        p++;
    }
    if (*p == '\0') {
        return false;
    }

    /* Parse counter decimal digits */
    uint32_t ctr = 0U;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        ctr = (ctr * 10U) + (uint32_t)(*p - '0');
        p++;
    }

    if (*p != ':') {
        return false;
    }
    p++;

    /* Parse up to 4 hex digits */
    uint16_t csum = 0U;
    int digits = 0;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        char ch = *p;
        uint8_t nibble = 0U;
        if (ch >= '0' && ch <= '9') {
            nibble = (uint8_t)(ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
            nibble = (uint8_t)(ch - 'A' + 10);
        } else if (ch >= 'a' && ch <= 'f') {
            nibble = (uint8_t)(ch - 'a' + 10);
        } else {
            break;
        }
        csum = (uint16_t)((csum << 4) | nibble);
        digits++;
        p++;
    }

    if (digits == 0) {
        return false;
    }

    *out_ctr = ctr;
    *out_csum = csum;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* WTF_PACKET_TYPES_H */
