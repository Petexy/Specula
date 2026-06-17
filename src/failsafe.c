/* failsafe.c - crash fail-safe implementation. */
#include "failsafe.h"
#include "protocol.h"

#include <string.h>
#include <signal.h>
#include <unistd.h>

/* The control socket fd to flush to, or -1 when disarmed. */
static volatile sig_atomic_t s_fd = -1;

/* Non-zero while the panel has been deliberately blanked, so the handler knows
 * to turn it back on. */
static volatile sig_atomic_t s_blanked = 0;

/* "Undo input" payload, double-buffered so the (single) writer can update it
 * without the signal handler ever observing a half-written buffer: the writer
 * fills the inactive buffer, then publishes it by flipping s_input_idx last. The
 * handler reads s_input_idx once and uses that buffer, which the writer will not
 * touch again until the following update. */
static guint8 s_input_buf[2][PM_FAILSAFE_INPUT_MAX];
static gsize  s_input_len[2];
static volatile sig_atomic_t s_input_idx = 0;

static void
failsafe_handler (int signum)
{
  int fd = s_fd;
  if (fd >= 0) {
    /* Release any held pointer/keys first so a stuck finger can never outlive
     * the process - otherwise it strands the phone's back/home gestures until a
     * reboot. */
    int i = s_input_idx;
    gsize len = s_input_len[i];
    if (len > 0) {
      ssize_t r = write (fd, s_input_buf[i], len);
      (void) r;   /* best-effort */
    }
    if (s_blanked) {
      /* Re-light the panel first so the power-key press below toggles a lit,
       * interactive screen *off* (locking the phone) rather than waking a dark
       * one. */
      static const unsigned char on_msg[2] = { PM_CTRL_SET_DISPLAY_POWER, 1 };
      ssize_t r = write (fd, on_msg, sizeof on_msg);
      (void) r;
    }
    /* Lock the phone. The server is no longer launched with power_off_on_close
     * (it can't tell a reconnect from a real disconnect), so it won't lock the
     * device; inject KEYCODE_POWER (press then release) instead. These mirror
     * pm_protocol_write_key's wire format: type(0=INJECT_KEYCODE), action, the
     * big-endian keycode 26 (KEYCODE_POWER), then repeat(0) and meta-state(0). */
    {
      static const unsigned char power_down[14] =
        { PM_CTRL_INJECT_KEYCODE, 0, 0, 0, 0, 26, 0, 0, 0, 0, 0, 0, 0, 0 };
      static const unsigned char power_up[14] =
        { PM_CTRL_INJECT_KEYCODE, 1, 0, 0, 0, 26, 0, 0, 0, 0, 0, 0, 0, 0 };
      ssize_t r = write (fd, power_down, sizeof power_down);
      (void) r;
      r = write (fd, power_up, sizeof power_up);
      (void) r;
    }
  }

  /* Restore the default disposition and re-raise so the original crash still
   * surfaces (core dump / termination); it is never masked. */
  signal (signum, SIG_DFL);
  raise (signum);
}

void
pm_failsafe_install (void)
{
  static gsize once = 0;
  if (!g_once_init_enter (&once))
    return;

  struct sigaction sa;
  memset (&sa, 0, sizeof sa);
  sa.sa_handler = failsafe_handler;
  sigemptyset (&sa.sa_mask);
  sa.sa_flags = 0;   /* the handler resets to SIG_DFL itself before re-raising */

  const int fatal[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
  for (gsize i = 0; i < G_N_ELEMENTS (fatal); i++)
    sigaction (fatal[i], &sa, NULL);

  g_once_init_leave (&once, 1);
}

void
pm_failsafe_arm (int control_fd)
{
  s_fd = control_fd;
}

void
pm_failsafe_set_screen_blanked (gboolean blanked)
{
  s_blanked = blanked ? 1 : 0;
}

void
pm_failsafe_set_input (const guint8 *bytes, gsize len)
{
  if (len > PM_FAILSAFE_INPUT_MAX)
    len = 0;   /* never publish a truncated message sequence */

  int w = 1 - s_input_idx;        /* the buffer the handler is not using */
  if (len > 0 && bytes != NULL)
    memcpy (s_input_buf[w], bytes, len);
  s_input_len[w] = len;
  s_input_idx = w;                /* publish last */
}
