/* adb.c - implementation of the adb CLI wrapper. */
#include "adb.h"
#include <stdio.h>
#include <string.h>

/* Run `adb` with the given NULL-terminated argv tail (after "adb"), wait for
 * it, and optionally capture stdout. Returns FALSE on non-zero exit. */
static gboolean
adb_run (const char *const *argv_tail,
         char             **out_stdout,
         GError           **error)
{
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, (gpointer) "adb");
  for (const char *const *a = argv_tail; a && *a; a++)
    g_ptr_array_add (argv, (gpointer) *a);
  g_ptr_array_add (argv, NULL);

  GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDERR_PIPE;
  if (out_stdout)
    flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;

  g_autoptr (GSubprocess) proc =
    g_subprocess_newv ((const gchar * const *) argv->pdata, flags, error);
  if (proc == NULL)
    return FALSE;

  g_autofree char *out_buf = NULL;
  g_autofree char *err_buf = NULL;
  if (!g_subprocess_communicate_utf8 (proc, NULL, NULL,
                                      out_stdout ? &out_buf : NULL,
                                      &err_buf, error))
    return FALSE;

  if (!g_subprocess_get_successful (proc)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "adb failed: %s", err_buf ? g_strstrip (err_buf) : "(no stderr)");
    return FALSE;
  }

  if (out_stdout)
    *out_stdout = g_steal_pointer (&out_buf);
  return TRUE;
}

/* Parse a keyguard/lock verdict out of a `dumpsys` blob. Returns PM_LOCK_UNKNOWN
 * if no recognised field is present. The field names differ across Android
 * versions, so several are checked:
 *   - `mDreamingLockscreen=true|false` (WindowManager policy, most versions)
 *   - `mShowingLockscreen=true|false`  (some AOSP builds)
 *   - `mScreenState=ON_UNLOCKED|...`   (NFC service; clean lock+screen enum)
 * A "true" lockscreen / a state containing LOCKED (but not UNLOCKED) means
 * locked; the negatives mean unlocked. */
static PmLockState
parse_lock_state (const char *dump)
{
  if (dump == NULL)
    return PM_LOCK_UNKNOWN;

  if (strstr (dump, "mDreamingLockscreen=true") ||
      strstr (dump, "mShowingLockscreen=true"))
    return PM_LOCK_LOCKED;
  if (strstr (dump, "mDreamingLockscreen=false") ||
      strstr (dump, "mShowingLockscreen=false"))
    return PM_LOCK_UNLOCKED;

  /* NFC's mScreenState encodes both screen and lock, e.g. "ON_UNLOCKED". */
  const char *s = strstr (dump, "mScreenState=");
  if (s != NULL) {
    s += strlen ("mScreenState=");
    /* UNLOCKED check first: "UNLOCKED" also contains "LOCKED" as a substring. */
    if (strncmp (s, "ON_UNLOCKED", 11) == 0 || strncmp (s, "OFF_UNLOCKED", 12) == 0)
      return PM_LOCK_UNLOCKED;
    if (strncmp (s, "ON_LOCKED", 9) == 0 || strncmp (s, "OFF_LOCKED", 10) == 0)
      return PM_LOCK_LOCKED;
  }

  return PM_LOCK_UNKNOWN;
}

PmLockState
pm_adb_query_lock_state (const char *serial)
{
  if (serial == NULL)
    return PM_LOCK_UNKNOWN;

  /* `dumpsys window` carries the keyguard fields on most builds; fall back to
   * the NFC service's clean lock+screen enum when they are absent. */
  const char *const probes[][6] = {
    { "-s", serial, "shell", "dumpsys", "window", NULL },
    { "-s", serial, "shell", "dumpsys", "nfc",    NULL },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (probes); i++) {
    g_autofree char *out = NULL;
    g_autoptr (GError) error = NULL;
    if (!adb_run (probes[i], &out, &error)) {
      g_debug ("adb: lock-state probe failed: %s",
               error ? error->message : "(unknown)");
      continue;
    }
    PmLockState state = parse_lock_state (out);
    if (state != PM_LOCK_UNKNOWN)
      return state;
  }

  return PM_LOCK_UNKNOWN;
}

