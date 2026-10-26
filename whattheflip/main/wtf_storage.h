/*
 * wtf_storage.h - MicroSD bitbang SPI PCAP storage task
 *
 * Implements FR-8: PCAP opens clean in Wireshark.
 * Uses bitbang SPI on GPIO18/19/20/21 due to Maker Pi Base MISO LED load.
 */

#ifndef WTF_STORAGE_H
#define WTF_STORAGE_H

#include "wtf_types.h"

EXPORT void wtf_storage_task(INT stacd, void *exinf);

#endif /* WTF_STORAGE_H */
