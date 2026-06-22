/* adb.h - thin wrapper over the `adb` CLI via GSubprocess.
 *
 * Uses the system `adb` executable rather than implementing the host protocol
 * directly. The CLI is the supported, stable interface and matches scrcpy's
 * integration model. All calls are blocking and therefore MUST run off the UI
 * thread (PmSession runs them on a worker; see pm-session.c).
 */
#pragma once

#include "pm-types.h"
#include <gio/gio.h>

G_BEGIN_DECLS

/* Result of probing the device's keyguard/lock state. UNKNOWN means it could
 * not be read (the dumpsys fields are version/OEM-dependent, so callers should
 * treat it as "give up gracefully" rather than an error). */
typedef enum {
  PM_LOCK_UNKNOWN  = -1,
  PM_LOCK_LOCKED   = 0,
  PM_LOCK_UNLOCKED = 1,
} PmLockState;

/* Best-effort probe of whether the device is unlocked (keyguard dismissed),
 * via `adb shell dumpsys`. Blocking - runs an adb subprocess, so it MUST be
 * called off the UI thread. A NULL serial returns PM_LOCK_UNKNOWN. */
PmLockState pm_adb_query_lock_state (const char *serial);

/* Pair with a device over Wi-Fi (`adb pair host:port code`). Used once during
 * initial setup of Android 11+ wireless debugging. Blocking. */
gboolean pm_adb_pair (const char *host, guint16 port, const char *code, GError **error);

/* Establish a TCP/IP adb connection to host:port (`adb connect`). */
gboolean pm_adb_connect (const char *host, guint16 port, GError **error);

/* Return the serial of the first connected device, or NULL (caller frees). */
char *pm_adb_get_first_serial (GError **error);

/* Find a wireless adb endpoint already known by the adb server. This checks
 * connected devices first, then adb's mDNS service list. Blocking. */
gboolean pm_adb_find_wireless_device (PmDeviceInfo *out, GError **error);

/* Push a local file to the device (`adb -s SERIAL push`). */
gboolean pm_adb_push (const char *serial,
                      const char *local_path,
                      const char *remote_path,
                      GError    **error);

/* Set up `adb forward tcp:LOCAL localabstract:REMOTE`. On success the server's
 * sockets become reachable at 127.0.0.1:local_port. */
gboolean pm_adb_forward (const char *serial,
                         guint16     local_port,
                         const char *remote_socket_name,
                         GError    **error);

/* Tear down a forward previously created with pm_adb_forward(). */
gboolean pm_adb_forward_remove (const char *serial, guint16 local_port, GError **error);

/* Read the device's Wi-Fi MAC address (`adb shell` reading the wlan interface).
 * Returns an uppercased "AA:BB:CC:DD:EE:FF" string, or NULL if it cannot be
 * read. Blocking - runs adb subprocesses, so call off the UI thread. The MAC is
 * a stable per-network identifier even as the device's DHCP IP changes, so it is
 * used to key the saved lockscreen PIN. A NULL serial returns NULL. */
char *pm_adb_query_mac (const char *serial);

/* Best-effort attempt to dismiss the keyguard and enter `pin` on the device's
 * lockscreen: wake the panel, swipe the keyguard away, type the digits, press
 * Enter. Returns TRUE if the sequence ran without an adb error (NOT proof the
 * the device actually unlocked - OEM lockscreens vary). Skips and returns TRUE when
 * the device already reports unlocked. Blocking; call off the UI thread. A NULL
 * serial or empty pin is a no-op returning FALSE.
 *
 * When `out_unlocked` is non-NULL it is set to TRUE only if the keyguard was
 * confirmed dismissed (already unlocked, or the lock state was observed as
 * unlocked after an entry); FALSE means the PIN was tried but the phone stayed
 * locked - almost always a wrong saved PIN. Attempts are deliberately capped
 * below Android's wrong-PIN lockout so the caller can prompt for a correct PIN
 * instead of letting repeated tries time-lock (or wipe) the device. */
gboolean pm_adb_unlock_with_pin (const char *serial, const char *pin,
                                 gboolean *out_unlocked, GError **error);

/* One-shot form for a PIN entered interactively. Unlike the saved-PIN helper
 * above, this submits the PIN exactly once so each press of Unlock corresponds
 * to one Android lockscreen attempt. The result semantics are otherwise the
 * same. */
gboolean pm_adb_unlock_with_pin_once (const char *serial, const char *pin,
                                      gboolean *out_unlocked, GError **error);

/* Wake the device screen (`adb shell input keyevent KEYCODE_WAKEUP`).
 * Fire-and-forget: spawns the command and reaps it asynchronously, so it is
 * safe to call from the UI thread. KEYCODE_WAKEUP only turns the screen *on*;
 * it never sleeps an already-awake device, so it can be called liberally on
 * user input. A NULL serial is a no-op. */
void pm_adb_wake_screen (const char *serial);

/* Launch the pushed server process. This is long-running: the returned
 * GSubprocess stays alive for the duration of the mirror session. `args` are
 * the trailing arguments passed to `app_process` (resolution, bitrate, etc.).
 * Returns NULL on spawn failure. Caller owns the GSubprocess. */
GSubprocess *pm_adb_spawn_server (const char *serial,
                                  const char *remote_jar_path,
                                  const char *server_class,
                                  const char *const *args,
                                  GError    **error);

G_END_DECLS
