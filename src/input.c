/* input.c - GTK event controllers -> scrcpy control messages. */
#include "input.h"
#include "adb.h"
#include "protocol.h"
#include "failsafe.h"
#include <gdk/gdkkeysyms.h>
#include <string.h>

/* Most simultaneously-held navigation keys tracked for the crash fail-safe. */
#define PM_MAX_HELD_KEYS 8

/* A dozing phone ignores injected touches, so a real interaction must first
 * wake it like a physical tap would. KEYCODE_WAKEUP is fired (via adb) on
 * genuine input - presses, key-downs, scrolls - throttled so a burst of clicks
 * spawns at most one adb process per interval. Hover/move are excluded; they
 * are not deliberate "I want the screen" actions and would spam adb. */
#define PM_WAKE_THROTTLE_US (G_USEC_PER_SEC * 3)

/* A subset of Android KeyEvent keycodes (android.view.KeyEvent). Printable
 * text is better sent via INJECT_TEXT; this table covers editing/navigation
 * keys that have no text. Extend as needed. */
#define AKEYCODE_BACK         4
#define AKEYCODE_HOME         3
#define AKEYCODE_DPAD_UP      19
#define AKEYCODE_DPAD_DOWN    20
#define AKEYCODE_DPAD_LEFT    21
#define AKEYCODE_DPAD_RIGHT   22
#define AKEYCODE_ENTER        66
#define AKEYCODE_DEL          67   /* backspace */
#define AKEYCODE_FORWARD_DEL  112

/* scrcpy reserves negative pointer ids for synthetic sources. POINTER_ID_MOUSE
 * makes the server inject from SOURCE_MOUSE (so the phone reacts as if a real
 * mouse is attached); GENERIC_FINGER injects from SOURCE_TOUCHSCREEN. */
#define SCRCPY_POINTER_ID_MOUSE          G_GUINT64_CONSTANT (0xffffffffffffffff)
#define SCRCPY_POINTER_ID_GENERIC_FINGER G_GUINT64_CONSTANT (0xfffffffffffffffe)

/* Forwarded mouse buttons, mapping each GDK button to its Android
 * AMOTION_EVENT_BUTTON bit. One independent click gesture is installed per
 * entry, allowing simultaneous button holds (e.g. drag-with-left while
 * right-clicking). */
typedef struct { guint gdk; guint32 android; } PmButtonMap;
static const PmButtonMap pm_button_map[] = {
  { GDK_BUTTON_PRIMARY,   PM_BUTTON_PRIMARY   },   /* left   */
  { GDK_BUTTON_MIDDLE,    PM_BUTTON_TERTIARY  },   /* middle */
  { GDK_BUTTON_SECONDARY, PM_BUTTON_SECONDARY },   /* right  */
  { 8,                    PM_BUTTON_BACK      },   /* back   */
  { 9,                    PM_BUTTON_FORWARD   },   /* forward*/
};
#define PM_N_BUTTONS G_N_ELEMENTS (pm_button_map)

static guint32
gdk_button_to_android (guint gdk_button)
{
  for (guint i = 0; i < PM_N_BUTTONS; i++)
    if (pm_button_map[i].gdk == gdk_button)
      return pm_button_map[i].android;
  return PM_BUTTON_NONE;
}

struct _PmInput {
  PmNet        *net;         /* borrowed */
  PmStreamInfo  stream;
  PmVideoView  *view;        /* borrowed */
  char         *serial;      /* adb serial for wake-on-input; NULL = disabled */
  const gint   *wake_suppressed; /* borrowed atomic; non-zero => skip waking */
  gint64        last_wake_us; /* monotonic time of the last wake, for throttle */
  GtkEventController *clicks[PM_N_BUTTONS];
  GtkEventController *motion;
  GtkEventController *scroll;
  GtkEventController *key;
  double        last_x, last_y;
  double        touch_x, touch_y;
  gboolean      pressed;       /* any pointer contact / button is active */
  guint32       buttons_state; /* mouse mode: currently-pressed Android buttons */
  gboolean      mouse_mode;    /* TRUE: forward as mouse; FALSE: as touch */
  guint32       held_keys[PM_MAX_HELD_KEYS]; /* Android keycodes currently down */
  guint         n_held_keys;
};

