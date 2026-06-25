/* pm-session.c - pipeline controller. */
#include "pm-session.h"
#include "device.h"
#include "discovery.h"
#include "adb.h"
#include "net.h"
#include "decoder.h"
#include "audio.h"
#include "input.h"
#include "protocol.h"
#include "failsafe.h"
#include "pinstore.h"

#include <string.h>
#include <glib/gi18n.h>

/* Local TCP port for the forwarded device socket. */
#define PM_LOCAL_PORT     27183
#define PM_REMOTE_SOCKET  "scrcpy"
#define PM_REMOTE_JAR     "/data/local/tmp/scrcpy-server.jar"
#define PM_SERVER_CLASS   "com.genymobile.scrcpy.Server"
#define PM_DISCOVERY_TIMEOUT_MS 8000
#define PM_SERVER_SOCKET_ATTEMPTS 100
#define PM_SERVER_SOCKET_DELAY_MS 100
/* Grace window for the server to read the teardown lock (the KEYCODE_SLEEP press
 * in finalize_screen, plus any panel restore) off the control socket before it
 * is force-killed on a clean disconnect, so the phone actually locks. */
#define PM_LOCK_DRAIN_US (300 * 1000)
/* Cadence of the in-stream keyguard probe that drives the floating Unlock button
 * and the battery-saver blank gate. One adb dumpsys per cycle, so kept modest. */
#define PM_LOCK_POLL_US (2 * G_USEC_PER_SEC)
#define PM_DEVICE_NAME_FIELD_LENGTH 64
#define PM_STREAM_PACKET_FLAG_SESSION 0x80000000u

/* The scrcpy server captures audio as raw 16-bit PCM, 48 kHz, stereo (its fixed
 * AudioConfig). Requesting audio_codec=raw makes it arrive undelimited (no frame
 * meta) so it can go straight to the sink. The 4-byte audio header is the codec
 * id; two reserved values stand in for "no audio" instead of a codec. */
#define PM_AUDIO_CODEC_RAW      0x00726177u  /* 'r','a','w'  */
#define PM_AUDIO_DISABLED       0x00000000u  /* device can't capture; video only */
#define PM_AUDIO_CONFIG_ERROR   0x00000001u  /* server hit a fatal audio error    */
#define PM_AUDIO_SAMPLE_RATE    48000u
#define PM_AUDIO_CHANNELS       2

struct _PmSession {
  GObject       parent_instance;

  PmState       state;
  PmVideoView  *view;          /* borrowed */
  PmStreamInfo  stream;

  /* discovery */
  PmDiscovery  *discovery;

  /* live pipeline */
  GThread      *worker;
  gint          stop;          /* atomic */
  PmNet        *video_net;
  PmNet        *audio_net;
  PmNet        *control_net;
  GSubprocess  *server;
  PmDecoder    *decoder;
  GThread      *audio_worker;  /* reads audio_net -> PmAudio; child of worker  */
  GThread      *unlock_worker; /* drives auto-unlock concurrently; child of worker */
  PmAudio      *audio;         /* owned by audio_worker once playback starts   */
  PmInput      *input;
  PmDeviceInfo  target;
  gboolean      silent;
  gboolean      mouse_mode;   /* TRUE: forward pointer as mouse, else touch */
  gboolean      audio_enabled;/* TRUE: stream phone audio to the desktop sink */
  guint         video_bitrate;/* requested video bitrate in Mbps (0 = scrcpy default) */
  PmDisplayMode display_mode; /* mirror the phone screen vs. a virtual display */
  guint         display_width;  /* virtual display geometry (PM_DISPLAY_VIRTUAL) */
  guint         display_height;
  guint         display_dpi;    /* 0 = let the device pick its default density   */
  gboolean      screen_off;   /* TRUE: blank the phone panel while mirroring   */
  gint          wake_suppressed; /* atomic; non-zero while the panel is blanked */
  gint          screen_blanked;  /* atomic; TRUE between the power-off and restore */
  gint          screen_finalized;/* atomic; TRUE once teardown has put the phone to sleep */
  gint          reconnecting;    /* atomic; TRUE while a reconnect tears the pipeline down */
  guint         inhibit_cookie;  /* session-manager sleep inhibitor, 0 when not held */

  /* demo pipeline */
  guint         demo_timer;
};

static void begin_connect (PmSession *self);

static gboolean
demo_mode_enabled (void)
{
  const char *demo = g_getenv ("PM_DEMO");
  return demo != NULL && *demo != '\0' && g_strcmp0 (demo, "0") != 0;
}

static char *
find_server_jar (void)
{
  const char *env = g_getenv ("PM_SERVER_JAR");
  if (env != NULL && *env != '\0')
    return g_strdup (env);

  env = g_getenv ("SCRCPY_SERVER_PATH");
  if (env != NULL && *env != '\0')
    return g_strdup (env);

  static const char *paths[] = {
    "/usr/share/scrcpy/scrcpy-server",
    "/usr/local/share/scrcpy/scrcpy-server",
    "/usr/share/scrcpy/scrcpy-server.jar",
    "/usr/local/share/scrcpy/scrcpy-server.jar",
    NULL,
  };

  for (int i = 0; paths[i] != NULL; i++) {
    if (g_file_test (paths[i], G_FILE_TEST_IS_REGULAR))
      return g_strdup (paths[i]);
  }

  return NULL;
}

static char *
find_scrcpy_version (void)
{
  const char *env = g_getenv ("PM_SCRCPY_VERSION");
  if (env != NULL && *env != '\0')
    return g_strdup (env);

  const char *argv[] = { "scrcpy", "--version", NULL };
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) proc =
    g_subprocess_newv ((const gchar * const *) argv,
                       G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                       G_SUBPROCESS_FLAGS_STDERR_PIPE,
                       &error);
  if (proc == NULL)
    return NULL;

  g_autofree char *out = NULL;
  if (!g_subprocess_communicate_utf8 (proc, NULL, NULL, &out, NULL, NULL))
    return NULL;
  if (!g_subprocess_get_successful (proc))
    return NULL;

  g_auto (GStrv) tokens = g_strsplit_set (out ? out : "", " \t\r\n", 0);
  if (tokens[0] != NULL && g_strcmp0 (tokens[0], "scrcpy") == 0 &&
      tokens[1] != NULL && *tokens[1] != '\0')
    return g_strdup (tokens[1]);

  return NULL;
}

