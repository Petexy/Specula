/* pm-session.h - the pipeline controller ("session").
 *
 * PmSession owns the end-to-end lifecycle and is the single object the UI
 * talks to. It hides the worker threads behind a simple state machine and one
 * signal:
 *
 *   "state-changed"  (guint state /PmState/, const char *message)
 *   "stream-changed" (guint width, guint height)
 *
 * It drives the sequence:
 *   discovery -> adb connect/push/forward -> spawn server -> net connect
 *   -> decoder open -> [frames] -> PmVideoView, with input flowing back.
 *
 * Threading: blocking work (network, adb, decode) runs on a worker thread;
 * state changes and decoded frames are marshalled to the GTK main thread.
 */
#pragma once

#include <glib-object.h>
#include "pm-types.h"
#include "pm-video-view.h"

G_BEGIN_DECLS

#define PM_TYPE_SESSION (pm_session_get_type ())
G_DECLARE_FINAL_TYPE (PmSession, pm_session, PM, SESSION, GObject)

PmSession *pm_session_new (void);

PmState pm_session_get_state (PmSession *self);

/* The adb endpoint ("host:port") of the live session, or NULL when not
 * mirroring. Used to key per-device data (e.g. the lockscreen PIN by MAC) while
 * a device is connected. Caller frees. */
char *pm_session_dup_serial (PmSession *self);

/* Copy the current negotiated stream geometry. Returns FALSE until the stream
 * metadata has been read. */
gboolean pm_session_get_stream_info (PmSession *self, PmStreamInfo *out);

/* The view to render into (borrowed; owned by the window). */
void pm_session_set_video_view (PmSession *self, PmVideoView *view);

/* Forward pointer input as a mouse (TRUE) or as touch (FALSE). Default TRUE.
 * Takes effect immediately if a session is live. */
void pm_session_set_mouse_mode (PmSession *self, gboolean mouse_mode);

/* Stream the phone's audio to the desktop's speakers/headphones (TRUE) or keep
 * the phone silent (FALSE). Default TRUE. Audio is negotiated with the on-device
 * server at connect time, so a change takes effect on the next connection. */
void pm_session_set_audio_enabled (PmSession *self, gboolean audio_enabled);

/* Requested video bitrate in Mbps; 0 lets the device use scrcpy's default
 * (8 Mbps). Like audio, the bitrate is negotiated with the on-device server at
 * connect time, so a change takes effect on the next connection - use
 * pm_session_reconnect() to apply it to a live session. */
void pm_session_set_video_bitrate (PmSession *self, guint mbps);

/* Mirror the phone's physical screen (PM_DISPLAY_MIRROR, default) or have the
 * on-device server create a separate virtual display (PM_DISPLAY_VIRTUAL). The
 * mode is negotiated with the server at connect time, so a change takes effect
 * on the next connection - use pm_session_reconnect() to apply it to a live
 * session. */
void pm_session_set_display_mode (PmSession *self, PmDisplayMode mode);

/* Geometry of the virtual display, used only when the mode is PM_DISPLAY_VIRTUAL.
 * A dpi of 0 lets the device pick its default density. Like the mode itself,
 * this is negotiated at connect time, so use pm_session_reconnect() to apply it
 * to a live session. */
void pm_session_set_virtual_display (PmSession *self,
                                     guint      width,
                                     guint      height,
                                     guint      dpi);

/* Blank the phone's physical screen while mirroring (TRUE, default) to save its
 * battery - input still works and the stream keeps flowing; the panel just goes
 * dark. The screen is restored when the session ends. The state is applied at
 * connect time, so a change takes effect on the next connection - use
 * pm_session_reconnect() to apply it to a live session. */
void pm_session_set_screen_off (PmSession *self, gboolean screen_off);

/* Re-establish a live (or in-progress) session to the same device using the
 * current settings, skipping discovery so the switch is seamless. No-op unless
 * a session is connecting/mirroring with a known endpoint. */
void pm_session_reconnect (PmSession *self);

/* Begin a session. `target` may be NULL to use the saved pairing (device.c).
 * Normal sessions run the real adb/decode pipeline. Set PM_DEMO=1 to exercise
 * the UI with a synthetic test pattern when no phone is available. */
void pm_session_start (PmSession *self, const PmDeviceInfo *target);

/* Like pm_session_start(), but startup/autoconnect failures return to IDLE
 * without surfacing an error message to the UI. */
void pm_session_start_silent (PmSession *self, const PmDeviceInfo *target);

/* Tear everything down and return to IDLE. */
void pm_session_stop (PmSession *self);

G_END_DECLS