PmInput *
pm_input_new (PmNet              *control_net,
              const PmStreamInfo *stream,
              const char         *serial,
              const gint         *wake_suppressed)
{
  PmInput *self = g_new0 (PmInput, 1);
  self->net = control_net;
  self->serial = g_strdup (serial);
  self->wake_suppressed = wake_suppressed;
  self->mouse_mode = TRUE;
  if (stream)
    self->stream = *stream;
  return self;
}

/* Wake the device if this looks like deliberate input and a wake hasn't just
 * been sent. Cheap and non-blocking (the adb call is fire-and-forget). */
static void
wake_on_input (PmInput *self)
{
  if (self->serial == NULL)
    return;
  /* Held off while the session has deliberately blanked the panel: there the
   * device is still awake and accepting input, so a KEYCODE_WAKEUP would only
   * re-light the screen and undo the battery saving. */
  if (self->wake_suppressed != NULL && g_atomic_int_get (self->wake_suppressed))
    return;
  gint64 now = g_get_monotonic_time ();
  if (self->last_wake_us != 0 && now - self->last_wake_us < PM_WAKE_THROTTLE_US)
    return;
  self->last_wake_us = now;
  pm_adb_wake_screen (self->serial);
}

void
pm_input_set_stream (PmInput *self, const PmStreamInfo *stream)
{
  g_return_if_fail (self != NULL);
  if (stream)
    self->stream = *stream;
}

static void
send_msg (PmInput *self, const guint8 *buf, gsize len)
{
  if (self->net == NULL)
    return;
  g_autoptr (GError) error = NULL;
  if (!pm_net_write_all (self->net, buf, len, &error))
    g_warning ("input: failed to send control message: %s", error->message);
  else
    g_debug ("input: sent control message type=%u len=%" G_GSIZE_FORMAT, buf[0], len);
}

/* Re-publish the "undo input" payload the crash fail-safe will flush if the app
 * dies right now: an ACTION_UP for any held pointer plus a KEY_UP for every held
 * key, so a crash mid-drag or mid-keypress can never strand a phantom finger or
 * stuck key on the device (which would block back/home gestures). Called on
 * every press/release/held-drag/key change; cheap, and empties to nothing once
 * the user lets go. Runs on the UI thread only (the fail-safe's single writer). */
static void
publish_input_failsafe (PmInput *self)
{
  guint8 payload[PM_FAILSAFE_INPUT_MAX];
  gsize off = 0;
  guint8 tmp[PM_CTRL_MSG_MAX];
  gsize n;

  if (self->pressed) {
    guint64 pid = self->mouse_mode ? SCRCPY_POINTER_ID_MOUSE
                                   : SCRCPY_POINTER_ID_GENERIC_FINGER;
    n = pm_protocol_write_touch (tmp, PM_ACTION_UP, pid,
                                 self->touch_x, self->touch_y,
                                 self->stream.width, self->stream.height,
                                 0.0, PM_BUTTON_NONE, 0);
    if (off + n <= sizeof payload) { memcpy (payload + off, tmp, n); off += n; }
  }

  for (guint i = 0; i < self->n_held_keys; i++) {
    n = pm_protocol_write_key (tmp, PM_KEY_UP, self->held_keys[i], 0);
    if (off + n <= sizeof payload) { memcpy (payload + off, tmp, n); off += n; }
  }

  pm_failsafe_set_input (payload, off);
}

/* Track a navigation keycode as held (down) or no longer held (up), so the
 * fail-safe payload covers it. Deduplicated and bounded; printable keys (sent as
 * text, ak == 0) are never tracked. */
