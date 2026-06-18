/* protocol.h - control-message serialization (scrcpy-compatible wire format).
 *
 * Messages are sent client -> server over the control socket. The encoding
 * mirrors scrcpy's: a 1-byte type tag followed by a fixed, big-endian payload.
 * NOTE: the exact layout is tied to the scrcpy-server protocol *version*;
 * keep this in lockstep with the bundled server jar (see README).
 */
#pragma once

#include "pm-types.h"

G_BEGIN_DECLS

typedef enum {
  PM_CTRL_INJECT_KEYCODE      = 0,
  PM_CTRL_INJECT_TEXT         = 1,
  PM_CTRL_INJECT_TOUCH_EVENT  = 2,
  PM_CTRL_INJECT_SCROLL_EVENT = 3,
  PM_CTRL_BACK_OR_SCREEN_ON   = 4,
  PM_CTRL_SET_CLIPBOARD       = 9,
  PM_CTRL_SET_DISPLAY_POWER   = 10,
} PmCtrlType;

/* Android MotionEvent actions used by the control socket. */
typedef enum {
  PM_ACTION_DOWN        = 0,
  PM_ACTION_UP          = 1,
  PM_ACTION_MOVE        = 2,
  PM_ACTION_HOVER_MOVE  = 7,   /* mouse moved with no button pressed */
  PM_ACTION_HOVER_ENTER = 9,
  PM_ACTION_HOVER_EXIT  = 10,
} PmTouchAction;

/* AMOTION_EVENT_BUTTON_* bits, for the touch message's button state. */
typedef enum {
  PM_BUTTON_NONE      = 0,
  PM_BUTTON_PRIMARY   = 1 << 0,
  PM_BUTTON_SECONDARY = 1 << 1,
  PM_BUTTON_TERTIARY  = 1 << 2,
  PM_BUTTON_BACK      = 1 << 3,
  PM_BUTTON_FORWARD   = 1 << 4,
} PmButton;

/* Android KeyEvent actions. */
typedef enum {
  PM_KEY_DOWN = 0,
  PM_KEY_UP   = 1,
} PmKeyAction;

/* Android KEYCODE_POWER. Toggles the screen/power state. */
#define PM_ANDROID_KEYCODE_POWER 26

/* Android KEYCODE_SLEEP. Unconditionally sleeps the device (never wakes it),
 * so it locks the phone on teardown regardless of the current screen state.
 * Preferred over POWER's toggle, which races a just-restored panel on slower
 * devices and can wake a still-dark screen instead of locking it. */
#define PM_ANDROID_KEYCODE_SLEEP 223

/* Largest emitted control message; callers can size buffers with this. */
#define PM_CTRL_MSG_MAX 512

/* Encode an absolute touch event. (norm_x, norm_y) are in [0,1] over the
 * video content; (dev_w, dev_h) is the current device frame size used by the
 * server to reconstruct absolute pixels. Returns bytes written into `buf`
 * (which must hold >= PM_CTRL_MSG_MAX). */
gsize pm_protocol_write_touch (guint8        *buf,
                               PmTouchAction  action,
                               guint64        pointer_id,
                               double         norm_x,
                               double         norm_y,
                               guint16        dev_w,
                               guint16        dev_h,
                               double         pressure,
                               guint32        buttons,
                               guint32        action_button);

/* Encode a key event (Android keycode + meta-state bitmask). */
gsize pm_protocol_write_key (guint8      *buf,
                             PmKeyAction  action,
                             guint32      android_keycode,
                             guint32      meta_state);

/* Encode a UTF-8 text injection. Returns bytes written, or 0 if `utf8` is empty
 * or does not fit in `cap` bytes. */
gsize pm_protocol_write_text (guint8     *buf,
                              gsize       cap,
                              const char *utf8);

/* Encode a display-power message: TRUE turns the device screen on, FALSE turns
 * it off (the panel goes dark to save battery while input still works). Returns
 * bytes written into `buf` (which must hold >= PM_CTRL_MSG_MAX). */
gsize pm_protocol_write_display_power (guint8 *buf, gboolean on);

/* Encode a scroll event at an absolute position. h/v are scroll deltas. */
gsize pm_protocol_write_scroll (guint8  *buf,
                                double   norm_x,
                                double   norm_y,
                                guint16  dev_w,
                                guint16  dev_h,
                                double   h_scroll,
                                double   v_scroll);

G_END_DECLS