enum { SIG_STATE_CHANGED, SIG_STREAM_CHANGED, SIG_STARTUP_CHECK_FAILED,
       SIG_PIN_REQUIRED, SIG_UNLOCKING, SIG_UNLOCK_FAILED, SIG_LOCKED_CHANGED,
       N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (PmSession, pm_session, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------- */
/* state plumbing                                                            */
/* ------------------------------------------------------------------------- */

/* Keep the desktop awake while mirroring. The user drives the phone, not the
 * desktop's keyboard/mouse, so on its idle timeout the session manager would
 * otherwise suspend the machine or blank/lock the screen and kill the stream.
 *
 * The inhibitor is held by the session manager (logind/gnome-session) over
 * D-Bus, keyed to the application's connection, which provides the fail-safe
 * the device-side undo can't: if the process exits for ANY reason (clean quit,
 * window close, or a crash, including a SIGKILL no signal handler can catch),
 * the connection drops and the session manager releases the inhibitor.
 * The explicit uninhibit in pm_session_set_state() handles the in-process cases
 * (stop, a spontaneous disconnect, a reconnect's transient teardown), so the
 * desktop can sleep again the moment mirroring ends. */
static void
pm_session_inhibit_sleep (PmSession *self)
{
  if (self->inhibit_cookie != 0)
    return;   /* already held */

  GApplication *app = g_application_get_default ();
  if (app == NULL || !GTK_IS_APPLICATION (app))
    return;

  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
  self->inhibit_cookie =
    gtk_application_inhibit (GTK_APPLICATION (app), window,
                            GTK_APPLICATION_INHIBIT_SUSPEND |
                            GTK_APPLICATION_INHIBIT_IDLE,
                            _("Mirroring a connected phone"));
  if (self->inhibit_cookie == 0)
    g_debug ("sleep inhibit: session manager refused; desktop may suspend mid-mirror");
}

static void
pm_session_uninhibit_sleep (PmSession *self)
{
  if (self->inhibit_cookie == 0)
    return;

  GApplication *app = g_application_get_default ();
  if (app != NULL && GTK_IS_APPLICATION (app))
    gtk_application_uninhibit (GTK_APPLICATION (app), self->inhibit_cookie);
  self->inhibit_cookie = 0;
}

static void
pm_session_set_state (PmSession *self, PmState state, const char *message)
{
  self->state = state;

  /* Hold the desktop awake only while actively mirroring; release it the moment
   * that state is left - a clean stop, a spontaneous disconnect, an error, or a
   * reconnect's transient CONNECTING all funnel through here. An abnormal exit
   * (crash/kill) is covered by the session manager auto-releasing the inhibitor
   * when the process dies. */
  if (state == PM_STATE_MIRRORING)
    pm_session_inhibit_sleep (self);
  else
    pm_session_uninhibit_sleep (self);

  g_signal_emit (self, signals[SIG_STATE_CHANGED], 0, (guint) state, message);

  if (state == PM_STATE_MIRRORING && self->input == NULL && self->view != NULL) {
    /* Wire input after the device geometry is known. The control net may be
     * NULL in demo mode; PmInput simply no-ops its sends in that case. The
     * serial (host:port, as used for every adb command in live_worker) lets
     * input wake a genuinely asleep/dozing screen; NULL in demo mode disables
     * that. Wake stays available even with screen-off enabled - the device can
     * doze (OS timeout, power button) and must be wakeable - and is held off
     * only while the panel has been *deliberately* blanked, via wake_suppressed
     * (set on the worker thread when the blank is sent). */
    g_autofree char *wake_serial =
      self->target.host != NULL
        ? g_strdup_printf ("%s:%u", self->target.host, self->target.port)
        : NULL;
    self->input = pm_input_new (self->control_net, &self->stream, wake_serial,
                                &self->wake_suppressed);
    pm_input_set_mouse_mode (self->input, self->mouse_mode);
    pm_input_attach (self->input, self->view);
  }
}

void
pm_session_set_mouse_mode (PmSession *self, gboolean mouse_mode)
{
  g_return_if_fail (PM_IS_SESSION (self));
  self->mouse_mode = mouse_mode;
  if (self->input != NULL)
    pm_input_set_mouse_mode (self->input, mouse_mode);
}

void
pm_session_set_audio_enabled (PmSession *self, gboolean audio_enabled)
{
  g_return_if_fail (PM_IS_SESSION (self));
  /* Audio is negotiated with the server when the pipeline starts, so this only
   * affects the next connection; a live session keeps its current audio state. */
  self->audio_enabled = audio_enabled;
}

void
pm_session_set_video_bitrate (PmSession *self, guint mbps)
{
  g_return_if_fail (PM_IS_SESSION (self));
  /* The bitrate is handed to the on-device encoder when the server launches, so
   * this only affects the next connection; reconnect to apply it to a live one. */
  self->video_bitrate = mbps;
}

void
pm_session_set_display_mode (PmSession *self, PmDisplayMode mode)
{
  g_return_if_fail (PM_IS_SESSION (self));
  /* The display mode is chosen when the on-device server is launched, so this
   * only affects the next connection; reconnect to apply it to a live session. */
  self->display_mode = mode;
}

void
pm_session_set_virtual_display (PmSession *self, guint width, guint height, guint dpi)
{
  g_return_if_fail (PM_IS_SESSION (self));
  self->display_width = width;
  self->display_height = height;
  self->display_dpi = dpi;
}

void
pm_session_set_screen_off (PmSession *self, gboolean screen_off)
{
  g_return_if_fail (PM_IS_SESSION (self));
  /* The phone screen is blanked once the control socket is up, so this only
   * affects the next connection; reconnect to apply it to a live session. */
  self->screen_off = screen_off;
}

static void
pm_session_update_stream_size (PmSession *self, guint32 width, guint32 height)
{
  if (width == 0 || height == 0)
    return;
  if (self->stream.width == width && self->stream.height == height)
    return;

  self->stream.width = width;
  self->stream.height = height;

  if (self->input != NULL)
    pm_input_set_stream (self->input, &self->stream);

  g_message ("Video stream changed size: %ux%u", width, height);
  g_signal_emit (self, signals[SIG_STREAM_CHANGED], 0, width, height);
}

/* Marshal a state change from the worker thread to the main thread. */
typedef struct { PmSession *self; PmState state; char *message; } StateMsg;

static gboolean
apply_state_idle (gpointer data)
{
  StateMsg *m = data;
  if (!g_atomic_int_get (&m->self->stop) || m->state == PM_STATE_IDLE || m->state == PM_STATE_ERROR)
    pm_session_set_state (m->self, m->state, m->message);
  return G_SOURCE_REMOVE;
}

static void
state_msg_free (gpointer data)
{
  StateMsg *m = data;
  g_object_unref (m->self);
  g_free (m->message);
  g_free (m);
}

static void
post_state (PmSession *self, PmState state, const char *message)
{
  StateMsg *m = g_new0 (StateMsg, 1);
  m->self = g_object_ref (self);
  m->state = state;
  m->message = g_strdup (message);
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                              apply_state_idle, m, state_msg_free);
}

/* Marshal an "unlocking" edge (TRUE = started, FALSE = finished) from a worker
 * thread to the main thread, where the UI shows/hides the unlock alert. */
typedef struct { PmSession *self; gboolean active; } UnlockMsg;

static gboolean
emit_unlocking_idle (gpointer data)
{
  UnlockMsg *m = data;
  g_signal_emit (m->self, signals[SIG_UNLOCKING], 0, m->active);
  return G_SOURCE_REMOVE;
}

static void
unlock_msg_free (gpointer data)
{
  UnlockMsg *m = data;
  g_object_unref (m->self);
  g_free (m);
}

static void
post_unlocking (PmSession *self, gboolean active)
{
  UnlockMsg *m = g_new0 (UnlockMsg, 1);
  m->self = g_object_ref (self);
  m->active = active;
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                              emit_unlocking_idle, m, unlock_msg_free);
}

/* Marshal an "auto-unlock exhausted its attempts and the phone is still locked"
 * edge from the unlock worker to the main thread, where the window alerts the
 * user that the saved PIN looks wrong and asks for a correct one. Reuses
 * UnlockMsg (its `active` field is unused here). */
static gboolean
emit_unlock_failed_idle (gpointer data)
{
  UnlockMsg *m = data;
  g_signal_emit (m->self, signals[SIG_UNLOCK_FAILED], 0);
  return G_SOURCE_REMOVE;
}