static void
note_key_state (PmInput *self, guint32 ak, gboolean down)
{
  if (ak == 0)
    return;

  guint i;
  for (i = 0; i < self->n_held_keys; i++)
    if (self->held_keys[i] == ak)
      break;

  if (down) {
    if (i == self->n_held_keys && self->n_held_keys < PM_MAX_HELD_KEYS)
      self->held_keys[self->n_held_keys++] = ak;
  } else if (i < self->n_held_keys) {
    self->held_keys[i] = self->held_keys[--self->n_held_keys];
  }
}

/* Low-level: emit one INJECT_TOUCH_EVENT at normalised coordinates. */
static void
send_pointer (PmInput *self, PmTouchAction action, guint64 pointer_id,
              double nx, double ny, double pressure,
              guint32 buttons, guint32 action_button)
{
  guint8 buf[PM_CTRL_MSG_MAX];
  gsize n = pm_protocol_write_touch (buf, action, pointer_id, nx, ny,
                                     self->stream.width, self->stream.height,
                                     pressure, buttons, action_button);
  send_msg (self, buf, n);
}

void
pm_input_release_all (PmInput *self)
{
  if (self == NULL)
    return;

  if (self->pressed) {
    guint64 pid = self->mouse_mode ? SCRCPY_POINTER_ID_MOUSE
                                   : SCRCPY_POINTER_ID_GENERIC_FINGER;
    send_pointer (self, PM_ACTION_UP, pid, self->touch_x, self->touch_y,
                  0.0, PM_BUTTON_NONE, 0);
    self->pressed = FALSE;
    self->buttons_state = 0;
  }

  while (self->n_held_keys > 0) {
    guint32 ak = self->held_keys[--self->n_held_keys];
    guint8 buf[PM_CTRL_MSG_MAX];
    gsize n = pm_protocol_write_key (buf, PM_KEY_UP, ak, 0);
    send_msg (self, buf, n);
  }

  /* The held state is now empty; keep the crash fail-safe's payload in sync. */
  publish_input_failsafe (self);
}

/* Map widget coords to device-normalised coords, caching them for fallbacks. */
static gboolean
map_coords (PmInput *self, double wx, double wy, gboolean clamp,
            double *nx, double *ny)
{
  gboolean ok = clamp
    ? pm_video_view_widget_to_device_clamped (self->view, wx, wy, nx, ny)
    : pm_video_view_widget_to_device (self->view, wx, wy, nx, ny);
  if (ok) {
    self->touch_x = *nx;
    self->touch_y = *ny;
  }
  return ok;
}

/* --- touch mode (single finger) ------------------------------------------- */

static gboolean
finger_send (PmInput *self, PmTouchAction action, double wx, double wy, gboolean clamp)
{
  double nx, ny;
  if (!map_coords (self, wx, wy, clamp, &nx, &ny))
    return FALSE;   /* outside the video content (letterbox) */

  gboolean down = (action != PM_ACTION_UP);
  send_pointer (self, action, SCRCPY_POINTER_ID_GENERIC_FINGER, nx, ny,
                down ? 1.0 : 0.0, down ? PM_BUTTON_PRIMARY : PM_BUTTON_NONE, 0);
  return TRUE;
}

static void
finger_release (PmInput *self, double x, double y)
{
  if (!self->pressed)
    return;

  if (!finger_send (self, PM_ACTION_UP, x, y, TRUE))
    send_pointer (self, PM_ACTION_UP, SCRCPY_POINTER_ID_GENERIC_FINGER,
                  self->touch_x, self->touch_y, 0.0, PM_BUTTON_NONE, 0);
  self->pressed = FALSE;
  publish_input_failsafe (self);
}

/* --- mouse mode (full buttons + hover) ------------------------------------ */

static gboolean
mouse_send (PmInput *self, PmTouchAction action, double wx, double wy,
            double pressure, guint32 buttons, guint32 action_button, gboolean clamp)
{
  double nx, ny;
  if (!map_coords (self, wx, wy, clamp, &nx, &ny))
    return FALSE;

  send_pointer (self, action, SCRCPY_POINTER_ID_MOUSE, nx, ny,
                pressure, buttons, action_button);
  return TRUE;
}