/* TRUE if `s` looks like a real, non-zero MAC ("AA:BB:CC:DD:EE:FF"). */
static gboolean
is_valid_mac (const char *s)
{
  if (s == NULL || strlen (s) != 17)
    return FALSE;
  gboolean all_zero = TRUE;
  for (int i = 0; i < 17; i++) {
    if ((i % 3) == 2) {
      if (s[i] != ':')
        return FALSE;
    } else {
      if (!g_ascii_isxdigit (s[i]))
        return FALSE;
      if (s[i] != '0')
        all_zero = FALSE;
    }
  }
  return !all_zero;   /* 00:00:.. means "unavailable / privacy MAC not assigned" */
}

/* Return an uppercased copy of the 17-char MAC starting at `p`, or NULL if it is
 * not a valid MAC. */
static char *
dup_mac_if_valid (const char *p)
{
  if (p == NULL)
    return NULL;
  g_autofree char *cand = g_strndup (p, 17);
  if (is_valid_mac (cand))
    return g_ascii_strup (cand, -1);
  return NULL;
}

/* Pull the wlan0 MAC out of `ip -o link` output. Lines look like:
 *   "45: wlan0: <...> ... link/ether aa:bb:cc:dd:ee:ff brd ff:ff:..."
 * Keys on the wlan0 interface specifically and ignores p2p / virtual ones
 * (e.g. "p2p-dev-wlan0") whose MACs differ, so the key stays stable. */
static char *
parse_mac_from_ip_link (const char *text)
{
  if (text == NULL)
    return NULL;
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  char *first_wlan = NULL;   /* fallback: any wlan* if no exact wlan0 line */

  for (guint i = 0; lines[i] != NULL; i++) {
    if (strstr (lines[i], "wlan") == NULL)
      continue;
    const char *ether = strstr (lines[i], "link/ether ");
    if (ether == NULL)
      continue;
    g_autofree char *mac = dup_mac_if_valid (ether + strlen ("link/ether "));
    if (mac == NULL)
      continue;

    /* The interface name sits between the leading "N: " and the next ":". */
    if (strstr (lines[i], " wlan0:") != NULL)
      return g_steal_pointer (&mac);   /* exact wlan0 wins immediately */
    if (first_wlan == NULL)
      first_wlan = g_steal_pointer (&mac);
  }
  return first_wlan;
}

/* Pull the device MAC out of `cmd wifi status`, which prints, among much else,
 * a field "MAC: aa:bb:cc:dd:ee:ff,". This is the canonical current Wi-Fi MAC and
 * works even when `ip` is restricted, so it is the most reliable source. The
 * BSSID field is also MAC-shaped, hence the anchor on the "MAC: " label. */
static char *
parse_mac_from_wifi_status (const char *text)
{
  if (text == NULL)
    return NULL;
  const char *p = text;
  while ((p = strstr (p, "MAC: ")) != NULL) {
    char *mac = dup_mac_if_valid (p + strlen ("MAC: "));
    if (mac != NULL)
      return mac;
    p += strlen ("MAC: ");
  }
  return NULL;
}

char *
pm_adb_query_mac (const char *serial)
{
  if (serial == NULL)
    return NULL;

  /* Source 1: `ip -o link`. NB: plain `ip -o link` - NOT `ip -o link show`,
   * which Android's toybox `ip` rejects (exit 1) on some OEM builds (e.g.
   * Samsung One UI). Works without special permissions on most devices. */
  {
    const char *argv[] = { "-s", serial, "shell", "ip", "-o", "link", NULL };
    g_autofree char *out = NULL;
    if (adb_run (argv, &out, NULL)) {
      char *mac = parse_mac_from_ip_link (out);
      if (mac != NULL)
        return mac;
    }
  }

  /* Source 2: `cmd wifi status` - the canonical current Wi-Fi MAC, robust when
   * `ip` is locked down. Requires Wi-Fi to be associated. */
  {
    const char *argv[] = { "-s", serial, "shell", "cmd", "wifi", "status", NULL };
    g_autofree char *out = NULL;
    if (adb_run (argv, &out, NULL)) {
      char *mac = parse_mac_from_wifi_status (out);
      if (mac != NULL)
        return mac;
    }
  }

  /* Source 3: the wlan0 sysfs address file. Often blocked by SELinux on recent
   * Android, but free and correct when readable. */
  {
    const char *argv[] = { "-s", serial, "shell",
                           "cat", "/sys/class/net/wlan0/address", NULL };
    g_autofree char *out = NULL;
    if (adb_run (argv, &out, NULL) && out != NULL) {
      g_strstrip (out);
      if (is_valid_mac (out))
        return g_ascii_strup (out, -1);
    }
  }

  return NULL;
}

