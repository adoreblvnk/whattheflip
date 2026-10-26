/*
 * wtf_telemetry.h - Telemetry and status dashboard task
 *
 * Implements FR-9: Dashboard telemetry updates at 1 to 2 Hz.
 * Implements FR-10: Fault alerts reach dashboard in < 200 ms.
 */

#ifndef WTF_TELEMETRY_H
#define WTF_TELEMETRY_H

#include "wtf_types.h"

EXPORT void wtf_telemetry_task(INT stacd, void *exinf);

#endif /* WTF_TELEMETRY_H */