/* A mouse button went down or up. DOWN is sent for the first button, UP when
 * the last is released, and MOVE while other buttons remain held; action_button
 * names the changed button so the server can synthesise the matching
 * ACTION_BUTTON_PRESS / ACTION_BUTTON_RELEASE. */
static void
mouse_button_changed (PmInput *self, guint32 button, gboolean down, double x, double y)
{
  if (button == PM_BUTTON_NONE)
    return;

  guint32 prev = self->buttons_state;
  if (down)
    self->buttons_state |= button;
  else
    self->buttons_state &= ~button;

  PmTouchAction action;
  if (down)
    action = (prev == 0) ? PM_ACTION_DOWN : PM_ACTION_MOVE;
  else
    action = (self->buttons_state == 0) ? PM_ACTION_UP : PM_ACTION_MOVE;

  self->pressed = (self->buttons_state != 0);
  mouse_send (self, action, x, y, self->pressed ? 1.0 : 0.0,
              self->buttons_state, button, TRUE);
  publish_input_failsafe (self);
}

void
pm_input_set_mouse_mode (PmInput *self, gboolean mouse_mode)
{
  g_return_if_fail (self != NULL);
  if (self->mouse_mode == mouse_mode)
    return;

  /* Cleanly retire whatever pointer the old mode left on the device, otherwise
   * the new mode's events are misread as a second pointer and ignored.
   *
   * Leaving mouse mode requires a real ACTION_UP (not just HOVER_EXIT): the
   * server frees a pointer slot only on UP, so a hovering mouse otherwise keeps
   * slot 0 and the next finger lands in slot 1 - a non-primary pointer whose
   * ACTION_DOWN the device drops. */
  if (self->view != NULL) {
    if (self->mouse_mode)
      mouse_send (self, PM_ACTION_UP, self->last_x, self->last_y, 0.0, 0, 0, TRUE);
    else
      finger_release (self, self->last_x, self->last_y);
  }

  self->buttons_state = 0;
  self->pressed = FALSE;
  self->mouse_mode = mouse_mode;
  publish_input_failsafe (self);
}

/* --- pointer handlers ----------------------------------------------------- */

static void
on_pressed (GtkGestureClick *g, int n_press, double x, double y, gpointer ud)
{
  PmInput *self = ud;
  guint gdk_button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (g));
  self->last_x = x;
  self->last_y = y;
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  wake_on_input (self);

  if (self->mouse_mode) {
    mouse_button_changed (self, gdk_button_to_android (gdk_button), TRUE, x, y);
  } else if (gdk_button == GDK_BUTTON_PRIMARY) {
    self->pressed = finger_send (self, PM_ACTION_DOWN, x, y, FALSE);
    publish_input_failsafe (self);
  }
}

static void
on_released (GtkGestureClick *g, int n_press, double x, double y, gpointer ud)
{
  PmInput *self = ud;
  guint gdk_button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (g));
  self->last_x = x;
  self->last_y = y;

  if (self->mouse_mode)
    mouse_button_changed (self, gdk_button_to_android (gdk_button), FALSE, x, y);
  else if (gdk_button == GDK_BUTTON_PRIMARY)
    finger_release (self, x, y);
}

static void
on_motion_enter (GtkEventControllerMotion *m, double x, double y, gpointer ud)
{
  PmInput *self = ud;
  self->last_x = x;
  self->last_y = y;
  if (self->mouse_mode && self->buttons_state == 0)
    mouse_send (self, PM_ACTION_HOVER_ENTER, x, y, 0.0, 0, 0, FALSE);
}

static void
on_motion (GtkEventControllerMotion *m, double x, double y, gpointer ud)
{
  PmInput *self = ud;
  self->last_x = x;
  self->last_y = y;

  if (self->mouse_mode) {
    if (self->buttons_state != 0) {
      mouse_send (self, PM_ACTION_MOVE, x, y, 1.0, self->buttons_state, 0, TRUE);
      publish_input_failsafe (self);   /* keep the release position current */
    } else {
      mouse_send (self, PM_ACTION_HOVER_MOVE, x, y, 0.0, 0, 0, FALSE);
    }
  } else if (self->pressed) {
    if (!finger_send (self, PM_ACTION_MOVE, x, y, FALSE))
      finger_release (self, x, y);
    else
      publish_input_failsafe (self);   /* keep the release position current */
  }
}