/* Read the device screen size via `wm size`. Output: "Physical size: 1080x2400"
 * and possibly an "Override size:" line we prefer when present. Falls back to a
 * common portrait default so a swipe still lands somewhere sane. */
static void
query_screen_size (const char *serial, guint *out_w, guint *out_h)
{
  *out_w = 1080;
  *out_h = 2160;

  const char *argv[] = { "-s", serial, "shell", "wm", "size", NULL };
  g_autofree char *out = NULL;
  if (!adb_run (argv, &out, NULL) || out == NULL)
    return;

  /* Override size wins when the user has forced a resolution. */
  const char *p = strstr (out, "Override size:");
  if (p == NULL)
    p = strstr (out, "Physical size:");
  if (p == NULL)
    p = strchr (out, ':');
  else
    p = strchr (p, ':');
  if (p == NULL)
    return;

  guint w = 0, h = 0;
  if (sscanf (p + 1, " %ux%u", &w, &h) == 2 && w > 0 && h > 0) {
    *out_w = w;
    *out_h = h;
  }
}

/* Run one `adb -s serial shell input <args...>` command, returning FALSE with
 * `error` set on failure. */
static gboolean
adb_input (const char *serial, const char *const *input_args, GError **error)
{
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, (gpointer) "-s");
  g_ptr_array_add (argv, (gpointer) serial);
  g_ptr_array_add (argv, (gpointer) "shell");
  g_ptr_array_add (argv, (gpointer) "input");
  for (const char *const *a = input_args; a && *a; a++)
    g_ptr_array_add (argv, (gpointer) *a);
  g_ptr_array_add (argv, NULL);
  return adb_run ((const char *const *) argv->pdata, NULL, error);
}

/* Per-attempt settle (ms) between the keyguard swipe and typing the PIN. Short
 * first so a snappy phone is unlocked almost instantly; growing so a sluggish
 * keyguard (older or heavy-skin devices) still gets enough time on a later pass.
 * The array length is also the attempt cap: kept at 3 so a wrong saved PIN is
 * tried a few times (covering swipes that land before the bouncer is ready) but
 * never enough to trip Android's wrong-PIN lockout, which typically begins at
 * the 5th failure with an escalating time-lock (and, on some policies, a wipe).
 * The caller surfaces the failure and asks the user for the correct PIN. */
static const guint k_unlock_settle_ms[] = { 250, 450, 700 };

/* After typing, poll the lock state up to this many times at this interval,
 * stopping the moment the PIN is accepted. This confirms the unlock *before* the
 * loop could start another attempt, so a successful entry never spills a swipe or
 * keystroke onto the now-unlocked home screen. The window (count x interval) is
 * generous enough that a correct PIN is virtually always observed as unlocked
 * here rather than racing a retry. */
#define PM_UNLOCK_VERIFY_POLLS         8
#define PM_UNLOCK_VERIFY_INTERVAL_US   (120 * 1000)

/* Wipe any digits already in the focused PIN field by sending that many
 * KEYCODE_DEL backspaces in a single `input keyevent` call. Harmless when the
 * field is empty or not yet focused, so each unlock attempt can retype the full
 * PIN from scratch rather than appending to a partial entry. */
static gboolean
keyguard_clear_field (const char *serial, gsize count, GError **error)
{
  g_autoptr (GPtrArray) args = g_ptr_array_new ();
  g_ptr_array_add (args, (gpointer) "keyevent");
  for (gsize i = 0; i < count; i++)
    g_ptr_array_add (args, (gpointer) "KEYCODE_DEL");
  g_ptr_array_add (args, NULL);
  return adb_input (serial, (const char *const *) args->pdata, error);
}

