/* net.h - blocking TCP client for the video and control sockets.
 *
 * scrcpy's server opens one or two sockets (video, control). A PmNet wraps a
 * single connection. All I/O is blocking and runs on PmSession's worker
 * thread; it never touches the GTK main loop.
 */
#pragma once

#include "pm-types.h"
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PmNet PmNet;

PmNet *pm_net_new (void);
void   pm_net_free (PmNet *self);

/* Connect to host:port. Typically 127.0.0.1:<forwarded-port>. */
gboolean pm_net_connect (PmNet *self, const char *host, guint16 port, GError **error);

/* Read up to `len` bytes; returns count read (0 = EOF), or -1 on error. */
gssize pm_net_read (PmNet *self, guint8 *buf, gsize len, GError **error);

/* Read exactly `len` bytes (loops until satisfied or EOF/error). */
gboolean pm_net_read_exact (PmNet *self, guint8 *buf, gsize len, GError **error);

/* Write all `len` bytes (loops on short writes). */
gboolean pm_net_write_all (PmNet *self, const guint8 *buf, gsize len, GError **error);

/* The underlying socket file descriptor, or -1 if not connected. Borrowed and
 * valid only until pm_net_close/pm_net_free. Intended for a best-effort
 * emergency write from a signal handler (where the GIO stream API is not
 * async-signal-safe); normal I/O must go through pm_net_read/write_all. */
int pm_net_get_fd (PmNet *self);

void pm_net_close (PmNet *self);

G_END_DECLS
