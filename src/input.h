/* input.h - translate GTK input events into control messages.
 *
 * Attaches GTK event controllers to the PmVideoView and converts pointer /
 * keyboard activity into scrcpy control messages, which are written to the
 * control socket. Coordinate mapping goes through
 * pm_video_view_widget_to_device(), so it is correct under Wayland fractional
 * scaling and letterboxing (the widget works in logical pixels; normalised
 * coordinates are scaled to device pixels by the server).
 */
#pragma once

#include "pm-types.h"
#include "net.h"
#include "pm-video-view.h"

G_BEGIN_DECLS

typedef struct _PmInput PmInput;

/* `control_net` is borrowed (owned by PmSession) and must outlive the input.
 * `serial` is the adb device serial (host:port) used to wake the screen on
 * input; pass NULL to disable wake-on-input (e.g. demo mode). Copied.
 * `wake_suppressed` is a borrowed pointer to an atomic flag (owned by the
 * session): while it reads non-zero, wake-on-input is held off because the
 * session deliberately blanked the panel and a wake would undo it. The screen
 * stays controllable regardless - only the KEYCODE_WAKEUP is skipped. Pass NULL
 * to never suppress. */
PmInput *pm_input_new (PmNet              *control_net,
                       const PmStreamInfo *stream,
                       const char         *serial,
                       const gint         *wake_suppressed);

/* Install event controllers on the video view. */
void pm_input_attach (PmInput *self, PmVideoView *view);

/* Update the device frame size after a rotation/resolution change. */
void pm_input_set_stream (PmInput *self, const PmStreamInfo *stream);

/* Choose how pointer input is forwarded: TRUE injects as a mouse
 * (SOURCE_MOUSE), FALSE as a touchscreen (SOURCE_TOUCHSCREEN). Default TRUE. */
void pm_input_set_mouse_mode (PmInput *self, gboolean mouse_mode);

/* Send releases for everything currently held on the device (a pressed pointer
 * and any held navigation keys) over the control socket, and clear that state.
 * Call on a clean teardown, while the control socket is still open, so a session
 * that ends mid-press cannot strand a finger or key on the phone. UI thread. */
void pm_input_release_all (PmInput *self);

void pm_input_free (PmInput *self);

G_END_DECLS