static gboolean
unlock_with_pin_attempts (const char *serial,
                          const char *pin,
                          gsize       max_attempts,
                          gboolean   *out_unlocked,
                          GError    **error)
{
  if (out_unlocked != NULL)
    *out_unlocked = FALSE;

  if (serial == NULL || pin == NULL || *pin == '\0') {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "missing serial or PIN");
    return FALSE;
  }

  /* Don't touch an already-unlocked phone: typing a PIN into the home screen
   * would leak the digits into whatever has focus. UNKNOWN falls through and the
   * attempt proceeds anyway (the keyguard swipe + text is harmless if already
   * unlocked). */
  if (pm_adb_query_lock_state (serial) == PM_LOCK_UNLOCKED) {
    if (out_unlocked != NULL)
      *out_unlocked = TRUE;
    return TRUE;
  }

  /* Wake the panel so the keyguard is interactive. No fixed settle here: if the
   * first swipe lands before the screen is up it is simply a no-op and the retry
   * loop below re-swipes once the device has woken. */
  {
    const char *wake[] = { "keyevent", "KEYCODE_WAKEUP", NULL };
    if (!adb_input (serial, wake, error))
      return FALSE;
  }

  /* Swipe-up coordinates, scaled to the real screen so the gesture lands across
   * resolutions. */
  guint w = 0, h = 0;
  query_screen_size (serial, &w, &h);
  g_autofree char *x  = g_strdup_printf ("%u", w / 2);
  g_autofree char *y1 = g_strdup_printf ("%u", (guint) (h * 0.75));
  g_autofree char *y2 = g_strdup_printf ("%u", (guint) (h * 0.25));
  const gsize clear_count = MIN (strlen (pin) + 2, (gsize) 24);

  /* Adaptive unlock instead of one long blind wait sized for the slowest phone:
   * try with a short settle and only grow it if the phone is still locked. After
   * each entry we poll until the unlock registers and return the instant it does,
   * so a fast device pays only one short settle and - crucially - a successful
   * entry never spills the next attempt's swipe/keystrokes onto the home screen.
   * Clearing the field first makes every retype a fresh, full entry; a swipe/type
   * that lands before the bouncer is ready simply enters nothing and retries. */
  for (gsize attempt = 0;
       attempt < MIN (max_attempts, G_N_ELEMENTS (k_unlock_settle_ms));
       attempt++) {
    /* Gate every retry against the lock state: once the phone is in, a swipe-up
     * would open the launcher's app drawer and the text would land in whatever
     * has focus. (Attempt 0 is already known locked from the entry check.) */
    if (attempt > 0 && pm_adb_query_lock_state (serial) == PM_LOCK_UNLOCKED) {
      if (out_unlocked != NULL)
        *out_unlocked = TRUE;
      return TRUE;
    }

    const char *swipe[] = { "swipe", x, y1, x, y2, "150", NULL };
    if (!adb_input (serial, swipe, error))
      return FALSE;

    g_usleep (k_unlock_settle_ms[attempt] * 1000);

    /* The settle may have spanned a prior entry's keyguard dismissal; re-check so
     * the type below never reaches the home screen. */
    if (attempt > 0 && pm_adb_query_lock_state (serial) == PM_LOCK_UNLOCKED) {
      if (out_unlocked != NULL)
        *out_unlocked = TRUE;
      return TRUE;
    }

    if (!keyguard_clear_field (serial, clear_count, error))
      return FALSE;
    {
      const char *text[] = { "text", pin, NULL };
      if (!adb_input (serial, text, error))
        return FALSE;
    }
    {
      /* ENTER confirms on lockscreens that don't auto-submit; fixed-length PIN
       * pads usually submit on the last digit and ignore this. */
      const char *enter[] = { "keyevent", "KEYCODE_ENTER", NULL };
      if (!adb_input (serial, enter, error))
        return FALSE;
    }

    /* Wait for the unlock to actually register before considering a retry, so a
     * correct PIN is seen here and never races into another attempt. */
    for (gsize poll = 0; poll < PM_UNLOCK_VERIFY_POLLS; poll++) {
      g_usleep (PM_UNLOCK_VERIFY_INTERVAL_US);
      if (pm_adb_query_lock_state (serial) == PM_LOCK_UNLOCKED) {
        if (out_unlocked != NULL)
          *out_unlocked = TRUE;
        return TRUE;
      }
    }
  }

  /* Sequence ran cleanly (no adb error) but the phone never reported unlocked
   * across every capped attempt: the saved PIN is almost certainly wrong. Leave
   * `out_unlocked` FALSE so the caller can prompt for the correct one instead of
   * retrying into a lockout. */
  return TRUE;
}

gboolean
pm_adb_unlock_with_pin (const char *serial, const char *pin,
                        gboolean *out_unlocked, GError **error)
{
  return unlock_with_pin_attempts (serial, pin,
                                   G_N_ELEMENTS (k_unlock_settle_ms),
                                   out_unlocked, error);
}