static void
on_motion_leave (GtkEventControllerMotion *m, gpointer ud)
{
  PmInput *self = ud;
  if (self->mouse_mode) {
    /* A drag (button held) keeps its grab and continues via motion; only the
     * idle cursor "leaves". */
    if (self->buttons_state == 0)
      mouse_send (self, PM_ACTION_HOVER_EXIT, self->last_x, self->last_y, 0.0, 0, 0, TRUE);
  } else {
    finger_release (self, self->last_x, self->last_y);
  }
}

static gboolean
on_scroll (GtkEventControllerScroll *s, double dx, double dy, gpointer ud)
{
  PmInput *self = ud;
  wake_on_input (self);
  double nx, ny;
  if (!pm_video_view_widget_to_device (self->view, self->last_x, self->last_y, &nx, &ny))
    return GDK_EVENT_PROPAGATE;

  guint8 buf[PM_CTRL_MSG_MAX];
  /* GTK scroll deltas are "lines"; invert to match natural touch scrolling. */
  gsize n = pm_protocol_write_scroll (buf, nx, ny,
                                      self->stream.width, self->stream.height,
                                      -dx, -dy);
  send_msg (self, buf, n);
  return GDK_EVENT_STOP;
}

/* --- keyboard ------------------------------------------------------------- */

static guint32
keyval_to_android (guint keyval)
{
  switch (keyval) {
    case GDK_KEY_BackSpace: return AKEYCODE_DEL;
    case GDK_KEY_Delete:    return AKEYCODE_FORWARD_DEL;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:  return AKEYCODE_ENTER;
    case GDK_KEY_Up:        return AKEYCODE_DPAD_UP;
    case GDK_KEY_Down:      return AKEYCODE_DPAD_DOWN;
    case GDK_KEY_Left:      return AKEYCODE_DPAD_LEFT;
    case GDK_KEY_Right:     return AKEYCODE_DPAD_RIGHT;
    case GDK_KEY_Escape:    return AKEYCODE_BACK;
    case GDK_KEY_Home:      return AKEYCODE_HOME;
    default:                return 0;   /* TODO: route printable keys via INJECT_TEXT */
  }
}

static void
send_key (PmInput *self, PmKeyAction action, guint keyval)
{
  guint32 ak = keyval_to_android (keyval);
  if (ak == 0)
    return;
  guint8 buf[PM_CTRL_MSG_MAX];
  gsize n = pm_protocol_write_key (buf, action, ak, /*meta*/ 0);
  send_msg (self, buf, n);
  /* Track the held state so the fail-safe can release it on a crash. */
  note_key_state (self, ak, action == PM_KEY_DOWN);
  publish_input_failsafe (self);
}

