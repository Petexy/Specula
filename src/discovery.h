/* discovery.h - background network discovery of the paired device.
 *
 * Two strategies, in order of preference:
 *   1. mDNS / DNS-SD: modern Android "wireless debugging" advertises
 *      `_adb-tls-connect._tcp` (and `_adb._tcp`). The robust path is to browse
 *      these via Avahi and match the paired device. (TODO in discovery.c.)
 *   2. ADB fallback: ask the local adb server for already-known wireless
 *      devices, and validate any saved/manual host:port with `adb connect`.
 *
 * The found-callback is always delivered on the GTK main thread, so it is safe
 * to touch widgets / PmSession state from it.
 */
#pragma once

#include "pm-types.h"

G_BEGIN_DECLS

typedef struct _PmDiscovery PmDiscovery;

/* Called on the main thread when the target becomes reachable. `found` is
 * owned by discovery and only valid for the duration of the call; copy it.
 * `user_data` must be a GObject: the worker-thread path refs it across the
 * marshal to the main thread so a late delivery can't outlive a freed owner. */
typedef void (*PmDiscoveryFoundCb) (const PmDeviceInfo *found, gpointer user_data);
typedef void (*PmDiscoveryProbeFailedCb) (gpointer user_data);

PmDiscovery *pm_discovery_new (const PmDeviceInfo *target,
                               PmDiscoveryFoundCb  cb,
                               gpointer            user_data);

/* Optional: called once on the main thread after the first saved/manual
 * fails. Discovery continues retrying after this. */
void pm_discovery_set_probe_failed_cb (PmDiscovery              *self,
                                       PmDiscoveryProbeFailedCb  cb,
                                       gpointer                  user_data);

/* Begin discovery on a worker thread plus mDNS when available. */
void pm_discovery_start (PmDiscovery *self);

/* Stop probing and join the worker. Call before freeing the owner that the
 * callback captures (e.g. PmSession). Safe to call multiple times. */
void pm_discovery_stop (PmDiscovery *self);

void pm_discovery_free (PmDiscovery *self);

G_END_DECLS
