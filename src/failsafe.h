/* failsafe.h - crash fail-safe for device-side state.
 *
 * The app mutates state on the phone that does NOT clear itself if the process
 * dies abnormally: it can blank the panel (SET_DISPLAY_POWER off), hold a
 * pointer down mid-drag, or hold a navigation key down. Left behind, these
 * strand the phone - a stuck pointer in particular blocks back/home gestures.
 *
 * This module installs fatal-signal handlers (SIGSEGV/SIGABRT/…) that, before
 * the process dies, write a small set of pre-serialised control messages straight
 * to the control socket: release any held pointer/keys, restore the panel if it
 * was blanked, and then lock the phone with a KEYCODE_POWER press. The server
 * is no longer launched with power_off_on_close (it can't tell a reconnect from a
 * real disconnect), so the handler must do the locking itself; re-lighting the
 * panel first keeps that toggle locking a lit screen rather than waking a dark
 * one. Everything the handler touches is prepared ahead of time and published
 * through async-signal-safe primitives, so the handler itself only does
 * write()/raise().
 *
 * It is best-effort by nature: a SIGKILL or power loss cannot be caught, and a
 * network drop kills the socket so the lock never lands (the phone is then
 * unreachable anyway). It covers the common case of the app crashing while the
 * control socket is still alive.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Largest "undo input" payload the handler will flush (a pointer release plus a
 * handful of key releases). */
#define PM_FAILSAFE_INPUT_MAX 256

/* Install the fatal-signal handlers. Idempotent; call once at startup. */
void pm_failsafe_install (void);

/* Publish the control-socket fd the handler should write to, or -1 to disarm
 * (e.g. once the socket is torn down). Safe to call from any thread. */
void pm_failsafe_arm (int control_fd);

/* Record whether the panel is currently blanked by the app: when TRUE the
 * handler also emits SET_DISPLAY_POWER(on). Safe to call from any thread. */
void pm_failsafe_set_screen_blanked (gboolean blanked);

/* Publish the bytes that would return device input to a neutral state right now
 * (pointer/key releases), or len 0 to clear. `bytes` is copied. MUST be called
 * from a single thread (the UI thread, where input lives). */
void pm_failsafe_set_input (const guint8 *bytes, gsize len);

G_END_DECLS
