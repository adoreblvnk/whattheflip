/*
 * wtf_dashboard.h - Wi-Fi SoftAP and WebSocket web dashboard task
 *
 * Implements FR-9: Real-time telemetry broadcast at 1..2 Hz.
 * Implements FR-10: Immediate fault alert delivery in < 200 ms.
 */

#ifndef WTF_DASHBOARD_H
#define WTF_DASHBOARD_H

#include "wtf_types.h"

EXPORT void wtf_dashboard_task(INT stacd, void *exinf);
EXPORT void wtf_dashboard_broadcast_alert(const char *msg);

#endif /* WTF_DASHBOARD_H */