static void
post_unlock_failed (PmSession *self)
{
  UnlockMsg *m = g_new0 (UnlockMsg, 1);
  m->self = g_object_ref (self);
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                              emit_unlock_failed_idle, m, unlock_msg_free);
}

/* Tell the UI that this locked phone has no saved PIN. The PIN prompt and its
 * unlock attempt are intentionally owned by the window: unlike auto-unlock,
 * the entered value must remain ephemeral and never pass through PmPinStore. */
static gboolean
emit_pin_required_idle (gpointer data)
{
  UnlockMsg *m = data;
  g_signal_emit (m->self, signals[SIG_PIN_REQUIRED], 0);
  return G_SOURCE_REMOVE;
}

static void
post_pin_required (PmSession *self)
{
  UnlockMsg *m = g_new0 (UnlockMsg, 1);
  m->self = g_object_ref (self);
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                              emit_pin_required_idle, m, unlock_msg_free);
}

/* Marshal a keyguard lock/unlock edge (TRUE = the phone is now on its
 * lockscreen) from the stream worker to the main thread, where the window shows
 * or hides the floating Unlock button. Reuses UnlockMsg's `active` field. */
static gboolean
emit_locked_changed_idle (gpointer data)
{
  UnlockMsg *m = data;
  g_signal_emit (m->self, signals[SIG_LOCKED_CHANGED], 0, m->active);
  return G_SOURCE_REMOVE;
}

static void
post_locked (PmSession *self, gboolean locked)
{
  UnlockMsg *m = g_new0 (UnlockMsg, 1);
  m->self = g_object_ref (self);
  m->active = locked;
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                              emit_locked_changed_idle, m, unlock_msg_free);
}

/* Decoder delivers textures on the main thread; push them to the view. */
static void
on_decoder_frame (GdkTexture *texture, gpointer user_data)
{
  PmSession *self = user_data;
  pm_session_update_stream_size (self,
                                 gdk_texture_get_width (texture),
                                 gdk_texture_get_height (texture));
  if (self->view != NULL)
    pm_video_view_set_texture (self->view, texture);
}

/* ------------------------------------------------------------------------- */
/* live pipeline                                                             */
/* ------------------------------------------------------------------------- */

static gboolean
connect_server_socket (GSubprocess  *server,
                       gboolean      expect_dummy,
                       PmNet       **out,
                       GError      **error)
{
  g_autoptr (GError) last_error = NULL;

  for (int i = 0; i < PM_SERVER_SOCKET_ATTEMPTS; i++) {
    if (g_subprocess_get_if_exited (server)) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("scrcpy-server exited with status %d"),
                   g_subprocess_get_exit_status (server));
      return FALSE;
    }

    g_autoptr (GError) attempt_error = NULL;
    PmNet *net = pm_net_new ();
    gboolean ok = pm_net_connect (net, "127.0.0.1", PM_LOCAL_PORT, &attempt_error);
    if (ok && expect_dummy) {
      guint8 dummy = 0;
      ok = pm_net_read_exact (net, &dummy, 1, &attempt_error);
    }

    if (ok) {
      *out = net;
      return TRUE;
    }

    pm_net_free (net);
    if (attempt_error != NULL) {
      g_clear_error (&last_error);
      last_error = g_steal_pointer (&attempt_error);
    }

    g_usleep (PM_SERVER_SOCKET_DELAY_MS * 1000);
  }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
               _("timed out waiting for scrcpy-server socket%s%s"),
               last_error ? ": " : "",
               last_error ? last_error->message : "");
  return FALSE;
}

/* Read the preamble the scrcpy 4.x server sends before the elementary stream:
 * dummy byte (already consumed by connect_server_socket), device name(64),
 * codec_id(4), then session_meta(flags(4) | width(4) | height(4)).
 *
 * Older notes in this project treated width/height as part of the codec header.
 * With scrcpy 4.x that misreads the session flag 0x80000000 as the width, which
 * makes Android reject every positional control event. */
static gboolean
read_stream_meta (PmSession *self, char **out_device_name, GError **error)
{
  guint8 device_name[PM_DEVICE_NAME_FIELD_LENGTH];
  if (!pm_net_read_exact (self->video_net, device_name, sizeof device_name, error))
    return FALSE;
  device_name[PM_DEVICE_NAME_FIELD_LENGTH - 1] = '\0';
  g_message ("Connected to Android device: %s", (char *) device_name);
  if (out_device_name != NULL && device_name[0] != '\0')
    *out_device_name = g_strdup ((char *) device_name);

  guint8 codec_buf[4];
  if (!pm_net_read_exact (self->video_net, codec_buf, sizeof codec_buf, error))
    return FALSE;

  guint32 codec_be = 0;
  memcpy (&codec_be, codec_buf, sizeof codec_be);
  guint32 codec_id = GUINT32_FROM_BE (codec_be);

  guint8 session_buf[12];
  if (!pm_net_read_exact (self->video_net, session_buf, sizeof session_buf, error))
    return FALSE;

  guint32 session_be[3];
  memcpy (session_be, session_buf, sizeof session_be);
  guint32 flags = GUINT32_FROM_BE (session_be[0]);
  self->stream.width  = GUINT32_FROM_BE (session_be[1]);
  self->stream.height = GUINT32_FROM_BE (session_be[2]);

  if ((flags & PM_STREAM_PACKET_FLAG_SESSION) == 0 ||
      self->stream.width == 0 || self->stream.height == 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "invalid scrcpy stream metadata flags=0x%08x size=%ux%u",
                 flags, self->stream.width, self->stream.height);
    return FALSE;
  }

  /* scrcpy uses fourcc-ish ids; treat unknown as H.264. */
  self->stream.codec = (codec_id == 0x68323635 /* "h265" */) ? PM_CODEC_H265
                                                             : PM_CODEC_H264;
  g_message ("Video stream: codec=0x%08x flags=0x%08x size=%ux%u",
             codec_id, flags, self->stream.width, self->stream.height);
  return TRUE;
}

/* Audio pump. Runs on its own thread (spawned by live_worker once the audio
 * socket is up) so a slow sink can never stall the video read loop. Entirely
 * best-effort: any failure here just means no sound, never a broken mirror.
 *
 * The server sends a 4-byte header (the negotiated codec id) before the PCM.
 * Raw PCM was requested, which - with frame metadata disabled - is a plain,
 * undelimited byte stream, so playback is just read -> write. */
static gpointer
audio_worker (gpointer data)
{
  PmSession *self = data;
  g_autoptr (GError) error = NULL;

  guint8 header[4];
  if (!pm_net_read_exact (self->audio_net, header, sizeof header, &error)) {
    if (!g_atomic_int_get (&self->stop))
      g_message ("Audio: no stream header (%s)", error ? error->message : "closed");
    return NULL;
  }

  guint32 codec_be = 0;
  memcpy (&codec_be, header, sizeof codec_be);
  guint32 codec_id = GUINT32_FROM_BE (codec_be);

  if (codec_id == PM_AUDIO_DISABLED) {
    g_message ("Audio: device reported no capturable audio; continuing video only");
    return NULL;
  }
  if (codec_id == PM_AUDIO_CONFIG_ERROR) {
    g_message ("Audio: server signalled a configuration error; continuing video only");
    return NULL;
  }
  if (codec_id != PM_AUDIO_CODEC_RAW) {
    g_message ("Audio: unexpected codec 0x%08x (expected raw PCM); skipping audio",
               codec_id);
    return NULL;
  }

  self->audio = pm_audio_new ();
  if (!pm_audio_open (self->audio, PM_AUDIO_SAMPLE_RATE, PM_AUDIO_CHANNELS, &error)) {
    g_message ("Audio: %s; continuing video only",
               error ? error->message : "could not open output");
    g_clear_pointer (&self->audio, pm_audio_free);
    return NULL;
  }
  g_message ("Audio: playing phone output at %u Hz, %d ch (raw PCM)",
             PM_AUDIO_SAMPLE_RATE, PM_AUDIO_CHANNELS);

  guint8 buf[16 * 1024];
  while (!g_atomic_int_get (&self->stop)) {
    gssize n = pm_net_read (self->audio_net, buf, sizeof buf, &error);
    if (n <= 0)
      break;
    if (!pm_audio_play (self->audio, buf, (gsize) n, &error)) {
      g_message ("Audio: %s; stopping playback",
                 error ? error->message : "write failed");
      break;
    }
  }

  g_clear_pointer (&self->audio, pm_audio_free);
  return NULL;
}

