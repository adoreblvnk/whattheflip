/*
 * wtf_storage.h - MicroSD bitbang SPI FAT32 PCAP storage task
 *
 * Implements FR-8: PCAP opens clean in Wireshark.
 * Implements Gap A4: 32 KB write buffering to absorb up to 40 ms card erase latency.
 * Uses bitbang SPI on GPIO18/19/20/21 due to Maker Pi Base MISO LED load.
 */

#ifndef WTF_STORAGE_H
#define WTF_STORAGE_H

#include "wtf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

EXPORT void   wtf_storage_task(INT stacd, void *exinf);

/* PCAP inspection and download helpers for HTTP GET /capture.pcap (FR-8, TC-4.2) */
EXPORT size_t wtf_storage_get_pcap_size(void);
EXPORT size_t wtf_storage_read_pcap(size_t offset, uint8_t *dest, size_t max_len);
EXPORT bool   wtf_storage_is_card_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* WTF_STORAGE_H */