static gboolean
on_key_pressed (GtkEventControllerKey *k, guint keyval, guint code,
                GdkModifierType state, gpointer ud)
{
  PmInput *self = ud;
  wake_on_input (self);

  /* Printable character with no Ctrl/Alt/Super modifier → inject as UTF-8 text
   * (lets the device IME handle composition, capitalisation, etc.). */
  if (!(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
    gunichar uc = gdk_keyval_to_unicode (keyval);
    if (uc != 0 && g_unichar_isprint (uc)) {
      char utf8[8];
      int n = g_unichar_to_utf8 (uc, utf8);
      utf8[n] = '\0';

      guint8 buf[PM_CTRL_MSG_MAX];
      gsize len = pm_protocol_write_text (buf, sizeof buf, utf8);
      if (len > 0) {
        send_msg (self, buf, len);
        return GDK_EVENT_STOP;
      }
    }
  }

  /* Otherwise treat it as a navigation/editing keycode. */
  send_key (self, PM_KEY_DOWN, keyval);
  return GDK_EVENT_PROPAGATE;
}

static void
on_key_released (GtkEventControllerKey *k, guint keyval, guint code,
                 GdkModifierType state, gpointer ud)
{
  send_key (ud, PM_KEY_UP, keyval);
}

void
pm_input_attach (PmInput *self, PmVideoView *view)
{
  g_return_if_fail (self != NULL && PM_IS_VIDEO_VIEW (view));
  self->view = view;
  GtkWidget *w = GTK_WIDGET (view);
  g_message ("input: attaching controls for stream %ux%u",
             self->stream.width, self->stream.height);

  /* One gesture per mouse button, so buttons are recognised independently and can be
   * held at the same time (chording, drag-with-modifier-button, etc.). */
  for (guint i = 0; i < PM_N_BUTTONS; i++) {
    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), pm_button_map[i].gdk);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                                GTK_PHASE_CAPTURE);
    g_signal_connect (click, "pressed",  G_CALLBACK (on_pressed),  self);
    g_signal_connect (click, "released", G_CALLBACK (on_released), self);
    self->clicks[i] = GTK_EVENT_CONTROLLER (click);
    gtk_widget_add_controller (w, self->clicks[i]);
  }

  self->motion = gtk_event_controller_motion_new ();
  gtk_event_controller_set_propagation_phase (self->motion, GTK_PHASE_CAPTURE);
  g_signal_connect (self->motion, "enter", G_CALLBACK (on_motion_enter), self);
  g_signal_connect (self->motion, "motion", G_CALLBACK (on_motion), self);
  g_signal_connect (self->motion, "leave", G_CALLBACK (on_motion_leave), self);
  gtk_widget_add_controller (w, self->motion);

  self->scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  gtk_event_controller_set_propagation_phase (self->scroll, GTK_PHASE_CAPTURE);
  g_signal_connect (self->scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (w, self->scroll);

  /* The view must be focusable to receive key events. */
  gtk_widget_set_focusable (w, TRUE);
  self->key = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (self->key, GTK_PHASE_CAPTURE);
  g_signal_connect (self->key, "key-pressed",  G_CALLBACK (on_key_pressed),  self);
  g_signal_connect (self->key, "key-released", G_CALLBACK (on_key_released), self);
  gtk_widget_add_controller (w, self->key);
}

void
pm_input_clear_net (PmInput *self)
{
  if (self == NULL)
    return;
  /* The session frees the control socket on a spontaneous disconnect (worker
   * thread), so the borrowed pointer here can dangle. NULL it; send_msg already
   * no-ops on a NULL net, so every later send (including teardown releases)
   * becomes a safe no-op instead of a write through freed memory. */
  self->net = NULL;
}

void
pm_input_free (PmInput *self)
{
  if (self == NULL)
    return;

  if (self->view != NULL) {
    /* Lift any pointer still down so the device doesn't get a stuck press. */
    if (self->mouse_mode) {
      if (self->buttons_state != 0) {
        mouse_send (self, PM_ACTION_UP, self->last_x, self->last_y, 0.0, 0, 0, TRUE);
        self->buttons_state = 0;
        self->pressed = FALSE;
      }
    } else {
      finger_release (self, self->last_x, self->last_y);
    }

    GtkWidget *w = GTK_WIDGET (self->view);
    for (guint i = 0; i < PM_N_BUTTONS; i++)
      if (self->clicks[i] != NULL)
        gtk_widget_remove_controller (w, self->clicks[i]);
    if (self->motion != NULL)
      gtk_widget_remove_controller (w, self->motion);
    if (self->scroll != NULL)
      gtk_widget_remove_controller (w, self->scroll);
    if (self->key != NULL)
      gtk_widget_remove_controller (w, self->key);
  }

  /* No pointer/keys to undo once input is gone; the session also disarms the
   * fail-safe fd around teardown, but clear the payload here too. */
  pm_failsafe_set_input (NULL, 0);

  g_free (self->serial);
  g_free (self);
}