/* Send a single SET_DISPLAY_POWER control message on the worker thread. Used to
 * blank the phone panel once mirroring is live and to restore it on teardown.
 * Best-effort: a dead socket (e.g. the phone dropped off Wi-Fi) just means the
 * message never lands, which is harmless. */
static void
send_display_power (PmSession *self, gboolean on)
{
  if (self->control_net == NULL)
    return;
  guint8 buf[PM_CTRL_MSG_MAX];
  gsize len = pm_protocol_write_display_power (buf, on);
  g_autoptr (GError) error = NULL;
  if (!pm_net_write_all (self->control_net, buf, len, &error))
    g_debug ("display-power: could not send (screen %s): %s",
             on ? "on" : "off", error ? error->message : "write failed");
}

/* Blank the panel and record that it was blanked, arming the crash fail-safe (so
 * a fatal signal re-lights the panel and then locks the phone with a power-key
 * press) and holding off wake-on-input. Worker thread. */
static void
blank_screen (PmSession *self)
{
  send_display_power (self, FALSE);
  pm_failsafe_set_screen_blanked (TRUE);
  g_atomic_int_set (&self->wake_suppressed, TRUE);
  g_atomic_int_set (&self->screen_blanked, TRUE);
}

/* Lock the phone as the session tears down - on a clean stop, a spontaneous
 * disconnect, or (via failsafe.c) a crash. Runs exactly once per session (claimed
 * atomically) so the several teardown paths can all call it without double-sending.
 *
 * The server is launched WITHOUT power_off_on_close (it can't tell a settings
 * reconnect from a real disconnect, so it would lock on every reconnect), so the
 * phone is locked here by injecting KEYCODE_SLEEP. Unlike POWER's toggle, SLEEP
 * always sleeps (never wakes), so it locks regardless of the current screen
 * state - on slower panels (e.g. MIUI) the just-restored display has not lit yet
 * when the key lands, and a POWER toggle would wake the dark screen instead of
 * locking it. A blanked panel is still display-power-restored first so the phone
 * is usable again on the next wake.
 *
 * Skipped during a reconnect: there the next session inherits a live, unlocked
 * phone rather than one locked in the gap. Best-effort over the control socket, so a
 * dead socket (a silent network drop) just leaves the phone as-is - unreachable
 * and so unlockable. Returns TRUE if it wrote a control message that the server
 * still needs to drain. */
static gboolean
finalize_screen (PmSession *self)
{
  if (g_atomic_int_get (&self->reconnecting))
    return FALSE;
  if (!g_atomic_int_compare_and_exchange (&self->screen_finalized, FALSE, TRUE))
    return FALSE;

  g_atomic_int_set (&self->wake_suppressed, FALSE);

  /* Re-light the panel if it was blanked, so the power key below toggles a lit
   * screen off (sleeping + locking the phone) instead of waking a dark one. */
  if (g_atomic_int_compare_and_exchange (&self->screen_blanked, TRUE, FALSE)) {
    send_display_power (self, TRUE);
    pm_failsafe_set_screen_blanked (FALSE);
  }

  if (self->control_net == NULL)
    return FALSE;   /* socket already gone (e.g. a network drop) - can't lock */

  /* Press and release the sleep key to put the phone to sleep (locking it). */
  guint8 buf[PM_CTRL_MSG_MAX];
  g_autoptr (GError) error = NULL;
  gsize len = pm_protocol_write_key (buf, PM_KEY_DOWN, PM_ANDROID_KEYCODE_SLEEP, 0);
  if (!pm_net_write_all (self->control_net, buf, len, &error)) {
    g_debug ("lock: sleep-key down failed: %s", error ? error->message : "write failed");
    return FALSE;
  }
  len = pm_protocol_write_key (buf, PM_KEY_UP, PM_ANDROID_KEYCODE_SLEEP, 0);
  if (!pm_net_write_all (self->control_net, buf, len, &error))
    g_debug ("lock: sleep-key up failed: %s", error ? error->message : "write failed");
  return TRUE;
}

/* Read the device's MAC, commit any PIN entered before the MAC was known, and -
 * if a PIN is stored for this device - best-effort unlock the keyguard. Entirely
 * non-fatal: every failure path just leaves the phone as-is and logs at debug.
 * `serial` is the adb endpoint ("host:port"). */
static void
maybe_unlock_device (PmSession *self, const char *serial)
{
  g_autofree char *mac = pm_adb_query_mac (serial);
  if (mac == NULL) {
    g_debug ("auto-unlock: could not read device MAC; offering one-time PIN");
    if (pm_adb_query_lock_state (serial) != PM_LOCK_UNLOCKED)
      post_pin_required (self);
    return;
  }

  /* Promote a freshly-entered ("pending") PIN to this device's MAC group. */
  if (pm_pinstore_commit_pending (mac))
    g_message ("auto-unlock: saved PIN for device %s", mac);

  g_autofree char *pin = pm_pinstore_get (mac);
  if (pin == NULL) {
    /* Don't interrupt an already-unlocked phone. If this OEM doesn't expose a
     * readable lock state, prefer offering the prompt: the user can still
     * cancel it, while a locked secure screen would otherwise be unusable. */
    if (pm_adb_query_lock_state (serial) != PM_LOCK_UNLOCKED)
      post_pin_required (self);
    return;
  }

  /* A PIN is stored, so the keyguard is about to be driven (several adb round
   * trips plus deliberate settle delays, during which Android blanks the secure
   * screen). Bracket it with the UI notice so the "Unlocking…" alert floats over
   * the live mirror for exactly as long as the attempt runs. */
  post_unlocking (self, TRUE);

  g_message ("auto-unlock: attempting to unlock device %s", mac);
  g_autoptr (GError) unlock_err = NULL;
  gboolean unlocked = FALSE;
  if (!pm_adb_unlock_with_pin (serial, pin, &unlocked, &unlock_err))
    g_debug ("auto-unlock: unlock attempt failed: %s",
             unlock_err ? unlock_err->message : "(unknown)");

  post_unlocking (self, FALSE);

  /* Ran cleanly but the keyguard never dismissed across every capped attempt:
   * the saved PIN is almost certainly wrong. Tell the window so it can alert the
   * user and ask for a correct PIN rather than letting the next connect retry
   * into Android's wrong-PIN lockout. */
  if (!unlocked)
    post_unlock_failed (self);

  /* Don't leave the plaintext PIN lingering in this buffer longer than needed. */
  memset (pin, 0, strlen (pin));
}

