/* device.h - persistence of the paired device.
 *
 * Pairing happens once (USB handoff or `adb pair` over Wi-Fi). The stored
 * fields are limited to automatic reconnect metadata:
 * its friendly name and last-known host:port. Stored as a GKeyFile
 * under $XDG_CONFIG_HOME/specula/device.ini.
 */
#pragma once

#include "pm-types.h"

G_BEGIN_DECLS

/* TRUE if a device has been paired and persisted. */
gboolean pm_device_has_pairing (void);

/* Load the paired device into `out` (caller owns the strings inside).
 * Returns FALSE with `error` set if nothing is stored or the file is bad. */
gboolean pm_device_load (PmDeviceInfo *out, GError **error);

/* Persist `info`. Overwrites any previous pairing. */
gboolean pm_device_save (const PmDeviceInfo *info, GError **error);

/* Forget the current pairing. */
void pm_device_forget (void);

G_END_DECLS