gboolean
pm_adb_unlock_with_pin_once (const char *serial, const char *pin,
                             gboolean *out_unlocked, GError **error)
{
  return unlock_with_pin_attempts (serial, pin, 1, out_unlocked, error);
}

gboolean
pm_adb_pair (const char *host, guint16 port, const char *code, GError **error)
{
  g_autofree char *endpoint = g_strdup_printf ("%s:%u", host, port);
  const char *argv[] = { "pair", endpoint, code, NULL };
  g_autofree char *out = NULL;
  if (!adb_run (argv, &out, error))
    return FALSE;

  if (out && !strstr (out, "Successfully paired")) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "pairing failed: %s", g_strstrip (out));
    return FALSE;
  }
  return TRUE;
}

gboolean
pm_adb_connect (const char *host, guint16 port, GError **error)
{
  g_autofree char *endpoint = g_strdup_printf ("%s:%u", host, port);
  const char *argv[] = { "connect", endpoint, NULL };
  g_autofree char *out = NULL;
  if (!adb_run (argv, &out, error))
    return FALSE;

  /* `adb connect` exits 0 even on failure; inspect the message. */
  if (out && (strstr (out, "failed") || strstr (out, "cannot") || strstr (out, "refused"))) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                 "adb connect: %s", g_strstrip (out));
    return FALSE;
  }
  return TRUE;
}

static gboolean
parse_wireless_endpoint (const char *endpoint,
                         char      **out_host,
                         guint16    *out_port)
{
  if (endpoint == NULL)
    return FALSE;

  g_autofree char *trimmed = g_strdup (endpoint);
  g_strstrip (trimmed);

  const char *colon = strrchr (trimmed, ':');
  if (colon == NULL || colon == trimmed || colon[1] == '\0')
    return FALSE;

  guint64 port = g_ascii_strtoull (colon + 1, NULL, 10);
  if (port == 0 || port > G_MAXUINT16)
    return FALSE;

  *out_host = g_strndup (trimmed, colon - trimmed);
  *out_port = (guint16) port;
  return TRUE;
}

char *
pm_adb_get_first_serial (GError **error)
{
  const char *argv[] = { "devices", NULL };
  g_autofree char *out = NULL;
  if (!adb_run (argv, &out, error))
    return NULL;

  /* Output: "List of devices attached\nSERIAL\tdevice\n..." */
  g_auto (GStrv) lines = g_strsplit (out, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    g_autofree char *line = g_strdup (g_strstrip (lines[i]));
    if (*line == '\0')
      continue;
    g_auto (GStrv) cols = g_strsplit_set (line, " \t", 2);
    if (cols[0] && cols[1] && g_str_has_prefix (g_strstrip (cols[1]), "device"))
      return g_strdup (cols[0]);
  }

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "no authorised device connected");
  return NULL;
}

static gboolean
find_wireless_from_devices (PmDeviceInfo *out, GError **error)
{
  const char *argv[] = { "devices", "-l", NULL };
  g_autofree char *text = NULL;
  if (!adb_run (argv, &text, error))
    return FALSE;

  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    g_autofree char *line = g_strdup (g_strstrip (lines[i]));
    const char *serial = NULL;
    const char *state = NULL;

    if (*line == '\0')
      continue;

    g_auto (GStrv) cols = g_strsplit_set (line, " \t", 0);
    for (guint c = 0; cols[c] != NULL; c++) {
      if (*cols[c] == '\0')
        continue;
      if (serial == NULL)
        serial = cols[c];
      else if (state == NULL) {
        state = cols[c];
        break;
      }
    }

    if (serial == NULL || g_strcmp0 (state, "device") != 0)
      continue;

    char *host = NULL;
    guint16 port = 0;
    if (!parse_wireless_endpoint (serial, &host, &port))
      continue;

    out->host = host;
    out->port = port;
    return TRUE;
  }

  return FALSE;
}

static gboolean
find_wireless_from_mdns (PmDeviceInfo *out, GError **error)
{
  const char *argv[] = { "mdns", "services", NULL };
  g_autofree char *text = NULL;
  if (!adb_run (argv, &text, error))
    return FALSE;

  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++) {
    g_autofree char *line = g_strdup (g_strstrip (lines[i]));
    if (*line == '\0')
      continue;
    if (strstr (line, "_adb-tls-connect._tcp") == NULL &&
        strstr (line, "_adb._tcp") == NULL)
      continue;

    g_auto (GStrv) cols = g_strsplit_set (line, " \t", 0);
    const char *service_name = NULL;
    for (guint c = 0; cols[c] != NULL; c++) {
      if (*cols[c] == '\0')
        continue;
      if (service_name == NULL)
        service_name = cols[c];

      char *host = NULL;
      guint16 port = 0;
      if (!parse_wireless_endpoint (cols[c], &host, &port))
        continue;

      out->name = g_strdup (service_name);
      out->host = host;
      out->port = port;
      return TRUE;
    }
  }

  return FALSE;
}