/* Auto-unlock runs on its own short-lived thread (started once the stream is
 * live) so its adb round trips and settle delays never stall the frame loop -
 * the user watches the unlock happen under the alert instead of a frozen frame.
 * Owns its serial copy; joined by the worker's teardown. */
typedef struct { PmSession *self; char *serial; } UnlockCtx;

static gpointer
unlock_worker (gpointer data)
{
  UnlockCtx *ctx = data;
  maybe_unlock_device (ctx->self, ctx->serial);
  g_free (ctx->serial);
  g_free (ctx);
  return NULL;
}

static gpointer
live_worker (gpointer data)
{
  PmSession *self = data;
  g_autoptr (GError) error = NULL;
  /* All autocleanup locals are declared and NULL-initialised up front so the
   * `goto fail` paths never run cleanup over an uninitialised pointer. */
  g_autofree char *serial = NULL;
  g_autofree char *device_name = NULL;
  g_autofree char *jar = NULL;
  g_autofree char *version = NULL;
  g_autofree char *new_display_arg = NULL;
  g_autofree char *bitrate_arg = NULL;
  const char *step = "Starting connection";

  post_state (self, PM_STATE_CONNECTING, _("Connecting to device…"));

  step = "Locating scrcpy-server";
  g_message ("%s", step);
  jar = find_server_jar ();
  if (jar == NULL) {
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         _("Could not find scrcpy-server. Install scrcpy or set PM_SERVER_JAR to the server path."));
    goto fail;
  }

  version = find_scrcpy_version ();
  if (version == NULL) {
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         _("Could not determine scrcpy-server version. Install scrcpy or set PM_SCRCPY_VERSION to match your server."));
    goto fail;
  }

  /* Audio is negotiated here: with audio_codec=raw and frame meta disabled the
   * phone streams plain 16-bit PCM on a second socket (see audio_worker). */
  const gboolean want_audio = self->audio_enabled;
  /* Virtual display: new_display=<w>x<h>/<dpi> tells the server to spin up a
   * fresh secondary display rather than mirroring the physical screen. Each
   * part is optional, so any missing geometry / density is dropped and the
   * device default fills in. The whole arg stays NULL when mirroring so it
   * is omitted entirely. */
  const gboolean want_virtual = self->display_mode == PM_DISPLAY_VIRTUAL;
  if (want_virtual) {
    if (self->display_width > 0 && self->display_height > 0 && self->display_dpi > 0)
      new_display_arg = g_strdup_printf ("new_display=%ux%u/%u",
                                         self->display_width, self->display_height,
                                         self->display_dpi);
    else if (self->display_width > 0 && self->display_height > 0)
      new_display_arg = g_strdup_printf ("new_display=%ux%u",
                                         self->display_width, self->display_height);
    else
      new_display_arg = g_strdup ("new_display=");
  }
  /* video_bit_rate is in bits/sec; the UI works in Mbps. A 0 value means "use
   * the scrcpy default", so the arg is omitted entirely in that case. */
  if (self->video_bitrate > 0)
    bitrate_arg = g_strdup_printf ("video_bit_rate=%u",
                                   self->video_bitrate * 1000000u);
  const char *server_args[] = {
    version,
    "log_level=debug",
    want_audio ? "audio=true" : "audio=false",
    "audio_codec=raw",
    "control=true",
    "tunnel_forward=true",
    "send_frame_meta=false",
    "send_dummy_byte=true",
    "send_device_meta=true",
    "send_stream_meta=true",
    "clipboard_autosync=false",
    "power_on=true",
    /* Deliberately NOT power_off_on_close: the server's close-handler can't tell a
     * settings reconnect (which force-kills and relaunches it) from a real
     * disconnect, so it would lock the phone on every settings change. The phone
     * is locked on a genuine teardown instead (see finalize_screen), letting
     * reconnects hand the next session a live, unlocked phone. Trade-off: a silent
     * network drop leaves the device unreachable, so it can no longer be locked. */
    bitrate_arg,       /* non-NULL whenever a bitrate is set (the normal case) */
    new_display_arg,   /* NULL when mirroring → trailing optional, omitted     */
    NULL,
  };
  /* pm_adb_spawn_server stops at the first NULL, so every optional arg that can
   * be NULL must trail the mandatory ones. bitrate_arg is only NULL when the
   * bitrate is 0 (never via the UI, which clamps to >= 1 Mbps); keep it ahead of
   * new_display_arg, the genuinely optional tail. */

  step = "Connecting with adb";
  post_state (self, PM_STATE_CONNECTING, _("Connecting with adb…"));
  g_message ("%s: %s:%u", step, self->target.host, self->target.port);
  if (!pm_adb_connect (self->target.host, self->target.port, &error))
    goto fail;

  serial = g_strdup_printf ("%s:%u", self->target.host, self->target.port);

  step = "Pushing scrcpy-server";
  post_state (self, PM_STATE_CONNECTING, _("Installing mirror server…"));
  g_message ("%s: %s", step, jar);
  if (!pm_adb_push (serial, jar, PM_REMOTE_JAR, &error))
    goto fail;

  step = "Forwarding scrcpy socket";
  post_state (self, PM_STATE_CONNECTING, _("Opening mirror socket…"));
  g_message ("%s", step);
  if (!pm_adb_forward (serial, PM_LOCAL_PORT, PM_REMOTE_SOCKET, &error))
    goto fail;

  step = "Starting scrcpy-server";
  post_state (self, PM_STATE_CONNECTING, _("Starting mirror server…"));
  g_message ("%s %s", step, version);
  self->server = pm_adb_spawn_server (serial, PM_REMOTE_JAR, PM_SERVER_CLASS, server_args, &error);
  if (self->server == NULL)
    goto fail;

  step = "Connecting video socket";
  post_state (self, PM_STATE_CONNECTING, _("Connecting video stream…"));
  g_message ("%s", step);
  if (!connect_server_socket (self->server, TRUE, &self->video_net, &error))
    goto fail;

  /* The server accepts sockets in a fixed order: video, then audio (when
   * enabled), then control. Connect the audio socket here so the control socket
   * still lands on the right stream; audio playback itself runs on its own
   * thread once the pipeline is live. */
  if (want_audio) {
    step = "Connecting audio socket";
    post_state (self, PM_STATE_CONNECTING, _("Connecting audio stream…"));
    g_message ("%s", step);
    if (!connect_server_socket (self->server, FALSE, &self->audio_net, &error))
      goto fail;
  }

  step = "Connecting control socket";
  post_state (self, PM_STATE_CONNECTING, _("Connecting controls…"));
  g_message ("%s", step);
  if (!connect_server_socket (self->server, FALSE, &self->control_net, &error))
    goto fail;

  /* Arm the crash fail-safe on the live control socket: from here on a fatal
   * signal can still flush pointer/key releases (and a panel wake) to the
   * device. Disarmed in teardown before the socket is closed. */
  pm_failsafe_arm (pm_net_get_fd (self->control_net));

  step = "Reading stream metadata";
  post_state (self, PM_STATE_CONNECTING, _("Reading video stream…"));
  g_message ("%s", step);
  if (!read_stream_meta (self, &device_name, &error))
    goto fail;

  step = "Opening video decoder";
  self->decoder = pm_decoder_new (on_decoder_frame, self);
  if (!pm_decoder_open (self->decoder, self->stream.codec, &error))
    goto fail;

  post_state (self, PM_STATE_MIRRORING, NULL);

  /* Persist the now-confirmed endpoint so the next launch can silently
   * re-check it (keeping its window hidden until that check fails). The setup
   * dialog already saves on manual pairing; do the same for a discovery-driven
   * connect, and refresh the last-known host:port. */
  {
    PmDeviceInfo confirmed = {
      .name   = device_name != NULL ? device_name : self->target.name,
      .host   = self->target.host,
      .port   = self->target.port,
    };
    if (confirmed.host != NULL)
      pm_device_save (&confirmed, NULL);
  }

  /* Start audio playback alongside video. The thread owns the read->sink pump
   * and is joined during this worker's teardown below. */
  if (self->audio_net != NULL)
    self->audio_worker = g_thread_new ("pm-audio", audio_worker, self);

  /* Auto-unlock now that the stream is live, on its own thread so the keyguard
   * dance never stalls the frame loop below: the user watches the unlock happen
   * under the "Unlocking…" alert instead of a black screen. The MAC is the stable
   * key for the saved PIN (the IP may change across DHCP leases); a phone with no
   * saved PIN, an unreadable MAC, or a stubborn OEM lockscreen just streams on.
   * Joined in teardown. */
  {
    UnlockCtx *ctx = g_new0 (UnlockCtx, 1);
    ctx->self = self;
    ctx->serial = g_strdup (serial);
    self->unlock_worker = g_thread_new ("pm-unlock", unlock_worker, ctx);
  }

  /* Keyguard polling drives two features off a single adb probe taken on a fixed
   * cadence from inside the stream loop (cheap, no extra thread, and this worker
   * stays the only control-socket writer besides input):
   *
   *   - The floating Unlock button: shown over the mirror whenever the phone is
   *     sitting on its lockscreen, so the user can punch in a one-time PIN.
   *   - Battery saver (screen-off): blank the phone's physical panel while
   *     mirroring, but only once the keyguard is gone - otherwise it would just
   *     black out an unusable lock screen they still have to get past. If the
   *     device never reports a lock state, blanking happens right away so the
   *     feature still works. `screen_blanked` also gates the restore on teardown.
   */
  const gboolean want_screen_off = self->screen_off;
  gboolean blanked = FALSE;
  gboolean reported_locked = FALSE;   /* last lock state pushed to the UI button */
  gint64 next_lock_poll_us = 0;       /* 0 → probe on the very first frame */
  /* Clear any leftover blank state from a prior connection so a reconnect
   * starts with the screen lit and wakeable. */
  pm_failsafe_set_screen_blanked (FALSE);
  g_atomic_int_set (&self->wake_suppressed, FALSE);
  g_atomic_int_set (&self->screen_blanked, FALSE);
  g_atomic_int_set (&self->screen_finalized, FALSE);

  /* Stream loop. */
  guint8 buf[64 * 1024];
  while (!g_atomic_int_get (&self->stop)) {
    gssize n = pm_net_read (self->video_net, buf, sizeof buf, &error);
    if (n <= 0)
      break;
    if (!pm_decoder_feed (self->decoder, buf, (gsize) n, &error))
      break;

    gint64 now = g_get_monotonic_time ();
    if (now >= next_lock_poll_us) {
      next_lock_poll_us = now + PM_LOCK_POLL_US;
      PmLockState lock = pm_adb_query_lock_state (serial);

      /* Raise the Unlock button only on a definitively-locked keyguard; UNKNOWN
       * (OEMs that don't expose the state) counts as unlocked so the button is
       * never stranded up on a device that can't report. Posted on transitions
       * only, so the UI isn't churned on every poll. */
      gboolean locked_now = (lock == PM_LOCK_LOCKED);
      if (locked_now != reported_locked) {
        post_locked (self, locked_now);
        reported_locked = locked_now;
      }

      if (want_screen_off && !blanked) {
        if (lock == PM_LOCK_UNKNOWN)
          /* Can't read the lock state on this device - blank immediately rather
           * than never honouring the setting. */
          g_debug ("screen-off: lock state unknown; blanking without unlock gate");

        if (lock == PM_LOCK_UNLOCKED || lock == PM_LOCK_UNKNOWN) {
          blank_screen (self);
          blanked = TRUE;
        }
        /* PM_LOCK_LOCKED → keep the panel lit and probe again next cycle. */
      }
    }
  }

  /* Lock the phone as the session ends (restoring the panel first if it was
   * blanked, so the device is never left dark). Idempotent: a deliberate
   * pm_session_stop() may already have done it while the socket was still open, in
   * which case this is a no-op. On a Wi-Fi drop the socket is already dead, so the
   * best-effort write fails and the phone - now unreachable - stays as it was. */
  gboolean locked = finalize_screen (self);
  /* Let the lock keypress drain to the server before it is force-killed below,
   * mirroring the clean-stop path. Skipped when nothing was sent - already
   * finalized by pm_session_stop(), reconnecting, or a dead socket. */
  if (locked && self->control_net != NULL)
    g_usleep (PM_LOCK_DRAIN_US);

  if (!g_atomic_int_get (&self->stop))
    post_state (self, PM_STATE_IDLE, NULL);
  if (self->server != NULL && !g_subprocess_get_if_exited (self->server))
    g_subprocess_force_exit (self->server);
  g_clear_object (&self->server);

  /* Release the pipeline this worker owns. On a spontaneous disconnect (e.g.
   * the phone's Wi-Fi dropped) the UI just shows IDLE and never calls
   * pm_session_stop, so without this the decoder, sockets and adb forward rule
   * would leak on every reconnect. g_clear_pointer NULLs each field, so a later
   * pm_session_stop (which joins this worker before touching them) sees nothing
   * to free and cannot double-free. */
  if (serial != NULL)
    pm_adb_forward_remove (serial, PM_LOCAL_PORT, NULL);
  /* Stop audio first: close its socket to unblock the pump's read, then join it
   * so the PmAudio sink (which the audio thread owns) is gone before return. */
  if (self->audio_net != NULL)
    pm_net_close (self->audio_net);
  if (self->audio_worker != NULL) {
    g_thread_join (self->audio_worker);
    self->audio_worker = NULL;
  }
  /* Best-effort auto-unlock thread: join it so it never touches the session after
   * teardown. It is short-lived (bounded adb round trips), so this won't hang. */
  if (self->unlock_worker != NULL) {
    g_thread_join (self->unlock_worker);
    self->unlock_worker = NULL;
  }
  g_clear_pointer (&self->decoder, pm_decoder_free);
  g_clear_pointer (&self->video_net, pm_net_free);
  g_clear_pointer (&self->audio_net, pm_net_free);
  pm_failsafe_arm (-1);   /* disarm before the control socket goes away */
  g_clear_pointer (&self->control_net, pm_net_free);
  return NULL;

