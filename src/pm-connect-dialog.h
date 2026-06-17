/* pm-connect-dialog.h - device setup: pairing and manual connection sheets.
 *
 * Pair & Connect runs one-time Android 11+ wireless-debugging pairing
 * (`adb pair`), then automatic discovery. Manual connection is a separate
 * fallback sheet for entering the device IP[:port].
 */
#pragma once

#include <adwaita.h>
#include "pm-types.h"

G_BEGIN_DECLS

#define PM_TYPE_CONNECT_DIALOG (pm_connect_dialog_get_type ())
G_DECLARE_FINAL_TYPE (PmConnectDialog, pm_connect_dialog, PM, CONNECT_DIALOG, AdwDialog)

/* Invoked when the user confirms manual connection, or with host == NULL to
 * start automatic discovery after successful pairing. */
typedef void (*PmConnectCb) (const char *host, guint16 port, const char *name,
                             gpointer user_data);

PmConnectDialog *pm_connect_dialog_new (PmConnectCb cb, gpointer user_data);
PmConnectDialog *pm_manual_connect_dialog_new (PmConnectCb cb, gpointer user_data);

G_END_DECLS