gboolean
pm_adb_find_wireless_device (PmDeviceInfo *out, GError **error)
{
  g_return_val_if_fail (out != NULL, FALSE);

  pm_device_info_clear (out);

  g_autoptr (GError) devices_error = NULL;
  if (find_wireless_from_devices (out, &devices_error))
    return TRUE;

  g_autoptr (GError) mdns_error = NULL;
  if (find_wireless_from_mdns (out, &mdns_error))
    return TRUE;

  if (error != NULL) {
    if (devices_error != NULL)
      g_propagate_error (error, g_steal_pointer (&devices_error));
    else if (mdns_error != NULL)
      g_propagate_error (error, g_steal_pointer (&mdns_error));
    else
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "no wireless adb device found");
  }

  return FALSE;
}

gboolean
pm_adb_push (const char *serial,
             const char *local_path,
             const char *remote_path,
             GError    **error)
{
  const char *argv[] = { "-s", serial, "push", local_path, remote_path, NULL };
  return adb_run (argv, NULL, error);
}

gboolean
pm_adb_forward (const char *serial,
                guint16     local_port,
                const char *remote_socket_name,
                GError    **error)
{
  g_autofree char *local  = g_strdup_printf ("tcp:%u", local_port);
  g_autofree char *remote = g_strdup_printf ("localabstract:%s", remote_socket_name);
  const char *argv[] = { "-s", serial, "forward", local, remote, NULL };
  return adb_run (argv, NULL, error);
}

gboolean
pm_adb_forward_remove (const char *serial, guint16 local_port, GError **error)
{
  g_autofree char *local = g_strdup_printf ("tcp:%u", local_port);
  const char *argv[] = { "-s", serial, "forward", "--remove", local, NULL };
  return adb_run (argv, NULL, error);
}

static void
wake_reaped (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GSubprocess *proc = G_SUBPROCESS (source);
  g_subprocess_wait_finish (proc, res, NULL);
  g_object_unref (proc);
}

void
pm_adb_wake_screen (const char *serial)
{
  if (serial == NULL)
    return;

  const char *argv[] = {
    "adb", "-s", serial, "shell", "input", "keyevent", "KEYCODE_WAKEUP", NULL
  };
  g_autoptr (GError) error = NULL;
  GSubprocess *proc =
    g_subprocess_newv ((const gchar * const *) argv,
                       G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                       G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                       &error);
  if (proc == NULL) {
    g_warning ("adb: failed to wake screen: %s", error->message);
    return;
  }

  /* Reap asynchronously so the (short-lived) process is never left a zombie
   * and the caller is never blocked. */
  g_subprocess_wait_async (proc, NULL, wake_reaped, NULL);
}

GSubprocess *
pm_adb_spawn_server (const char *serial,
                     const char *remote_jar_path,
                     const char *server_class,
                     const char *const *args,
                     GError    **error)
{
  /* Equivalent of:
   *   adb -s SERIAL shell CLASSPATH=/data/local/tmp/server.jar \
   *       app_process / com.example.Server <args...>
   */
  g_autofree char *classpath = g_strdup_printf ("CLASSPATH=%s", remote_jar_path);

  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, (gpointer) "adb");
  g_ptr_array_add (argv, (gpointer) "-s");
  g_ptr_array_add (argv, (gpointer) serial);
  g_ptr_array_add (argv, (gpointer) "shell");
  g_ptr_array_add (argv, (gpointer) classpath);
  g_ptr_array_add (argv, (gpointer) "app_process");
  g_ptr_array_add (argv, (gpointer) "/");
  g_ptr_array_add (argv, (gpointer) server_class);
  for (const char *const *a = args; a && *a; a++)
    g_ptr_array_add (argv, (gpointer) *a);
  g_ptr_array_add (argv, NULL);

  return g_subprocess_newv ((const gchar * const *) argv->pdata,
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_PIPE,
                            error);
}