fail:
  {
    g_autofree char *server_out = NULL;
    g_autofree char *server_err = NULL;
    g_autofree char *server_note = NULL;

    if (self->server != NULL && !g_subprocess_get_if_exited (self->server))
      g_subprocess_force_exit (self->server);
    if (self->server != NULL) {
      g_subprocess_communicate_utf8 (self->server, NULL, NULL,
                                     &server_out, &server_err, NULL);
      char *out = server_out ? g_strstrip (server_out) : NULL;
      char *err = server_err ? g_strstrip (server_err) : NULL;
      const char *log = (out && *out) ? out : (err && *err) ? err : NULL;
      if (log != NULL)
        server_note = g_strdup_printf (" Server log: %.500s", log);
    }
    g_clear_object (&self->server);
    if (serial != NULL)
      pm_adb_forward_remove (serial, PM_LOCAL_PORT, NULL);

    g_clear_pointer (&self->decoder, pm_decoder_free);
    g_clear_pointer (&self->video_net, pm_net_free);
    g_clear_pointer (&self->audio_net, pm_net_free);
    pm_failsafe_arm (-1);   /* disarm before the control socket goes away */
    g_clear_pointer (&self->control_net, pm_net_free);

    g_autofree char *message =
      g_strdup_printf (_("%s failed: %s%s"),
                       step,
                       error ? error->message : _("connection failed"),
                       server_note ? server_note : "");
    g_message ("%s", message);
    if (self->silent)
      post_state (self, PM_STATE_IDLE, NULL);
    else
      post_state (self, PM_STATE_ERROR, message);
  }
  return NULL;
}

/* ------------------------------------------------------------------------- */
/* demo pipeline (explicit PM_DEMO=1)                                        */
/* ------------------------------------------------------------------------- */

/* Build a colourful test pattern so the renderer + view are exercised without
 * a phone present. Returns a transfer-full GdkTexture. */
static GdkTexture *
make_test_pattern (int w, int h)
{
  const int stride = w * 3;
  guint8 *rgb = g_malloc ((gsize) stride * h);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      guint8 *p = rgb + y * stride + x * 3;
      gboolean check = ((x / 24) + (y / 24)) & 1;
      p[0] = (guint8) (255 * x / w);
      p[1] = (guint8) (255 * y / h);
      p[2] = check ? 200 : 80;
    }
  }
  GBytes *bytes = g_bytes_new_take (rgb, (gsize) stride * h);
  GdkTexture *tex = GDK_TEXTURE (gdk_memory_texture_new (w, h, GDK_MEMORY_R8G8B8, bytes, stride));
  g_bytes_unref (bytes);
  return tex;
}

static gboolean
demo_to_mirroring (gpointer data)
{
  PmSession *self = data;
  self->demo_timer = 0;

  self->stream = (PmStreamInfo){ .codec = PM_CODEC_H264, .width = 360, .height = 780 };
  if (self->view != NULL) {
    g_autoptr (GdkTexture) tex = make_test_pattern (360, 780);
    pm_video_view_set_texture (self->view, tex);
  }
  pm_session_set_state (self, PM_STATE_MIRRORING, NULL);
  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/* discovery -> connect                                                      */
/* ------------------------------------------------------------------------- */

/* Move from discovery to the streaming pipeline. Normal app usage always
 * starts the real adb/decode pipeline; the synthetic pattern is reserved for
 * explicit demo mode so it cannot masquerade as a live phone. */
static void
begin_connect (PmSession *self)
{
  pm_session_set_state (self, PM_STATE_CONNECTING,
                        _("Found device — starting mirror server…"));

  if (demo_mode_enabled ())
    self->demo_timer = g_timeout_add (1400, demo_to_mirroring, self);
  else
    self->worker = g_thread_new ("pm-session", live_worker, self);
}

/* Defer tearing down discovery until after the current Avahi callback unwinds
 * (freeing the Avahi client from inside its own callback is unsafe). */
static gboolean
free_discovery_idle (gpointer data)
{
  PmSession *self = data;
  g_clear_pointer (&self->discovery, pm_discovery_free);
  return G_SOURCE_REMOVE;
}

/* Called on the main thread by PmDiscovery. Idempotent: ignores reports after
 * leaving SEARCHING because mDNS and the probe thread can both fire. */
static void
on_discovery_found (const PmDeviceInfo *info, gpointer user_data)
{
  PmSession *self = user_data;
  if (self->state != PM_STATE_SEARCHING)
    return;

  /* Adopt the discovered endpoint. */
  g_clear_pointer (&self->target.host, g_free);
  self->target.host = g_strdup (info->host);
  self->target.port = info->port;
  if (info->name != NULL && self->target.name == NULL)
    self->target.name = g_strdup (info->name);

  if (self->demo_timer != 0) {
    g_source_remove (self->demo_timer);
    self->demo_timer = 0;
  }
  g_idle_add_full (G_PRIORITY_DEFAULT, free_discovery_idle,
                   g_object_ref (self), g_object_unref);

  begin_connect (self);
}

static void
on_discovery_probe_failed (gpointer user_data)
{
  PmSession *self = user_data;
  if (self->silent && self->state == PM_STATE_SEARCHING)
    g_signal_emit (self, signals[SIG_STARTUP_CHECK_FAILED], 0);
}

/* No device discovered in time. Demo mode may still show the renderer pattern;
 * normal sessions report the discovery failure instead of pretending to be
 * connected. */
static gboolean
discovery_timeout (gpointer data)
{
  PmSession *self = data;
  self->demo_timer = 0;
  if (self->state == PM_STATE_SEARCHING) {
    g_clear_pointer (&self->discovery, pm_discovery_free);

    if (demo_mode_enabled ())
      begin_connect (self);
    else if (self->silent)
      pm_session_set_state (self, PM_STATE_IDLE, NULL);
    else
      pm_session_set_state (self, PM_STATE_ERROR,
                            _("No phone found"));
  }
  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

static void
pm_session_start_internal (PmSession           *self,
                           const PmDeviceInfo  *target,
                           gboolean             silent,
                           gboolean             skip_discovery)
{
  g_return_if_fail (PM_IS_SESSION (self));
  if ((self->state == PM_STATE_SEARCHING || self->state == PM_STATE_CONNECTING) &&
      self->silent && !silent)
    pm_session_stop (self);

  if (self->state == PM_STATE_SEARCHING || self->state == PM_STATE_CONNECTING ||
      self->state == PM_STATE_MIRRORING)
    return;

  g_atomic_int_set (&self->stop, 0);
  self->silent = silent;

  if (self->worker != NULL) {
    g_thread_join (self->worker);
    self->worker = NULL;
  }

  /* Resolve target: explicit arg > saved pairing > network discovery. */
  pm_device_info_clear (&self->target);
  if (target != NULL) {
    self->target.name   = g_strdup (target->name);
    self->target.host   = g_strdup (target->host);
    self->target.port   = target->port;
  } else if (pm_device_has_pairing ()) {
    pm_device_load (&self->target, NULL);
  } else {
    /* No prior info: rely on mDNS plus adb's own paired-device discovery. */
    self->target.port = 5555;
  }

  /* Seamless reconnect: with a confirmed endpoint already held (e.g. the user
   * toggled the display mode mid-session) skip the discovery round trip and go
   * straight to the live pipeline. begin_connect() handles demo vs. real. */
  if (skip_discovery && self->target.host != NULL) {
    begin_connect (self);
    return;
  }

  pm_session_set_state (self, PM_STATE_SEARCHING, NULL);

  /* Start real discovery (mDNS + adb fallback). on_discovery_found() drives
   * the transition to CONNECTING. */
  self->discovery = pm_discovery_new (&self->target, on_discovery_found, self);
  pm_discovery_set_probe_failed_cb (self->discovery, on_discovery_probe_failed, self);
  pm_discovery_start (self->discovery);

  /* Demo mode is opt-in. It keeps the renderer demonstrable without a phone,
   * while normal use reports discovery failures honestly. */
  self->demo_timer = g_timeout_add (PM_DISCOVERY_TIMEOUT_MS,
                                    discovery_timeout, self);
}

void
pm_session_start (PmSession *self, const PmDeviceInfo *target)
{
  pm_session_start_internal (self, target, FALSE, FALSE);
}

void
pm_session_start_silent (PmSession *self, const PmDeviceInfo *target)
{
  pm_session_start_internal (self, target, TRUE, FALSE);
}

void
pm_session_reconnect (PmSession *self)
{
  g_return_if_fail (PM_IS_SESSION (self));

  /* Only an in-flight or live session can be reconnected; otherwise the changed
   * settings simply apply the next time the user connects. */
  if (self->state != PM_STATE_CONNECTING && self->state != PM_STATE_MIRRORING)
    return;
  if (self->target.host == NULL)
    return;   /* nothing confirmed to reconnect to */

  /* Copy the endpoint before pm_session_stop() clears self->target. */
  PmDeviceInfo target = {
    .name   = g_strdup (self->target.name),
    .host   = g_strdup (self->target.host),
    .port   = self->target.port,
  };

  /* Tear the current pipeline down, then go straight back to the same endpoint
   * with the current settings - no discovery wait, so the swap is seamless. The
   * reconnecting flag suppresses the teardown's put-to-sleep so the next session
   * picks up a live, unlocked phone instead of one locked in the gap. */
  g_atomic_int_set (&self->reconnecting, TRUE);
  pm_session_stop (self);
  pm_session_start_internal (self, &target, FALSE, TRUE);
  g_atomic_int_set (&self->reconnecting, FALSE);

  pm_device_info_clear (&target);
}

void
pm_session_stop (PmSession *self)
{
  g_return_if_fail (PM_IS_SESSION (self));

  g_atomic_int_set (&self->stop, 1);
  self->silent = FALSE;

  g_clear_pointer (&self->discovery, pm_discovery_free);

  if (self->demo_timer != 0) {
    g_source_remove (self->demo_timer);
    self->demo_timer = 0;
  }
  if (self->worker != NULL) {
    /* Release anything still held on the device (a pointer mid-press, a held
     * navigation key) while the control socket is open, so ending the session
     * mid-gesture can never strand a finger or key on the phone - which would
     * block its back/home gestures until a reboot. The crash fail-safe covers
     * the abnormal-exit case; this covers a clean close. */
    if (self->input != NULL)
      pm_input_release_all (self->input);
    /* Hand the lock + teardown to the worker rather than doing it here too.
     * Closing only the video socket wakes the worker out of its blocking stream
     * read so it falls through to its own teardown, which locks the phone over
     * the still-open control socket, lets the SLEEP keypress drain, then
     * force-kills the server and closes the remaining sockets.
     *
     * Both paths used to lock and tear down concurrently, racing on the one-shot
     * screen_finalized claim. When the worker won that claim - e.g. it was inside
     * the ~hundreds-of-ms keyguard poll (pm_adb_query_lock_state) when stop
     * landed, so it left the loop and reached its own finalize first - this
     * thread saw finalize_screen return FALSE, skipped the drain, and immediately
     * closed the control socket / force-killed the server, cutting off the
     * worker's just-issued SLEEP keypress before the device injected it. The
     * phone then stayed fully awake. Leaving the worker as the sole owner of the
     * lock and teardown removes the race; this thread only wakes the read it is
     * blocked on and joins. (The fail-safe is disarmed inside the worker's
     * teardown, before it closes the control socket.) */
    if (self->video_net) pm_net_close (self->video_net);
    g_thread_join (self->worker);
    self->worker = NULL;
  }

  g_clear_pointer (&self->input, pm_input_free);
  g_clear_pointer (&self->decoder, pm_decoder_free);
  g_clear_pointer (&self->audio, pm_audio_free);
  g_clear_pointer (&self->video_net, pm_net_free);
  g_clear_pointer (&self->audio_net, pm_net_free);
  g_clear_pointer (&self->control_net, pm_net_free);
  g_clear_object (&self->server);
  if (self->view)
    pm_video_view_clear (self->view);

  pm_session_set_state (self, PM_STATE_IDLE, NULL);
}

PmState
pm_session_get_state (PmSession *self)
{
  g_return_val_if_fail (PM_IS_SESSION (self), PM_STATE_IDLE);
  return self->state;
}

char *
pm_session_dup_serial (PmSession *self)
{
  g_return_val_if_fail (PM_IS_SESSION (self), NULL);

  /* Only meaningful once mirroring: the endpoint is settled by then (discovery
   * may still be mutating target.host while CONNECTING). */
  if (self->state != PM_STATE_MIRRORING || self->target.host == NULL)
    return NULL;
  return g_strdup_printf ("%s:%u", self->target.host, self->target.port);
}

gboolean
pm_session_get_stream_info (PmSession *self, PmStreamInfo *out)
{
  g_return_val_if_fail (PM_IS_SESSION (self), FALSE);

  if (self->stream.width == 0 || self->stream.height == 0)
    return FALSE;

  if (out != NULL)
    *out = self->stream;
  return TRUE;
}

void
pm_session_set_video_view (PmSession *self, PmVideoView *view)
{
  g_return_if_fail (PM_IS_SESSION (self));
  self->view = view;
}

/* ------------------------------------------------------------------------- */
/* GObject boilerplate                                                       */
/* ------------------------------------------------------------------------- */

static void
pm_session_dispose (GObject *object)
{
  PmSession *self = PM_SESSION (object);
  pm_session_stop (self);
  pm_device_info_clear (&self->target);
  G_OBJECT_CLASS (pm_session_parent_class)->dispose (object);
}

static void
pm_session_class_init (PmSessionClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pm_session_dispose;

  signals[SIG_STATE_CHANGED] =
    g_signal_new ("state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);

  signals[SIG_STREAM_CHANGED] =
    g_signal_new ("stream-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);

  signals[SIG_STARTUP_CHECK_FAILED] =
    g_signal_new ("startup-check-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /* The connected phone is locked and has no stored PIN. The window asks for a
   * one-time PIN, which is never persisted and is required again next time. */
  signals[SIG_PIN_REQUIRED] =
    g_signal_new ("pin-required",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /* TRUE when auto-unlock starts driving the keyguard, FALSE when it finishes.
   * Runs while the stream is already live, so the window floats an "Unlocking…"
   * alert over the mirror (black while Android keeps the secure screen blanked)
   * and drops it once the phone is in. */
  signals[SIG_UNLOCKING] =
    g_signal_new ("unlocking",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  /* Auto-unlock tried the saved PIN the capped number of times and the phone
   * stayed locked - the PIN is almost certainly wrong. The window alerts the
   * user and offers to enter the correct one before further attempts risk a
   * lockout. */
  signals[SIG_UNLOCK_FAILED] =
    g_signal_new ("unlock-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /* TRUE when the live phone has settled onto its lockscreen, FALSE when the
   * keyguard is dismissed. Polled from the stream loop; the window floats an
   * Unlock button over the mirror while it is TRUE. */
  signals[SIG_LOCKED_CHANGED] =
    g_signal_new ("locked-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
pm_session_init (PmSession *self)
{
  self->state = PM_STATE_IDLE;
  self->mouse_mode = TRUE;
  self->audio_enabled = TRUE;
  self->video_bitrate = 8;   /* Mbps; matches scrcpy's default */
  self->display_mode = PM_DISPLAY_MIRROR;
  self->display_width = 1920;
  self->display_height = 1080;
  self->display_dpi = 190;
  self->screen_off = TRUE;

  /* Install the crash fail-safe once, so a fatal signal mid-session can still
   * release held pointers/keys and re-light the panel (see failsafe.c). */
  pm_failsafe_install ();
}

PmSession *
pm_session_new (void)
{
  return g_object_new (PM_TYPE_SESSION, NULL);
}
