/* pm-window.c - top-level window and view orchestration.
 *
 * The window is intentionally thin: it owns a PmSession (the pipeline
 * controller) and an AdwViewStack, and it reacts to PmSession::state-changed
 * by flipping the visible page and updating status text. All the heavy
 * lifting (discovery, adb, decode, render) lives behind PmSession.
 *
 * UI states (AdwViewStack pages):
 *   "searching"  -> AdwStatusPage with a spinner ("Looking for your phone…")
 *   "mirror"     -> PmVideoView (live stream)
 *
 * The UI is built in C rather than from a GtkBuilder template so the runtime
 * layout remains explicit in this file.
 */
#include "pm-config.h"
#include "pm-window.h"
#include "pm-session.h"
#include "pm-video-view.h"
#include "pm-connect-dialog.h"
#include "pm-settings-dialog.h"
#include "device.h"
#include "prefs.h"
#include "pinstore.h"
#include "adb.h"

#include <math.h>
#include <string.h>
#include <glib/gi18n.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#define PM_MIRROR_TOP_REVEAL_PX 4
#define PM_MIRROR_TOP_HIDE_PX   72
#define PM_MIRROR_MIN_WIDTH     260
#define PM_MIRROR_MIN_HEIGHT    360
#define PM_MIRROR_MIN_SHORT_EDGE 300
#define PM_MIRROR_SCREEN_MARGIN 96
#define PM_MIRROR_SCREEN_FRACTION 0.70
/* Width (px) of the edge band that initiates a client-driven aspect-locked
 * resize while mirroring. See the "live aspect-locked resize" block below. */
#define PM_RESIZE_GRAB_PX       8

/* Which window edge a resize drag is anchored to. Only the edges that keep the
 * window's top-left corner fixed are honored (RIGHT, BOTTOM, and the
 * bottom-right corner): Wayland gives clients no way to reposition their own
 * surface, so a left/top grab could only grow the window away from the pointer,
 * which feels broken. The window is moved from the header bar instead. */
typedef enum {
  PM_EDGE_NONE         = 0,
  PM_EDGE_RIGHT        = 1 << 0,
  PM_EDGE_BOTTOM       = 1 << 1,
  PM_EDGE_BOTTOM_RIGHT = PM_EDGE_RIGHT | PM_EDGE_BOTTOM,
} PmResizeEdge;

typedef enum {
  PM_SETUP_WELCOME,
  PM_SETUP_SETTINGS,
  PM_SETUP_ABOUT,
  PM_SETUP_BUILD_NUMBER,
  PM_SETUP_USB_DEBUGGING,
  PM_SETUP_WIRELESS_DEBUGGING,
  PM_SETUP_PAIRING,
  PM_SETUP_N_STEPS
} PmSetupStep;

static const char * const pm_setup_page_names[] = {
  [PM_SETUP_WELCOME]            = "setup-welcome",
  [PM_SETUP_SETTINGS]           = "setup-settings",
  [PM_SETUP_ABOUT]              = "setup-about",
  [PM_SETUP_BUILD_NUMBER]       = "setup-build-number",
  [PM_SETUP_USB_DEBUGGING]      = "setup-usb-debugging",
  [PM_SETUP_WIRELESS_DEBUGGING] = "setup-wireless-debugging",
  [PM_SETUP_PAIRING]            = "setup-pairing",
};

typedef struct {
  PmSetupStep step;
  gint64      start_us;
} PmSetupAnimation;

static void pm_window_save_prefs (PmWindow *self);
static void pm_window_show_setup_step (PmWindow *self, guint step);
static void pm_window_set_resize_gesture_active (PmWindow *self, gboolean active);

struct _PmWindow {
  AdwApplicationWindow parent_instance;

  /* Controller */
  PmSession   *session;

  /* Runtime-owned widgets */
  AdwToolbarView *toolbar_view;
  AdwHeaderBar   *header_bar;
  AdwViewStack   *stack;
  AdwWindowTitle *title;
  GtkSpinner     *spinner;
  AdwStatusPage  *status_page;
  GtkAspectFrame *mirror_frame;
  PmVideoView    *video_view;
  GtkButton      *connect_button;
  GtkToggleButton *pin_button;
  GtkImage       *pin_image;
  GtkWidget      *setup_animation_areas[PM_SETUP_N_STEPS];
  GtkStack       *setup_anim_stack;        /* instant swap, mirrors the step */
  GtkStack       *setup_instruction_stack; /* slides left/right between steps */
  GtkWidget      *setup_back_button;       /* hidden on the welcome step */
  GtkWidget      *setup_next_button;       /* label flips on the final step */
  gboolean        setup_complete;
  gboolean        setup_review_active;
  guint           setup_step;
  gboolean        mirror_chrome_pinned;
  gboolean        free_resize;
  gboolean        mouse_mode;
  gboolean        audio;
  guint           video_bitrate;   /* video bitrate in Mbps (scrcpy default 8) */
  gboolean        bitrate_reconnect_pending; /* bitrate changed in the open settings sheet */
  PmDisplayMode   display_mode;
  guint           display_width;
  guint           display_height;
  guint           display_dpi;
  gboolean        screen_off;      /* blank the phone panel while mirroring */
  gboolean        startup_search_active;
  gboolean        defer_initial_present;
  /* Aspect-ratio lock: the last settled window size, plus a re-entry guard so
   * the lock's own corrections don't re-trigger the resize handler. */
  gboolean        aspect_lock_applying;
  int             aspect_locked_w;
  int             aspect_locked_h;
  /* Live aspect-locked resize: the window is non-resizable while locked, so the
   * compositor never free-resizes it; the size is driven directly from an
   * edge-drag gesture. These hold the in-flight drag's anchor edge and the
   * window size when the drag began. */
  PmResizeEdge    resize_edge;
  int             resize_start_w;
  int             resize_start_h;
  GtkEventController *resize_drag;
  GtkEventController *resize_motion;
  /* Header reveal: the window grows/shrinks in lockstep with the bar's reveal
   * animation so the video keeps its exact size - only the window makes room. */
  gboolean        chrome_animating;
  gboolean        chrome_target_revealed;
  int             chrome_video_w;
  int             chrome_video_h;
  int             chrome_frames;
  guint           chrome_tick;
};

G_DEFINE_FINAL_TYPE (PmWindow, pm_window, ADW_TYPE_APPLICATION_WINDOW)

static gboolean
desktop_name_contains (const char *needle)
{
  const char *desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  if (desktop == NULL || *desktop == '\0')
    desktop = g_getenv ("DESKTOP_SESSION");
  if (desktop == NULL || *desktop == '\0')
    return FALSE;

  g_autofree char *lower_desktop = g_ascii_strdown (desktop, -1);
  return strstr (lower_desktop, needle) != NULL;
}

static const char *
pm_window_first_available_icon (PmWindow           *self,
                                const char * const *names)
{
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
  GtkIconTheme *theme = display != NULL ? gtk_icon_theme_get_for_display (display) : NULL;

  for (guint i = 0; names[i] != NULL; i++) {
    if (theme == NULL || gtk_icon_theme_has_icon (theme, names[i]))
      return names[i];
  }

  return names[0];
}

static const char *
pm_window_get_pin_icon_name (PmWindow *self, gboolean pinned)
{
  static const char * const plasma_pin_icons[] = {
    "view-pin-symbolic",
    "window-pin-symbolic",
    "window-pin",
    "pin-symbolic",
    "pin",
    NULL
  };
  static const char * const plasma_unpin_icons[] = {
    "window-unpin-symbolic",
    "window-unpin",
    "window-pin-symbolic",
    "window-pin",
    "unpin-symbolic",
    "unpin",
    "view-pin-symbolic",
    NULL
  };
  static const char * const gnome_pin_icons[] = {
    "view-pin-symbolic",
    "pin-symbolic",
    "window-pin-symbolic",
    NULL
  };
  static const char * const gnome_unpin_icons[] = {
    "view-pin-symbolic",
    "unpin-symbolic",
    "window-unpin-symbolic",
    NULL
  };
  static const char * const generic_pin_icons[] = {
    "view-pin-symbolic",
    "window-pin-symbolic",
    "window-pin",
    "pin",
    "pin-symbolic",
    NULL
  };
  static const char * const generic_unpin_icons[] = {
    "view-pin-symbolic",
    "window-unpin",
    "window-unpin-symbolic",
    "window-pin",
    "window-pin-symbolic",
    "unpin",
    "unpin-symbolic",
    NULL
  };

  if (desktop_name_contains ("kde") || desktop_name_contains ("plasma"))
    return pm_window_first_available_icon (self,
                                           pinned ? plasma_unpin_icons : plasma_pin_icons);

  if (desktop_name_contains ("gnome"))
    return pm_window_first_available_icon (self,
                                           pinned ? gnome_unpin_icons : gnome_pin_icons);

  return pm_window_first_available_icon (self,
                                         pinned ? generic_unpin_icons : generic_pin_icons);
}

/* Widen the window's resize grab area. The mirror window is undecorated, so the
 * native resize handles are otherwise barely a pixel; a small decoration margin
 * gives the resize edges some extra room to grab. */
static void
pm_window_install_resize_css (PmWindow *self)
{
  static gboolean installed;

  if (installed)
    return;

  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
  if (display == NULL)
    return;

  GtkCssProvider *provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider,
                                     "window.specula-mirror decoration {"
                                     "  margin: 2px;"
                                     "}");
  gtk_style_context_add_provider_for_display (display,
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);

  installed = TRUE;
}

static void
pm_window_install_pin_css (PmWindow *self)
{
  static gboolean installed;

  if (installed)
    return;

  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
  if (display == NULL)
    return;

  GtkCssProvider *provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider,
                                     "button.specula-pin-button image {"
                                     "  color: @theme_fg_color;"
                                     "  -gtk-icon-palette: success @theme_fg_color,"
                                     "                     warning @theme_fg_color,"
                                     "                     error @theme_fg_color;"
                                     "}");
  gtk_style_context_add_provider_for_display (display,
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);

  installed = TRUE;
}

static void
pm_window_install_setup_css (PmWindow *self)
{
  static gboolean installed;

  if (installed)
    return;

  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
  if (display == NULL)
    return;

  GtkCssProvider *provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider,
                                     ".specula-setup-card {"
                                     "  padding: 12px 18px;"
                                     "}"
                                     ".specula-setup-step {"
                                     "  color: @accent_color;"
                                     "  font-weight: 700;"
                                     "}"
                                     ".specula-setup-title {"
                                     "  font-size: 22px;"
                                     "  font-weight: 800;"
                                     "}"
                                     ".specula-setup-body {"
                                     "  font-size: 14px;"
                                     "}"
                                     ".specula-setup-animation {"
                                     "  min-width: 320px;"
                                     "  min-height: 292px;"
                                     "}");
  gtk_style_context_add_provider_for_display (display,
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);

  installed = TRUE;
}

static void
pm_window_update_pin_icon (PmWindow *self)
{
  if (self->pin_button == NULL)
    return;

  gtk_image_set_from_icon_name (self->pin_image,
                                pm_window_get_pin_icon_name (self,
                                                             self->mirror_chrome_pinned));
}

/* --- header auto-hide: lockstep window/bar animation ----------------------
 *
 * While mirroring, the header is revealed/hidden as the pointer nears the top.
 * The picture must keep its exact size (so it never scales or jumps) AND the
 * header must not cover it - which leaves growing/shrinking the *window* by the
 * header's height as the only option.
 *
 * The catch is doing that smoothly. Two tempting shortcuts both fail:
 *   - One-step resize (grow the window, then reveal the bar): the async window
 *     resize and the bar reveal happen on different clocks, so a strip of empty
 *     space flashes between them - and any missed shrink strands that height as
 *     a growing band at the bottom.
 *   - Overlaying the bar on the video (extend-content-to-top-edge): smooth, but
 *     it covers the interactive top of the phone screen.
 *
 * So the two are locked together: libadwaita animates the bar on its own clock,
 * and every frame pm_window_chrome_tick reads the bar's *current* height and
 * sets the window to video + that height. The content area is therefore exactly
 * the video size on every frame; only the window grows/shrinks to make room.
 * Do not "simplify" this back to a single resize step.
 */

/* The header bar's full height, measured regardless of how much of it is
 * currently revealed (used to reserve space and to detect a finished reveal). */
static int
pm_window_natural_top_bar_height (PmWindow *self)
{
  int min = 0;
  int nat = 0;
  gtk_widget_measure (GTK_WIDGET (self->header_bar),
                      GTK_ORIENTATION_VERTICAL,
                      -1,
                      &min, &nat, NULL, NULL);
  return MAX (min, nat);
}

static void
pm_window_cancel_chrome_tick (PmWindow *self)
{
  if (self->chrome_tick != 0) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->chrome_tick);
    self->chrome_tick = 0;
  }
}

/* Runs every frame while the header reveals or hides. libadwaita animates the
 * bar's height on its own clock; that height is sampled and the window resized to
 * video + bar, so the picture keeps its exact size and never gets covered; the
 * window simply grows or shrinks to make room. */
static gboolean
pm_window_chrome_tick (GtkWidget     *widget,
                       GdkFrameClock *clock,
                       gpointer       data)
{
  PmWindow *self = PM_WINDOW (widget);
  int top = adw_toolbar_view_get_top_bar_height (self->toolbar_view);
  int win_h = self->chrome_video_h + top;

  /* Programmatic resize that stays on the locked aspect; suppress the resize
   * handler's correction and record where it lands. */
  self->aspect_lock_applying = TRUE;
  gtk_window_set_default_size (GTK_WINDOW (self), self->chrome_video_w, win_h);
  self->aspect_locked_w = self->chrome_video_w;
  self->aspect_locked_h = win_h;
  self->aspect_lock_applying = FALSE;

  /* Stop once the bar has reached its end state; a frame budget guarantees this
   * finishes even if the settled height never lands exactly on the measurement. */
  gboolean settled = self->chrome_target_revealed
                       ? top >= pm_window_natural_top_bar_height (self)
                       : top <= 0;
  if (settled || ++self->chrome_frames >= 90) {
    self->chrome_frames = 0;
    self->chrome_tick = 0;
    self->chrome_animating = FALSE;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

/* Apply the reveal state immediately, with no window animation. Used on state
 * changes (entering/leaving mirroring) where the window is being resized anyway
 * and there is no settled video size to preserve. */
static void
pm_window_reset_mirror_chrome (PmWindow *self, gboolean reveal)
{
  pm_window_cancel_chrome_tick (self);
  self->chrome_animating = FALSE;
  adw_toolbar_view_set_reveal_top_bars (self->toolbar_view, reveal);
}

static void
pm_window_set_mirror_chrome (PmWindow *self, gboolean reveal)
{
  if (self->mirror_chrome_pinned)
    reveal = TRUE;

  /* The motion handler re-requests the same state on every pointer move; ignore
   * it once already in (or animating toward) that state, so the transition is
   * never restarted into a jitter. reveal-top-bars holds the target. */
  if (reveal == adw_toolbar_view_get_reveal_top_bars (self->toolbar_view))
    return;

  gboolean mirroring = (self->session != NULL &&
                        pm_session_get_state (self->session) == PM_STATE_MIRRORING);
  int cur_w = gtk_widget_get_width (GTK_WIDGET (self));
  int cur_h = gtk_widget_get_height (GTK_WIDGET (self));

  if (!mirroring || cur_w <= 0 || cur_h <= 0) {
    pm_window_reset_mirror_chrome (self, reveal);
    return;
  }

  /* Capture the video's current size (window minus whatever the bar occupies
   * right now, mid-animation included) so the tick can hold it constant while
   * the window grows or shrinks around it. */
  self->chrome_video_w = cur_w;
  self->chrome_video_h = cur_h - adw_toolbar_view_get_top_bar_height (self->toolbar_view);
  self->chrome_target_revealed = reveal;
  self->chrome_animating = TRUE;
  self->chrome_frames = 0;

  adw_toolbar_view_set_reveal_top_bars (self->toolbar_view, reveal);
  if (self->chrome_tick == 0)
    self->chrome_tick = gtk_widget_add_tick_callback (GTK_WIDGET (self),
                                                      pm_window_chrome_tick,
                                                      NULL, NULL);
}

static void
pm_window_set_mirror_chrome_pinned (PmWindow *self, gboolean pinned)
{
  self->mirror_chrome_pinned = pinned;
  if (self->pin_button != NULL &&
      gtk_toggle_button_get_active (self->pin_button) != pinned)
    gtk_toggle_button_set_active (self->pin_button, pinned);

  pm_window_update_pin_icon (self);
  pm_window_set_mirror_chrome (self, pinned);
}

static void
pm_window_get_aspect_min_size (double aspect, int *out_width, int *out_height)
{
  if (aspect >= 1.0) {
    *out_height = PM_MIRROR_MIN_SHORT_EDGE;
    *out_width = MAX (PM_MIRROR_MIN_WIDTH, (int) round (*out_height * aspect));
  } else {
    *out_width = PM_MIRROR_MIN_SHORT_EDGE;
    *out_height = MAX (PM_MIRROR_MIN_HEIGHT, (int) round (*out_width / aspect));
  }
}

static gboolean
pm_window_get_available_geometry (PmWindow *self, GdkRectangle *geometry)
{
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
  if (display == NULL)
    return FALSE;

  GdkMonitor *monitor = NULL;
  gboolean unref_monitor = FALSE;
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (self));
  if (surface != NULL)
    monitor = gdk_display_get_monitor_at_surface (display, surface);

  if (monitor == NULL) {
    GListModel *monitors = gdk_display_get_monitors (display);
    if (g_list_model_get_n_items (monitors) > 0) {
      monitor = g_list_model_get_item (monitors, 0);
      unref_monitor = TRUE;
    }
  }

  if (monitor == NULL)
    return FALSE;

  gdk_monitor_get_geometry (monitor, geometry);

#ifdef GDK_WINDOWING_X11
  if (GDK_IS_X11_DISPLAY (display) && GDK_IS_X11_MONITOR (monitor)) {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_x11_monitor_get_workarea (monitor, geometry);
    G_GNUC_END_IGNORE_DEPRECATIONS
  }
#endif

  if (unref_monitor)
    g_object_unref (monitor);

  return TRUE;
}

static int
pm_window_get_top_bar_min_width (PmWindow *self)
{
  int min = 0;
  int nat = 0;

  gtk_widget_measure (GTK_WIDGET (self->header_bar),
                      GTK_ORIENTATION_HORIZONTAL,
                      -1,
                      &min, &nat, NULL, NULL);
  return MAX (0, min);
}

static void
pm_window_get_window_max_size (PmWindow *self,
                               int       top_reserve,
                               gboolean  conservative,
                               int      *out_width,
                               int      *out_height)
{
  int max_w = 900;
  int max_h = 900;
  GdkRectangle geometry;

  if (pm_window_get_available_geometry (self, &geometry)) {
    int safe_w = geometry.width - PM_MIRROR_SCREEN_MARGIN;
    int safe_h = geometry.height - PM_MIRROR_SCREEN_MARGIN - top_reserve;
    max_w = safe_w;
    max_h = safe_h;

    if (conservative) {
      int conservative_w = (int) floor (geometry.width * PM_MIRROR_SCREEN_FRACTION);
      int conservative_h = (int) floor (geometry.height * PM_MIRROR_SCREEN_FRACTION);
      max_w = MIN (max_w, conservative_w);
      max_h = MIN (max_h, conservative_h);
    }
  }

  *out_width = MAX (1, max_w);
  *out_height = MAX (1, max_h);
}

static void
pm_window_set_mirror_content_size (PmWindow *self,
                                   int       width,
                                   int       height)
{
  /* Drive the window's size, not a hard size request: the frame's minimum is
   * set separately (by pm_window_update_stream_aspect) so the window stays
   * freely resizable, and the aspect frame letterboxes the video to fit.
   * Reserve room for the header when it is (or is heading to be) revealed, so
   * the requested size is the settled one - the video itself is @height. */
  int top = adw_toolbar_view_get_reveal_top_bars (self->toolbar_view)
              ? pm_window_natural_top_bar_height (self)
              : 0;
  int win_h = height + top;

  /* This is a programmatic resize that already lands on the locked aspect, so
   * record it and suppress the resize handler's correction. */
  self->aspect_lock_applying = TRUE;
  gtk_window_set_default_size (GTK_WINDOW (self), width, win_h);
  self->aspect_locked_w = width;
  self->aspect_locked_h = win_h;
  self->aspect_lock_applying = FALSE;
}

static gboolean
pm_window_get_stream_aspect (PmWindow *self, double *out_aspect)
{
  PmStreamInfo stream = { 0 };
  if (!pm_session_get_stream_info (self->session, &stream))
    return FALSE;
  if (stream.width == 0 || stream.height == 0)
    return FALSE;

  *out_aspect = (double) stream.width / stream.height;
  return TRUE;
}

static void
pm_window_get_mirror_max_size (PmWindow *self, int *out_width, int *out_height)
{
  pm_window_get_window_max_size (self,
                                 pm_window_natural_top_bar_height (self),
                                 TRUE, out_width, out_height);
}

static void
pm_window_get_mirror_resize_max_size (PmWindow *self, int *out_width, int *out_height)
{
  pm_window_get_window_max_size (self,
                                 pm_window_natural_top_bar_height (self),
                                 FALSE, out_width, out_height);
}

static void
pm_window_clamp_to_aspect_min (PmWindow *self,
                               double    aspect,
                               int      *width,
                               int      *height)
{
  int min_w, min_h;
  pm_window_get_aspect_min_size (aspect, &min_w, &min_h);
  min_w = MAX (min_w, pm_window_get_top_bar_min_width (self));

  *width = MAX (min_w, *width);
  *height = (int) round (*width / aspect);

  if (*height < min_h) {
    *height = min_h;
    *width = (int) round (*height * aspect);
  }
}

static void
pm_window_clamp_to_initial_aspect (PmWindow *self,
                                   double    aspect,
                                   int      *width,
                                   int      *height)
{
  int max_w, max_h;
  pm_window_clamp_to_aspect_min (self, aspect, width, height);
  pm_window_get_mirror_max_size (self, &max_w, &max_h);

  if (*width > max_w) {
    *width = max_w;
    *height = (int) round (*width / aspect);
  }
  if (*height > max_h) {
    *height = max_h;
    *width = (int) round (*height * aspect);
  }
}

static void
pm_window_clamp_to_resize_aspect (PmWindow *self,
                                  double    aspect,
                                  int      *width,
                                  int      *height)
{
  int max_w, max_h;
  pm_window_clamp_to_aspect_min (self, aspect, width, height);
  pm_window_get_mirror_resize_max_size (self, &max_w, &max_h);

  if (*width > max_w) {
    *width = max_w;
    *height = (int) round (*width / aspect);
  }
  if (*height > max_h) {
    *height = max_h;
    *width = (int) round (*height * aspect);
  }
}

static void
pm_window_update_stream_aspect (PmWindow *self, gboolean preserve_area)
{
  double aspect = 0.0;
  int min_w, min_h;
  if (!pm_window_get_stream_aspect (self, &aspect))
    return;

  /* The aspect frame always keeps the video itself undistorted (letterboxing
   * inside whatever the window allows). */
  gtk_aspect_frame_set_ratio (self->mirror_frame, (float) aspect);

  /* Free resize: do not drive the window from the stream aspect. Relax the
   * minimum so the window can be shaped freely. */
  if (self->free_resize) {
    gtk_widget_set_size_request (GTK_WIDGET (self->mirror_frame),
                                 PM_MIRROR_MIN_WIDTH, PM_MIRROR_MIN_HEIGHT);
    return;
  }

  pm_window_get_aspect_min_size (aspect, &min_w, &min_h);
  pm_window_clamp_to_initial_aspect (self, aspect, &min_w, &min_h);
  gtk_widget_set_size_request (GTK_WIDGET (self->mirror_frame), min_w, min_h);

  if (self->session == NULL ||
      pm_session_get_state (self->session) != PM_STATE_MIRRORING)
    return;

  if (!preserve_area) {
    PmStreamInfo stream = { 0 };
    pm_session_get_stream_info (self->session, &stream);
    int width = stream.width;
    int height = stream.height;
    pm_window_clamp_to_initial_aspect (self, aspect, &width, &height);
    pm_window_set_mirror_content_size (self, width, height);
    return;
  }

  int cur_w = gtk_widget_get_width (GTK_WIDGET (self->mirror_frame));
  int cur_h = gtk_widget_get_height (GTK_WIDGET (self->mirror_frame));
  if (cur_w <= 0 || cur_h <= 0)
    return;

  double area = (double) cur_w * cur_h;
  int width = (int) round (sqrt (area * aspect));
  int height = (int) round (width / aspect);
  pm_window_clamp_to_resize_aspect (self, aspect, &width, &height);
  pm_window_set_mirror_content_size (self, width, height);
}

static void
pm_window_fit_to_stream (PmWindow *self)
{
  PmStreamInfo stream = { 0 };
  if (!pm_session_get_stream_info (self->session, &stream))
    return;

  int max_w, max_h;
  pm_window_get_mirror_max_size (self, &max_w, &max_h);

  double scale = MIN ((double) max_w / stream.width,
                      (double) max_h / stream.height);
  if (scale <= 0.0 || !isfinite (scale))
    scale = 1.0;

  int width = MAX (PM_MIRROR_MIN_WIDTH, (int) round (stream.width * scale));
  int height = MAX (PM_MIRROR_MIN_HEIGHT, (int) round (stream.height * scale));

  /* In free-resize mode the window uses the compositor's native edge resize.
   * When the phone aspect is locked the window is instead made non-resizable so
   * the compositor can never free-resize it off-aspect; a dedicated edge-drag
   * gesture drives the size and keeps it on the aspect line the whole time. */
  gtk_window_set_resizable (GTK_WINDOW (self), self->free_resize);
  pm_window_set_resize_gesture_active (self, !self->free_resize);
  pm_window_update_stream_aspect (self, FALSE);
  pm_window_clamp_to_initial_aspect (self,
                                     (double) stream.width / stream.height,
                                     &width, &height);
  pm_window_set_mirror_content_size (self, width, height);
}

static void
pm_window_reset_window_shape (PmWindow *self)
{
  int max_w, max_h;
  pm_window_get_window_max_size (self, 0, TRUE, &max_w, &max_h);

  gtk_window_set_resizable (GTK_WINDOW (self), TRUE);
  pm_window_set_resize_gesture_active (self, FALSE);
  gtk_widget_set_size_request (GTK_WIDGET (self->mirror_frame), -1, -1);
  gtk_window_set_default_size (GTK_WINDOW (self),
                               MIN (420, max_w),
                               MIN (760, max_h));
}

/* Aspect-ratio snap-back - now only a backstop.
 *
 * While mirroring is aspect-locked the window is made non-resizable and the
 * dedicated edge-drag gesture (see the "live aspect-locked resize" block below)
 * is the only thing that resizes it, so this handler does nothing in that case.
 * It still matters during the brief windows where the window is made
 * temporarily resizable (e.g. an AdwDialog bottom sheet, which requires a
 * resizable host): a compositor free-resize then snaps the window back onto the
 * phone aspect afterwards. The dimension with the larger change
 * is treated as the one the user dragged; the other is rebuilt from the stream
 * aspect. Skipped in free-resize mode, where the window is unconstrained. */
static void
on_window_size_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  double aspect = 0.0;

  if (self->aspect_lock_applying || self->chrome_animating || self->free_resize ||
      self->session == NULL ||
      pm_session_get_state (self->session) != PM_STATE_MIRRORING ||
      !pm_window_get_stream_aspect (self, &aspect))
    return;

  int cur_w = 0, cur_h = 0;
  gtk_window_get_default_size (GTK_WINDOW (object), &cur_w, &cur_h);
  if (cur_w <= 0 || cur_h <= 0)
    return;

  int top = adw_toolbar_view_get_top_bar_height (self->toolbar_view);

  /* The video area (window minus the header) is what must match the aspect. */
  int video_w, video_h;
  if (ABS (cur_w - self->aspect_locked_w) >= ABS (cur_h - self->aspect_locked_h)) {
    video_w = cur_w;
    video_h = (int) round (video_w / aspect);
  } else {
    video_h = MAX (1, cur_h - top);
    video_w = (int) round (video_h * aspect);
  }
  pm_window_clamp_to_resize_aspect (self, aspect, &video_w, &video_h);

  int target_w = video_w;
  int target_h = video_h + top;

  /* Already on the aspect line (within rounding) - just record the position. */
  if (ABS (target_w - cur_w) <= 1 && ABS (target_h - cur_h) <= 1) {
    self->aspect_locked_w = cur_w;
    self->aspect_locked_h = cur_h;
    return;
  }

  self->aspect_lock_applying = TRUE;
  gtk_window_set_default_size (GTK_WINDOW (object), target_w, target_h);
  self->aspect_locked_w = target_w;
  self->aspect_locked_h = target_h;
  self->aspect_lock_applying = FALSE;
}

/* --- live aspect-locked resize -------------------------------------------
 *
 * The old behaviour let the compositor free-resize the window and only snapped
 * it back onto the phone aspect on release - so the window visibly jumped at
 * the end of every drag. To keep it on-aspect the *whole* time, the window is
 * made non-resizable while locked (so the compositor never drags it) and the
 * resize is driven here: a drag in the right/bottom edge band updates the
 * window size directly on every motion event, always landing on the aspect
 * line. Only edges that keep the top-left corner fixed are honored, because
 * Wayland gives clients no way to reposition their own surface. */

static gboolean
pm_window_resize_lock_active (PmWindow *self)
{
  /* Active only while the phone aspect is locked and the window is in the
   * non-resizable state set for that lock. When something temporarily makes the
   * window resizable (a dialog sheet), this stands down and lets the native path
   * (plus the snap-back backstop) handle it. */
  return !self->free_resize &&
         self->session != NULL &&
         pm_session_get_state (self->session) == PM_STATE_MIRRORING &&
         !gtk_window_get_resizable (GTK_WINDOW (self));
}

static PmResizeEdge
pm_window_hit_test_edge (PmWindow *self, double x, double y)
{
  PmResizeEdge edge = PM_EDGE_NONE;
  int w = gtk_widget_get_width (GTK_WIDGET (self));
  int h = gtk_widget_get_height (GTK_WIDGET (self));

  if (x >= w - PM_RESIZE_GRAB_PX)
    edge |= PM_EDGE_RIGHT;
  if (y >= h - PM_RESIZE_GRAB_PX)
    edge |= PM_EDGE_BOTTOM;

  return edge;
}

static const char *
pm_window_resize_cursor_name (PmResizeEdge edge)
{
  switch (edge) {
    case PM_EDGE_RIGHT:        return "e-resize";
    case PM_EDGE_BOTTOM:       return "s-resize";
    case PM_EDGE_BOTTOM_RIGHT: return "se-resize";
    default:                   return NULL;
  }
}

static void
on_resize_drag_begin (GtkGestureDrag *gesture,
                      double          start_x,
                      double          start_y,
                      gpointer        user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  if (!pm_window_resize_lock_active (self)) {
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  PmResizeEdge edge = pm_window_hit_test_edge (self, start_x, start_y);
  if (edge == PM_EDGE_NONE) {
    /* Not on a resize band - let the press through to the video/buttons. */
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }

  self->resize_edge = edge;
  self->resize_start_w = gtk_widget_get_width (GTK_WIDGET (self));
  self->resize_start_h = gtk_widget_get_height (GTK_WIDGET (self));
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_resize_drag_update (GtkGestureDrag *gesture,
                       double          offset_x,
                       double          offset_y,
                       gpointer        user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  double aspect = 0.0;

  if (self->resize_edge == PM_EDGE_NONE ||
      !pm_window_get_stream_aspect (self, &aspect))
    return;

  int top = adw_toolbar_view_get_reveal_top_bars (self->toolbar_view)
              ? pm_window_natural_top_bar_height (self)
              : 0;

  int new_w = self->resize_start_w;
  int new_h = self->resize_start_h;
  if (self->resize_edge & PM_EDGE_RIGHT)
    new_w = self->resize_start_w + (int) round (offset_x);
  if (self->resize_edge & PM_EDGE_BOTTOM)
    new_h = self->resize_start_h + (int) round (offset_y);

  /* Resolve to the video area (window minus the header), then rebuild the
   * other dimension from the stream aspect. For a corner drag the axis the
   * pointer moved furthest along drives the size. */
  int video_w = new_w;
  int video_h = MAX (1, new_h - top);
  gboolean horiz = (self->resize_edge & PM_EDGE_RIGHT) != 0;
  gboolean vert  = (self->resize_edge & PM_EDGE_BOTTOM) != 0;

  if (horiz && vert) {
    if (ABS (offset_x) >= ABS (offset_y))
      video_h = (int) round (video_w / aspect);
    else
      video_w = (int) round (video_h * aspect);
  } else if (horiz) {
    video_h = (int) round (video_w / aspect);
  } else {
    video_w = (int) round (video_h * aspect);
  }

  pm_window_clamp_to_resize_aspect (self, aspect, &video_w, &video_h);
  /* Records the new locked size and guards the snap-back handler. */
  pm_window_set_mirror_content_size (self, video_w, video_h);
}

static void
on_resize_drag_end (GtkGestureDrag *gesture,
                    double          offset_x,
                    double          offset_y,
                    gpointer        user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  self->resize_edge = PM_EDGE_NONE;
}

/* Show a resize cursor while hovering the edge bands so the grab area is
 * discoverable on the undecorated, non-resizable mirror window. */
static void
on_resize_motion (GtkEventControllerMotion *controller,
                  double                    x,
                  double                    y,
                  gpointer                  user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  if (!pm_window_resize_lock_active (self)) {
    gtk_widget_set_cursor (GTK_WIDGET (self), NULL);
    return;
  }

  const char *name = pm_window_resize_cursor_name (pm_window_hit_test_edge (self, x, y));
  if (name != NULL)
    gtk_widget_set_cursor_from_name (GTK_WIDGET (self), name);
  else
    gtk_widget_set_cursor (GTK_WIDGET (self), NULL);
}

/* Take the resize gesture fully out of the event stream when the aspect lock is
 * off. A capture-phase gesture grabs the press before GTK's native edge-resize
 * even when it later denies the sequence, so simply denying is not enough to
 * let free-resize mode use the compositor's resize - the controllers must be
 * stopped from seeing events at all. */
static void
pm_window_set_resize_gesture_active (PmWindow *self, gboolean active)
{
  if (self->resize_drag == NULL || self->resize_motion == NULL)
    return;

  gtk_event_controller_set_propagation_phase (self->resize_drag,
                                              active ? GTK_PHASE_CAPTURE : GTK_PHASE_NONE);
  gtk_event_controller_set_propagation_phase (self->resize_motion,
                                              active ? GTK_PHASE_BUBBLE : GTK_PHASE_NONE);

  if (!active) {
    self->resize_edge = PM_EDGE_NONE;
    gtk_widget_set_cursor (GTK_WIDGET (self), NULL);
  }
}

static void
pm_window_set_action_enabled (PmWindow *self, const char *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), name);
  if (G_IS_SIMPLE_ACTION (action))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

/* Drive the state-dependent menu rows off the mirroring flag. The items carry
 * hidden-when="action-disabled", so disabling an action also hides its row:
 * Disconnect and Lockscreen PIN appear only while mirroring; First Time Setup
 * only in the main menu. */
static void
pm_window_set_disconnect_available (PmWindow *self, gboolean mirroring)
{
  pm_window_set_action_enabled (self, "disconnect", mirroring);
  pm_window_set_action_enabled (self, "lockscreen-pin", mirroring);
  pm_window_set_action_enabled (self, "first-setup", !mirroring);
}

/* --- state -> UI ---------------------------------------------------------- */

static void
pm_window_apply_state (PmWindow   *self,
                       PmState     state,
                       const char *message)
{
  gboolean mirroring = (state == PM_STATE_MIRRORING);
  pm_window_set_disconnect_available (self, mirroring);

  if (!mirroring) {
    adw_window_title_set_title (self->title, _("Phone Mirror"));
    adw_window_title_set_subtitle (self->title, NULL);
  }

  switch (state) {
    case PM_STATE_IDLE:
      pm_window_reset_window_shape (self);
      adw_view_stack_set_visible_child_name (self->stack, "searching");
      adw_status_page_set_title (self->status_page, _("Phone Mirror"));
      adw_status_page_set_description (self->status_page,
        _("Press Connect to find your phone automatically."));
      gtk_spinner_set_spinning (self->spinner, FALSE);
      gtk_widget_set_visible (GTK_WIDGET (self->connect_button), TRUE);
      break;

    case PM_STATE_SEARCHING:
      pm_window_reset_window_shape (self);
      adw_view_stack_set_visible_child_name (self->stack, "searching");
      adw_status_page_set_title (self->status_page, _("Searching…"));
      adw_status_page_set_description (self->status_page,
        _("Looking for your phone on the local network."));
      gtk_spinner_set_spinning (self->spinner, TRUE);
      gtk_widget_set_visible (GTK_WIDGET (self->connect_button), FALSE);
      break;

    case PM_STATE_CONNECTING:
      pm_window_reset_window_shape (self);
      adw_view_stack_set_visible_child_name (self->stack, "searching");
      adw_status_page_set_title (self->status_page, _("Connecting…"));
      adw_status_page_set_description (self->status_page,
        message ? message : _("Starting the mirror server on your device."));
      gtk_spinner_set_spinning (self->spinner, TRUE);
      gtk_widget_set_visible (GTK_WIDGET (self->connect_button), FALSE);
      break;

    case PM_STATE_MIRRORING:
      adw_toolbar_view_set_extend_content_to_top_edge (self->toolbar_view, FALSE);
      adw_toolbar_view_set_top_bar_style (self->toolbar_view, ADW_TOOLBAR_RAISED_BORDER);
      adw_header_bar_set_show_title (self->header_bar, FALSE);
      gtk_widget_set_visible (GTK_WIDGET (self->pin_button), TRUE);
      gtk_window_set_decorated (GTK_WINDOW (self), FALSE);
      pm_window_reset_mirror_chrome (self, FALSE);
      adw_view_stack_set_visible_child_name (self->stack, "mirror");
      pm_window_fit_to_stream (self);
      gtk_spinner_set_spinning (self->spinner, FALSE);
      break;

    case PM_STATE_ERROR:
      pm_window_reset_window_shape (self);
      adw_view_stack_set_visible_child_name (self->stack, "searching");
      adw_status_page_set_title (self->status_page, _("Connection failed"));
      adw_status_page_set_description (self->status_page,
        message ? message : _("Could not reach the device."));
      gtk_spinner_set_spinning (self->spinner, FALSE);
      gtk_widget_set_visible (GTK_WIDGET (self->connect_button), TRUE);
      break;
  }

  /* Keep Android's top gesture area usable; the header takes its own space
   * above the picture (and the window grows to make room) instead of overlaying
   * the mirrored pixels. */
  adw_toolbar_view_set_extend_content_to_top_edge (self->toolbar_view, FALSE);
  adw_toolbar_view_set_top_bar_style (self->toolbar_view,
                                      mirroring ? ADW_TOOLBAR_RAISED_BORDER
                                                : ADW_TOOLBAR_FLAT);
  adw_header_bar_set_show_title (self->header_bar, !mirroring);
  adw_header_bar_set_decoration_layout (self->header_bar, NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->pin_button), mirroring);
  if (!mirroring)
    pm_window_set_mirror_chrome_pinned (self, FALSE);
  gtk_window_set_decorated (GTK_WINDOW (self), !mirroring);
  /* Enables the wider resize grab area on the undecorated mirror window. */
  if (mirroring)
    gtk_widget_add_css_class (GTK_WIDGET (self), "specula-mirror");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "specula-mirror");
  pm_window_reset_mirror_chrome (self, !mirroring);
}

static void
pm_window_present_if_deferred (PmWindow *self)
{
  if (!self->defer_initial_present)
    return;

  self->defer_initial_present = FALSE;
  gtk_window_present (GTK_WINDOW (self));
}

static void
on_session_state_changed (PmSession  *session,
                          guint       state,
                          const char *message,
                          gpointer    user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  PmState pm_state = (PmState) state;

  if (self->startup_search_active) {
    if (pm_state == PM_STATE_SEARCHING || pm_state == PM_STATE_CONNECTING)
      return;

    self->startup_search_active = FALSE;
    if (pm_state == PM_STATE_IDLE || pm_state == PM_STATE_ERROR) {
      pm_window_apply_state (self, PM_STATE_IDLE, NULL);
      pm_window_present_if_deferred (self);
      return;
    }
  }

  pm_window_apply_state (self, pm_state, message);
  if (pm_state == PM_STATE_MIRRORING)
    pm_window_present_if_deferred (self);
}

static void
on_startup_check_failed (PmSession *session, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  if (!self->startup_search_active)
    return;

  pm_window_apply_state (self, PM_STATE_IDLE, NULL);
  pm_window_present_if_deferred (self);
}

static void
on_session_stream_changed (PmSession *session,
                           guint      width,
                           guint      height,
                           gpointer   user_data)
{
  pm_window_update_stream_aspect (PM_WINDOW (user_data), TRUE);
}

/* --- header-bar auto-hide while mirroring --------------------------------- */

static void
on_motion (GtkEventControllerMotion *ctrl,
           double                    x,
           double                    y,
           gpointer                  user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  if (self->session == NULL ||
      pm_session_get_state (self->session) != PM_STATE_MIRRORING)
    return;

  if (y <= PM_MIRROR_TOP_REVEAL_PX)
    pm_window_set_mirror_chrome (self, TRUE);
  else if (y > PM_MIRROR_TOP_HIDE_PX && !self->mirror_chrome_pinned)
    pm_window_set_mirror_chrome (self, FALSE);
}

/* --- user actions --------------------------------------------------------- */

/* Manual connection passes a target; Pair & Connect passes NULL so discovery
 * finds the actual post-pairing connection endpoint. */
static void
on_device_chosen (const char *host, guint16 port, const char *name, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  if (host == NULL) {
    pm_session_start (self->session, NULL);
    return;
  }

  PmDeviceInfo info = { .host = (char *) host, .port = port,
                        .name = (char *) name, .serial = NULL };
  pm_session_start (self->session, &info);
}

/* While the window is temporarily resizable for the setup dialog (mirroring
 * mode), the WM can still allow maximizing (e.g. double-clicking the
 * title bar). The mirror is locked to the video aspect, so revert any maximize
 * the moment it happens. */
static void
on_window_maximized_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  GtkWindow *win = GTK_WINDOW (object);
  if (gtk_window_is_maximized (win))
    gtk_window_unmaximize (win);
}

/* Return a copy of @layout (a "gtk-decoration-layout" string) with the
 * "maximize" button token removed from either side of the "left:right" split. */
static char *
decoration_layout_without_maximize (const char *layout)
{
  g_auto (GStrv) sides = g_strsplit (layout, ":", -1);
  for (guint s = 0; sides[s] != NULL; s++) {
    g_auto (GStrv) tokens = g_strsplit (sides[s], ",", -1);
    GString *rebuilt = g_string_new (NULL);
    for (guint t = 0; tokens[t] != NULL; t++) {
      if (g_strcmp0 (tokens[t], "maximize") == 0)
        continue;
      if (rebuilt->len > 0)
        g_string_append_c (rebuilt, ',');
      g_string_append (rebuilt, tokens[t]);
    }
    g_free (sides[s]);
    sides[s] = g_string_free (rebuilt, FALSE);
  }
  return g_strjoinv (":", sides);
}

/* Hide the header bar's maximize button while the window is temporarily
 * resizable, so it doesn't flash up behind the menu/sheet during mirroring. */
static void
pm_window_suppress_maximize_button (PmWindow *self)
{
  const char *layout = adw_header_bar_get_decoration_layout (self->header_bar);
  g_autofree char *settings_layout = NULL;
  if (layout == NULL) {
    g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)),
                  "gtk-decoration-layout", &settings_layout, NULL);
    layout = settings_layout;
  }
  if (layout == NULL)
    return;

  g_autofree char *stripped = decoration_layout_without_maximize (layout);
  adw_header_bar_set_decoration_layout (self->header_bar, stripped);
}

/* Restore the host window's resizable state once an in-window dialog is gone. */
static void
on_window_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  gboolean was_resizable =
    GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dialog), "pm-prev-resizable"));

  gulong handler =
    GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (dialog), "pm-maximized-handler"));
  if (handler != 0) {
    g_signal_handler_disconnect (self, handler);
    /* Restore the default decoration layout (brings the maximize button back). */
    adw_header_bar_set_decoration_layout (self->header_bar, NULL);
  }

  gtk_window_set_resizable (GTK_WINDOW (self), was_resizable);
}

/* Present @dialog as an in-window bottom sheet, even while mirroring.
 *
 * AdwDialog falls back to a standalone floating window (ignoring its
 * presentation mode) when the host window is non-resizable. Mirroring locks
 * the window to the video aspect, so the window is made resizable for the
 * dialog's lifetime, guarded against maximizing, and restored on close. */
static void
pm_window_present_sheet (PmWindow *self, AdwDialog *dialog)
{
  gboolean was_resizable = gtk_window_get_resizable (GTK_WINDOW (self));
  g_object_set_data (G_OBJECT (dialog), "pm-prev-resizable",
                     GINT_TO_POINTER (was_resizable));

  /* If the window was locked (mirroring), guard against the user maximizing it
   * via the temporary resizable state. */
  if (!was_resizable) {
    gulong handler = g_signal_connect (self, "notify::maximized",
                                       G_CALLBACK (on_window_maximized_notify), NULL);
    g_object_set_data (G_OBJECT (dialog), "pm-maximized-handler",
                       GSIZE_TO_POINTER (handler));
    pm_window_suppress_maximize_button (self);
  }

  g_signal_connect (dialog, "closed", G_CALLBACK (on_window_dialog_closed), self);
  gtk_window_set_resizable (GTK_WINDOW (self), TRUE);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_setup_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  PmConnectDialog *dialog = pm_connect_dialog_new (on_device_chosen, self);
  pm_window_present_sheet (self, ADW_DIALOG (dialog));
}

static void
on_manual_connect_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  PmConnectDialog *dialog = pm_manual_connect_dialog_new (on_device_chosen, self);
  pm_window_present_sheet (self, ADW_DIALOG (dialog));
}

static void
on_disconnect_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  if (self->session != NULL &&
      pm_session_get_state (self->session) != PM_STATE_IDLE)
    pm_session_stop (self->session);
}

/* Toggle free-window-resize (drop the phone aspect-ratio lock). */
static void
pm_window_set_free_resize (PmWindow *self, gboolean free_resize)
{
  if (self->free_resize == free_resize)
    return;
  self->free_resize = free_resize;

  if (self->session == NULL ||
      pm_session_get_state (self->session) != PM_STATE_MIRRORING)
    return;

  /* Free resize hands sizing back to the compositor's native edge resize;
   * locking takes it over with the edge-drag gesture (window made non-resizable). */
  gtk_window_set_resizable (GTK_WINDOW (self), free_resize);
  pm_window_set_resize_gesture_active (self, !free_resize);

  if (free_resize)
    /* Relax the minimum so the window can be dragged to any shape; the current
     * size is left untouched. */
    gtk_widget_set_size_request (GTK_WIDGET (self->mirror_frame),
                                 PM_MIRROR_MIN_WIDTH, PM_MIRROR_MIN_HEIGHT);
  else
    /* Snap back onto the phone aspect, keeping roughly the same area. */
    pm_window_update_stream_aspect (self, TRUE);
}

static void
pm_window_set_mouse_mode (PmWindow *self, gboolean mouse_mode)
{
  if (self->mouse_mode == mouse_mode)
    return;
  self->mouse_mode = mouse_mode;
  pm_session_set_mouse_mode (self->session, mouse_mode);
}

/* Returns TRUE if the audio setting actually changed. */
static gboolean
pm_window_set_audio (PmWindow *self, gboolean audio)
{
  if (self->audio == audio)
    return FALSE;
  self->audio = audio;
  /* Audio is negotiated at connect time; the caller reconnects to apply it. */
  pm_session_set_audio_enabled (self->session, audio);
  return TRUE;
}

/* Returns TRUE if the bitrate actually changed. */
static gboolean
pm_window_set_video_bitrate (PmWindow *self, guint mbps)
{
  if (self->video_bitrate == mbps)
    return FALSE;
  self->video_bitrate = mbps;
  /* Bitrate is negotiated at connect time; the caller reconnects to apply it. */
  pm_session_set_video_bitrate (self->session, mbps);
  return TRUE;
}

/* Returns TRUE if the mode actually changed. */
static gboolean
pm_window_set_display_mode (PmWindow *self, PmDisplayMode mode)
{
  if (self->display_mode == mode)
    return FALSE;
  self->display_mode = mode;
  pm_session_set_display_mode (self->session, mode);
  return TRUE;
}

/* Returns TRUE if the screen-off setting actually changed. */
static gboolean
pm_window_set_screen_off (PmWindow *self, gboolean screen_off)
{
  if (self->screen_off == screen_off)
    return FALSE;
  self->screen_off = screen_off;
  /* Applied when the on-device server's control socket comes up, so the caller
   * reconnects to apply it to a live session. */
  pm_session_set_screen_off (self->session, screen_off);
  return TRUE;
}

/* Returns TRUE if any of the virtual-display geometry actually changed. */
static gboolean
pm_window_set_virtual_display (PmWindow *self, guint width, guint height, guint dpi)
{
  if (self->display_width == width && self->display_height == height &&
      self->display_dpi == dpi)
    return FALSE;
  self->display_width = width;
  self->display_height = height;
  self->display_dpi = dpi;
  pm_session_set_virtual_display (self->session, width, height, dpi);
  return TRUE;
}

static void
on_settings_changed (const PmSettings *settings, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  pm_window_set_free_resize (self, settings->free_resize);
  pm_window_set_mouse_mode (self, settings->mouse_mode);

  gboolean audio_changed   = pm_window_set_audio (self, settings->audio);
  gboolean bitrate_changed = pm_window_set_video_bitrate (self, settings->video_bitrate);
  gboolean mode_changed    = pm_window_set_display_mode (self, settings->display_mode);
  gboolean screen_off_changed = pm_window_set_screen_off (self, settings->screen_off);
  gboolean geom_changed    = pm_window_set_virtual_display (self, settings->display_width,
                                                            settings->display_height,
                                                            settings->display_dpi);

  /* Audio and the display config are decided when the on-device server launches,
   * so a live session must reconnect to apply them. Do it seamlessly (no
   * discovery wait) rather than making the user disconnect and reconnect by
   * hand. A geometry change only matters while a virtual display is in use.
   *
   * Bitrate is the same kind of connect-time setting, but its spin row fires on
   * every increment, so reconnecting here would thrash the stream as the user
   * holds the +/- button. Defer it instead: flag it and reconnect once, when the
   * settings sheet is closed (on_settings_dialog_closed). */
  if (bitrate_changed)
    self->bitrate_reconnect_pending = TRUE;

  if (audio_changed || mode_changed || screen_off_changed ||
      (geom_changed && self->display_mode == PM_DISPLAY_VIRTUAL)) {
    pm_session_reconnect (self->session);
    /* That reconnect already picked up the latest bitrate. */
    self->bitrate_reconnect_pending = FALSE;
  }

  pm_window_save_prefs (self);
}

/* Flush a bitrate change deferred while the settings sheet was open: the spin
 * row fires per-increment, so the reconnect waits until the sheet closes and
 * runs once with the value the user actually settled on. */
static void
on_settings_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  if (!self->bitrate_reconnect_pending)
    return;
  self->bitrate_reconnect_pending = FALSE;
  pm_session_reconnect (self->session);
}

/* --- lockscreen PIN ------------------------------------------------------- */

/* Off-thread save: reading the device MAC means an adb round-trip, so it must
 * not run on the UI thread. `pin == NULL` removes the stored PIN instead. */
typedef struct { char *serial; char *pin; } PinSaveData;

static void
pin_save_data_free (gpointer data)
{
  PinSaveData *d = data;
  g_free (d->serial);
  if (d->pin != NULL) {
    memset (d->pin, 0, strlen (d->pin));   /* don't leave the PIN in freed memory */
    g_free (d->pin);
  }
  g_free (d);
}

static void
pin_save_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  PinSaveData *d = task_data;
  g_autofree char *mac = pm_adb_query_mac (d->serial);
  if (mac == NULL) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "could not read the device's MAC address");
    return;
  }

  if (d->pin == NULL) {
    pm_pinstore_remove (mac);
    g_task_return_boolean (task, TRUE);
    return;
  }

  GError *error = NULL;
  if (!pm_pinstore_set (mac, d->pin, &error)) {
    g_task_return_error (task, error);
    return;
  }

  /* The phone is connected, so the connect-time auto-unlock has already passed.
   * Apply the just-saved PIN now (best-effort) so saving it from the menu unlocks
   * a phone sitting on its keyguard immediately, not only on the next connect. */
  pm_adb_unlock_with_pin (d->serial, d->pin, NULL);
  g_task_return_boolean (task, TRUE);
}

static void
pin_save_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (GError) error = NULL;
  if (!g_task_propagate_boolean (G_TASK (res), &error))
    g_warning ("lockscreen-pin: %s", error ? error->message : "save failed");
  else
    g_message ("lockscreen-pin: updated for the connected device");
}

/* Store `pin` (NULL/empty removes it) for the current device. When a phone is
 * live we save straight to its MAC; otherwise we stash it as "pending" - exactly
 * like pairing - so it is committed to the device's MAC on the next connect. */
static void
pm_window_apply_lockscreen_pin (PmWindow *self, const char *pin)
{
  gboolean have_pin = (pin != NULL && *pin != '\0');
  g_autofree char *serial = pm_session_dup_serial (self->session);

  if (serial != NULL) {
    PinSaveData *d = g_new0 (PinSaveData, 1);
    d->serial = g_steal_pointer (&serial);
    d->pin = have_pin ? g_strdup (pin) : NULL;

    GTask *task = g_task_new (self, NULL, pin_save_done, NULL);
    g_task_set_task_data (task, d, pin_save_data_free);
    g_task_run_in_thread (task, pin_save_thread);
    g_object_unref (task);
    return;
  }

  /* Not connected: pending now, committed on the next connect. An empty PIN
   * clears any pending entry. */
  g_autoptr (GError) error = NULL;
  if (!pm_pinstore_set_pending (have_pin ? pin : NULL, &error))
    g_warning ("lockscreen-pin: %s", error->message);
}

typedef struct {
  PmWindow    *window;
  GtkEditable *entry;
  AdwDialog   *dialog;
} PinDialogCtx;

static void
on_pin_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  g_free (user_data);   /* PinDialogCtx; the dialog owns its own widgets */
}

static void
on_pin_save_clicked (GtkButton *button, gpointer user_data)
{
  PinDialogCtx *ctx = user_data;
  const char *pin = gtk_editable_get_text (ctx->entry);
  /* Empty + Save is a deliberate no-op so an accidental Save never wipes a saved
   * PIN; removal is the explicit Remove button. */
  if (pin != NULL && *pin != '\0')
    pm_window_apply_lockscreen_pin (ctx->window, pin);
  adw_dialog_close (ctx->dialog);
}

static void
on_pin_remove_clicked (GtkButton *button, gpointer user_data)
{
  PinDialogCtx *ctx = user_data;
  pm_window_apply_lockscreen_pin (ctx->window, NULL);
  adw_dialog_close (ctx->dialog);
}

/* A centered pill action button, matching the device-setup sheet's style. */
static GtkWidget *
pin_action_button (const char *label, const char *style_class,
                   GCallback cb, gpointer data)
{
  GtkWidget *b = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (b, "pill");
  if (style_class != NULL)
    gtk_widget_add_css_class (b, style_class);
  gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (b, 6);
  g_signal_connect (b, "clicked", cb, data);
  return b;
}

static void
on_lockscreen_pin_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  /* A bottom-sheet AdwDialog matching the Settings / device-setup sheets, rather
   * than a floating alert. */
  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, _("Lockscreen PIN"));
  adw_dialog_set_content_width (dialog, 420);
  adw_dialog_set_presentation_mode (dialog, ADW_DIALOG_BOTTOM_SHEET);

  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (grp, _("Auto-unlock"));
  adw_preferences_group_set_description (grp,
    _("Stored encrypted on this computer, tied to this phone, and used to unlock "
      "it automatically when it connects."));

  AdwPasswordEntryRow *row = ADW_PASSWORD_ENTRY_ROW (adw_password_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Lockscreen PIN"));
  adw_preferences_group_add (grp, GTK_WIDGET (row));

  PinDialogCtx *ctx = g_new0 (PinDialogCtx, 1);
  ctx->window = self;
  ctx->entry = GTK_EDITABLE (row);
  ctx->dialog = dialog;

  adw_preferences_group_add (grp,
    pin_action_button (_("Save"), "suggested-action",
                       G_CALLBACK (on_pin_save_clicked), ctx));
  adw_preferences_group_add (grp,
    pin_action_button (_("Remove"), "destructive-action",
                       G_CALLBACK (on_pin_remove_clicked), ctx));
  adw_preferences_page_add (page, grp);

  AdwToolbarView *tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (tv, adw_header_bar_new ());
  adw_toolbar_view_set_content (tv, GTK_WIDGET (page));
  adw_dialog_set_child (dialog, GTK_WIDGET (tv));

  g_signal_connect (dialog, "closed", G_CALLBACK (on_pin_dialog_closed), ctx);

  pm_window_present_sheet (self, dialog);
}

static void
on_settings_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  self->bitrate_reconnect_pending = FALSE;
  PmSettings initial = { .free_resize = self->free_resize,
                         .mouse_mode = self->mouse_mode,
                         .audio = self->audio,
                         .video_bitrate = self->video_bitrate,
                         .display_mode = self->display_mode,
                         .display_width = self->display_width,
                         .display_height = self->display_height,
                         .display_dpi = self->display_dpi,
                         .screen_off = self->screen_off };
  PmSettingsDialog *dialog =
    pm_settings_dialog_new (&initial, on_settings_changed, self);
  g_signal_connect (dialog, "closed",
                    G_CALLBACK (on_settings_dialog_closed), self);
  pm_window_present_sheet (self, ADW_DIALOG (dialog));
}

static void
on_first_setup_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  self->setup_review_active = self->setup_complete;

  if (self->session != NULL &&
      pm_session_get_state (self->session) != PM_STATE_IDLE)
    pm_session_stop (self->session);
  else
    pm_window_apply_state (self, PM_STATE_IDLE, NULL);

  pm_window_show_setup_step (self, PM_SETUP_WELCOME);
}

static void
on_connect_clicked (GtkButton *button, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  /* Zero-click: always start auto-discovery. mDNS finds the device on the
   * network without any prior input; a saved pairing just adds a direct probe.
   * First-time pairing lives behind "Set Up Device…"; manual IP entry is an
   * app-menu fallback. */
  pm_session_start (self->session, NULL);
}

static void
on_pin_toggled (GtkToggleButton *button, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);
  pm_window_set_mirror_chrome_pinned (self,
                                      gtk_toggle_button_get_active (button));
}

static void
pm_window_save_prefs (PmWindow *self)
{
  PmPrefs prefs = {
    .free_resize = self->free_resize,
    .mouse_mode = self->mouse_mode,
    .audio = self->audio,
    .video_bitrate = self->video_bitrate,
    .display_mode = self->display_mode,
    .display_width = self->display_width,
    .display_height = self->display_height,
    .display_dpi = self->display_dpi,
    .screen_off = self->screen_off,
    .setup_complete = self->setup_complete,
  };
  pm_prefs_save (&prefs);
}

static GtkWidget *
phone_label_new (const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_halign (label, GTK_ALIGN_FILL);
  if (css_class != NULL)
    gtk_widget_add_css_class (label, css_class);
  return label;
}

typedef struct {
  double x;
  double y;
  double w;
  double h;
} PmSetupRect;

typedef struct {
  const char *icon;
  const char *title;
  const char *detail;
  gboolean    has_switch;
  double      switch_progress;
} PmSetupDrawRow;

enum {
  PM_SETUP_SETTINGS_ROW_NETWORK,
  PM_SETUP_SETTINGS_ROW_CONNECTED,
  PM_SETUP_SETTINGS_ROW_APPS,
  PM_SETUP_SETTINGS_ROW_NOTIFICATIONS,
  PM_SETUP_SETTINGS_ROW_BATTERY,
  PM_SETUP_SETTINGS_ROW_SYSTEM,
  PM_SETUP_SETTINGS_ROW_ABOUT,
  PM_SETUP_SETTINGS_ROW_DEVELOPER,
};

static const PmSetupDrawRow pm_setup_main_settings_rows[] = {
  { "Net", N_("Network & internet"), N_("Wi-Fi, SIM, hotspot"), FALSE, 0.0 },
  { "BT", N_("Connected devices"), N_("Bluetooth, pairing"), FALSE, 0.0 },
  { "App", N_("Apps"), N_("Permissions and defaults"), FALSE, 0.0 },
  { "Bell", N_("Notifications"), N_("History and conversations"), FALSE, 0.0 },
  { "Bat", N_("Battery"), N_("87%, until 10:30 PM"), FALSE, 0.0 },
  { "Sys", N_("System"), N_("Languages, gestures, time"), FALSE, 0.0 },
  { "Info", N_("About phone"), N_("Model, Android version"), FALSE, 0.0 },
  { "Dev", N_("Developer options"), N_("Debugging and input"), FALSE, 0.0 },
};

enum {
  PM_SETUP_ABOUT_PHONE_ROW_NAME,
  PM_SETUP_ABOUT_PHONE_ROW_ANDROID,
  PM_SETUP_ABOUT_PHONE_ROW_BUILD,
};

static const PmSetupDrawRow pm_setup_about_phone_rows[] = {
  { "ID", N_("Device name"), N_("Pixel phone"), FALSE, 0.0 },
  { "A", N_("Android version"), N_("Version and security update"), FALSE, 0.0 },
  { "123", N_("Build number"), N_("Tap seven times"), FALSE, 0.0 },
};

enum {
  PM_SETUP_DEVELOPER_ROW_MASTER,
  PM_SETUP_DEVELOPER_ROW_USB,
  PM_SETUP_DEVELOPER_ROW_WIRELESS,
  PM_SETUP_DEVELOPER_ROW_REVOKE,
  PM_SETUP_DEVELOPER_N_ROWS,
};

static void
pm_setup_developer_rows_for_state (PmSetupDrawRow rows[PM_SETUP_DEVELOPER_N_ROWS],
                                   double         usb_progress,
                                   double         wireless_progress)
{
  const PmSetupDrawRow base[] = {
    { "Dev", N_("Use developer options"), N_("On"), TRUE, 1.0 },
    { "USB", N_("USB debugging"), N_("Debug mode when USB is connected"), TRUE, 0.0 },
    { "Wi", N_("Wireless debugging"), N_("Debug over Wi-Fi"), TRUE, 0.0 },
    { "ADB", N_("Revoke debugging authorizations"), NULL, FALSE, 0.0 },
  };

  memcpy (rows, base, sizeof base);
  rows[PM_SETUP_DEVELOPER_ROW_USB].switch_progress = usb_progress;
  rows[PM_SETUP_DEVELOPER_ROW_WIRELESS].switch_progress = wireless_progress;
}

static double
pm_setup_clamp01 (double value)
{
  if (value < 0.0)
    return 0.0;
  if (value > 1.0)
    return 1.0;
  return value;
}

static double
pm_setup_interval (double t,
                   double start,
                   double end)
{
  if (end <= start)
    return t >= end ? 1.0 : 0.0;
  return pm_setup_clamp01 ((t - start) / (end - start));
}

static double
pm_setup_ease (double value)
{
  value = pm_setup_clamp01 (value);
  return value * value * (3.0 - 2.0 * value);
}

static double
pm_setup_mix (double a,
              double b,
              double t)
{
  return a + (b - a) * pm_setup_clamp01 (t);
}

static void
pm_setup_rounded_rect (cairo_t *cr,
                       double   x,
                       double   y,
                       double   w,
                       double   h,
                       double   r)
{
  r = MIN (r, MIN (w, h) / 2.0);
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2.0, 0.0);
  cairo_arc (cr, x + w - r, y + h - r, r, 0.0, G_PI / 2.0);
  cairo_arc (cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
  cairo_arc (cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
  cairo_close_path (cr);
}

static void
pm_setup_fill_rounded (cairo_t *cr,
                       double   x,
                       double   y,
                       double   w,
                       double   h,
                       double   r,
                       double   red,
                       double   green,
                       double   blue,
                       double   alpha)
{
  pm_setup_rounded_rect (cr, x, y, w, h, r);
  cairo_set_source_rgba (cr, red, green, blue, alpha);
  cairo_fill (cr);
}

static void
pm_setup_stroke_rounded (cairo_t *cr,
                         double   x,
                         double   y,
                         double   w,
                         double   h,
                         double   r,
                         double   red,
                         double   green,
                         double   blue,
                         double   alpha,
                         double   width)
{
  pm_setup_rounded_rect (cr, x, y, w, h, r);
  cairo_set_source_rgba (cr, red, green, blue, alpha);
  cairo_set_line_width (cr, width);
  cairo_stroke (cr);
}

static void
pm_setup_draw_text (cairo_t    *cr,
                    const char *text,
                    double      x,
                    double      baseline,
                    double      size,
                    gboolean    bold,
                    double      red,
                    double      green,
                    double      blue,
                    double      alpha)
{
  cairo_save (cr);
  cairo_set_source_rgba (cr, red, green, blue, alpha);
  cairo_select_font_face (cr,
                          "Sans",
                          CAIRO_FONT_SLANT_NORMAL,
                          bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, size);
  cairo_move_to (cr, x, baseline);
  cairo_show_text (cr, text);
  cairo_restore (cr);
}

static double
pm_setup_text_width (cairo_t    *cr,
                     const char *text,
                     double      size,
                     gboolean    bold)
{
  cairo_text_extents_t extents;

  cairo_save (cr);
  cairo_select_font_face (cr,
                          "Sans",
                          CAIRO_FONT_SLANT_NORMAL,
                          bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, size);
  cairo_text_extents (cr, text, &extents);
  cairo_restore (cr);

  return extents.width;
}

static void
pm_setup_draw_text_fit (cairo_t    *cr,
                        const char *text,
                        double      x,
                        double      baseline,
                        double      max_width,
                        double      size,
                        gboolean    bold,
                        double      red,
                        double      green,
                        double      blue,
                        double      alpha)
{
  char clipped[128];

  if (text == NULL || max_width <= 0.0)
    return;

  if (pm_setup_text_width (cr, text, size, bold) <= max_width) {
    pm_setup_draw_text (cr, text, x, baseline, size, bold, red, green, blue, alpha);
    return;
  }

  g_strlcpy (clipped, text, sizeof clipped);
  while (strlen (clipped) > 3) {
    char ellipsized[128];

    clipped[strlen (clipped) - 1] = '\0';
    g_snprintf (ellipsized, sizeof ellipsized, "%s...", clipped);
    if (pm_setup_text_width (cr, ellipsized, size, bold) <= max_width) {
      pm_setup_draw_text (cr,
                          ellipsized,
                          x,
                          baseline,
                          size,
                          bold,
                          red,
                          green,
                          blue,
                          alpha);
      return;
    }
  }

  pm_setup_draw_text (cr, "...", x, baseline, size, bold, red, green, blue, alpha);
}

static void
pm_setup_draw_text_center (cairo_t    *cr,
                           const char *text,
                           double      center_x,
                           double      baseline,
                           double      size,
                           gboolean    bold,
                           double      red,
                           double      green,
                           double      blue,
                           double      alpha)
{
  cairo_text_extents_t extents;

  cairo_save (cr);
  cairo_select_font_face (cr,
                          "Sans",
                          CAIRO_FONT_SLANT_NORMAL,
                          bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, size);
  cairo_text_extents (cr, text, &extents);
  cairo_restore (cr);

  pm_setup_draw_text (cr,
                      text,
                      center_x - extents.width / 2.0 - extents.x_bearing,
                      baseline,
                      size,
                      bold,
                      red,
                      green,
                      blue,
                      alpha);
}

static PmSetupRect
pm_setup_draw_phone_shell (cairo_t *cr,
                           double   x,
                           double   y,
                           double   w,
                           double   h)
{
  PmSetupRect screen = { x + 9.0, y + 9.0, w - 18.0, h - 18.0 };

  pm_setup_fill_rounded (cr, x, y, w, h, 25.0, 0.11, 0.13, 0.17, 1.0);
  pm_setup_fill_rounded (cr,
                         x + w / 2.0 - 19.0,
                         y + 7.0,
                         38.0,
                         7.0,
                         4.0,
                         0.08,
                         0.09,
                         0.12,
                         1.0);
  pm_setup_fill_rounded (cr,
                         screen.x,
                         screen.y,
                         screen.w,
                         screen.h,
                         18.0,
                         0.97,
                         0.98,
                         1.0,
                         1.0);
  return screen;
}

static void
pm_setup_clip_screen (cairo_t     *cr,
                      PmSetupRect  screen)
{
  pm_setup_rounded_rect (cr, screen.x, screen.y, screen.w, screen.h, 18.0);
  cairo_clip (cr);
}

static void
pm_setup_draw_status_bar (cairo_t     *cr,
                          PmSetupRect  screen)
{
  double y = screen.y + 17.0;

  pm_setup_draw_text (cr,
                      "9:41",
                      screen.x + 13.0,
                      y,
                      9.0,
                      TRUE,
                      0.28,
                      0.32,
                      0.39,
                      1.0);
  pm_setup_draw_text (cr,
                      "Wi-Fi",
                      screen.x + screen.w - 49.0,
                      y,
                      8.5,
                      TRUE,
                      0.28,
                      0.32,
                      0.39,
                      1.0);
  pm_setup_stroke_rounded (cr,
                           screen.x + screen.w - 22.0,
                           screen.y + 8.5,
                           13.0,
                           7.0,
                           2.0,
                           0.28,
                           0.32,
                           0.39,
                           1.0,
                           1.0);
  pm_setup_fill_rounded (cr,
                         screen.x + screen.w - 19.0,
                         screen.y + 10.5,
                         7.0,
                         3.0,
                         1.5,
                         0.28,
                         0.32,
                         0.39,
                         1.0);
}

static void
pm_setup_draw_nav_bar (cairo_t     *cr,
                       PmSetupRect  screen)
{
  pm_setup_fill_rounded (cr,
                         screen.x + screen.w / 2.0 - 33.0,
                         screen.y + screen.h - 12.0,
                         66.0,
                         4.5,
                         3.0,
                         0.72,
                         0.75,
                         0.80,
                         1.0);
}

static void
pm_setup_draw_app_icon (cairo_t    *cr,
                        double      x,
                        double      y,
                        double      size,
                        const char *label,
                        const char *abbr,
                        double      red,
                        double      green,
                        double      blue)
{
  pm_setup_fill_rounded (cr, x, y, size, size, 11.0, red, green, blue, 1.0);
  pm_setup_draw_text_center (cr,
                             abbr,
                             x + size / 2.0,
                             y + size / 2.0 + 4.0,
                             10.0,
                             TRUE,
                             0.07,
                             0.10,
                             0.14,
                             0.86);
  pm_setup_draw_text_center (cr,
                             label,
                             x + size / 2.0,
                             y + size + 14.0,
                             7.4,
                             FALSE,
                             0.12,
                             0.14,
                             0.18,
                             1.0);
}

static void
pm_setup_draw_home_screen (cairo_t      *cr,
                           PmSetupRect   screen,
                           PmSetupRect  *settings_icon)
{
  const struct {
    const char *label;
    const char *abbr;
    double      red;
    double      green;
    double      blue;
  } apps[] = {
    { N_("Phone"), "Ph", 0.77, 0.94, 0.82 },
    { N_("Messages"), "Msg", 0.81, 0.89, 1.00 },
    { N_("Photos"), "Pic", 1.00, 0.86, 0.72 },
    { N_("Camera"), "Cam", 0.90, 0.86, 1.00 },
    { N_("Settings"), "Set", 0.84, 0.87, 0.92 },
    { N_("Files"), "File", 1.00, 0.94, 0.70 },
  };
  double start_x = screen.x + 19.0;
  double start_y = screen.y + 84.0;
  double size = 32.0;
  double gap_x = 42.0;
  double gap_y = 58.0;

  cairo_save (cr);
  pm_setup_clip_screen (cr, screen);
  cairo_set_source_rgb (cr, 0.90, 0.95, 1.0);
  cairo_paint (cr);
  pm_setup_fill_rounded (cr,
                         screen.x + 14.0,
                         screen.y + 40.0,
                         screen.w - 28.0,
                         26.0,
                         13.0,
                         1.0,
                         1.0,
                         1.0,
                         0.85);
  pm_setup_draw_text (cr,
                      _("Search your phone"),
                      screen.x + 29.0,
                      screen.y + 57.0,
                      9.0,
                      FALSE,
                      0.45,
                      0.49,
                      0.56,
                      1.0);

  for (guint i = 0; i < G_N_ELEMENTS (apps); i++) {
    double x = start_x + (i % 3) * gap_x;
    double y = start_y + (i / 3) * gap_y;

    pm_setup_draw_app_icon (cr,
                            x,
                            y,
                            size,
                            gettext (apps[i].label),
                            apps[i].abbr,
                            apps[i].red,
                            apps[i].green,
                            apps[i].blue);
    if (g_strcmp0 (apps[i].label, "Settings") == 0 && settings_icon != NULL) {
      settings_icon->x = x;
      settings_icon->y = y;
      settings_icon->w = size;
      settings_icon->h = size;
    }
  }

  pm_setup_draw_status_bar (cr, screen);
  pm_setup_draw_nav_bar (cr, screen);
  cairo_restore (cr);
}

static void
pm_setup_draw_switch (cairo_t *cr,
                      double   x,
                      double   y,
                      double   progress)
{
  progress = pm_setup_ease (progress);
  pm_setup_fill_rounded (cr,
                         x,
                         y,
                         26.0,
                         15.0,
                         9.0,
                         pm_setup_mix (0.74, 0.18, progress),
                         pm_setup_mix (0.77, 0.76, progress),
                         pm_setup_mix (0.83, 0.43, progress),
                         1.0);
  pm_setup_fill_rounded (cr,
                         x + 2.0 + progress * 11.0,
                         y + 2.0,
                         11.0,
                         11.0,
                         7.0,
                         1.0,
                         1.0,
                         1.0,
                         1.0);
}

static void
pm_setup_draw_settings_row (cairo_t             *cr,
                            PmSetupRect          screen,
                            double               y,
                            const PmSetupDrawRow *row,
                            gboolean             highlighted)
{
  double x = screen.x + 10.0;
  double w = screen.w - 20.0;
  double h = 35.0;
  double text_x = x + 33.0;
  double text_w = row->has_switch ? w - 73.0 : w - 51.0;
  double title_size = row->has_switch ? 8.2 : 9.0;
  double detail_size = row->has_switch ? 6.4 : 7.2;

  if (y + h < screen.y + 45.0 || y > screen.y + screen.h - 22.0)
    return;

  pm_setup_fill_rounded (cr,
                         x,
                         y,
                         w,
                         h,
                         10.0,
                         highlighted ? 0.84 : 1.0,
                         highlighted ? 0.92 : 1.0,
                         highlighted ? 1.0 : 1.0,
                         1.0);
  pm_setup_stroke_rounded (cr,
                           x,
                           y,
                           w,
                           h,
                           10.0,
                           highlighted ? 0.36 : 0.87,
                           highlighted ? 0.62 : 0.89,
                           highlighted ? 0.92 : 0.93,
                           1.0,
                           1.0);
  pm_setup_fill_rounded (cr,
                         x + 7.0,
                         y + 6.5,
                         22.0,
                         22.0,
                         7.0,
                         highlighted ? 0.62 : 0.91,
                         highlighted ? 0.78 : 0.93,
                         highlighted ? 1.0 : 0.96,
                         1.0);
  pm_setup_draw_text_center (cr,
                             row->icon,
                             x + 18.0,
                             y + 21.5,
                             7.0,
                             TRUE,
                             0.10,
                             0.16,
                             0.24,
                             0.85);
  pm_setup_draw_text_fit (cr,
                          gettext (row->title),
                          text_x,
                          y + 15.0,
                          text_w,
                          title_size,
                          TRUE,
                          highlighted ? 0.03 : 0.09,
                          highlighted ? 0.25 : 0.11,
                          highlighted ? 0.45 : 0.15,
                          1.0);
  if (row->detail != NULL)
    pm_setup_draw_text_fit (cr,
                            gettext (row->detail),
                            text_x,
                            y + 27.0,
                            text_w,
                            detail_size,
                            FALSE,
                            0.39,
                            0.43,
                            0.50,
                            1.0);

  if (row->has_switch)
    pm_setup_draw_switch (cr, x + w - 34.0, y + 10.0, row->switch_progress);
  else
    pm_setup_draw_text (cr,
                        ">",
                        x + w - 15.0,
                        y + 22.0,
                        10.0,
                        TRUE,
                        0.55,
                        0.58,
                        0.64,
                        1.0);
}

static void
pm_setup_draw_settings_screen (cairo_t             *cr,
                               PmSetupRect          screen,
                               const char          *title,
                               const PmSetupDrawRow *rows,
                               guint                n_rows,
                               int                  highlight,
                               double               scroll,
                               gboolean             show_scrollbar,
                               gboolean             show_back)
{
  double content_top = screen.y + 48.0;
  double row_gap = 5.0;
  double row_h = 35.0;
  double title_x = screen.x + (show_back ? 29.0 : 12.0);

  cairo_save (cr);
  pm_setup_clip_screen (cr, screen);
  cairo_set_source_rgb (cr, 0.965, 0.975, 1.0);
  cairo_paint (cr);

  cairo_save (cr);
  cairo_rectangle (cr, screen.x, content_top, screen.w, screen.h - 68.0);
  cairo_clip (cr);
  for (guint i = 0; i < n_rows; i++)
    pm_setup_draw_settings_row (cr,
                                screen,
                                content_top + i * (row_h + row_gap) - scroll,
                                &rows[i],
                                (int) i == highlight);
  cairo_restore (cr);

  pm_setup_fill_rounded (cr,
                         screen.x,
                         screen.y,
                         screen.w,
                         45.0,
                         0.0,
                         0.965,
                         0.975,
                         1.0,
                         1.0);
  pm_setup_draw_status_bar (cr, screen);
  if (show_back)
    pm_setup_draw_text (cr,
                        "<",
                        screen.x + 13.0,
                        screen.y + 38.0,
                        13.0,
                        TRUE,
                        0.07,
                        0.08,
                        0.11,
                        1.0);
  pm_setup_draw_text (cr,
                      title,
                      title_x,
                      screen.y + 38.0,
                      13.0,
                      TRUE,
                      0.07,
                      0.08,
                      0.11,
                      1.0);

  if (show_scrollbar) {
    double track_y = content_top + 2.0;
    double track_h = screen.h - 84.0;
    double thumb_y = track_y + pm_setup_clamp01 (scroll / 118.0) * (track_h - 34.0);

    pm_setup_fill_rounded (cr,
                           screen.x + screen.w - 6.0,
                           track_y,
                           2.0,
                           track_h,
                           1.0,
                           0.78,
                           0.82,
                           0.88,
                           0.8);
    pm_setup_fill_rounded (cr,
                           screen.x + screen.w - 7.0,
                           thumb_y,
                           3.5,
                           34.0,
                           2.0,
                           0.31,
                           0.53,
                           0.77,
                           0.95);
  }

  pm_setup_draw_nav_bar (cr, screen);
  cairo_restore (cr);
}

static void
pm_setup_draw_touch (cairo_t *cr,
                     double   x,
                     double   y,
                     double   progress)
{
  progress = pm_setup_clamp01 (progress);
  if (progress <= 0.0 || progress >= 1.0)
    return;

  cairo_save (cr);
  cairo_set_source_rgba (cr, 0.20, 0.50, 0.86, 0.30 * (1.0 - progress));
  cairo_arc (cr, x, y, 8.0 + progress * 23.0, 0.0, 2.0 * G_PI);
  cairo_fill (cr);
  cairo_set_source_rgba (cr, 0.20, 0.50, 0.86, 0.80 * (1.0 - progress));
  cairo_arc (cr, x, y, 5.0, 0.0, 2.0 * G_PI);
  cairo_fill (cr);
  cairo_restore (cr);
}

static void
pm_setup_draw_drag (cairo_t *cr,
                    double   x,
                    double   y0,
                    double   y1,
                    double   progress)
{
  double y = pm_setup_mix (y0, y1, pm_setup_ease (progress));

  if (progress <= 0.0 || progress >= 1.0)
    return;

  cairo_save (cr);
  cairo_set_source_rgba (cr, 0.20, 0.50, 0.86, 0.28);
  cairo_set_line_width (cr, 3.0);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to (cr, x, y0);
  cairo_line_to (cr, x, y);
  cairo_stroke (cr);
  cairo_arc (cr, x, y, 8.0, 0.0, 2.0 * G_PI);
  cairo_fill (cr);
  cairo_restore (cr);
}

static void
pm_setup_draw_toast (cairo_t     *cr,
                     PmSetupRect  screen,
                     const char  *text,
                     double       progress)
{
  double alpha = pm_setup_ease (progress);
  double w = screen.w - 30.0;
  double x = screen.x + 15.0;
  double y = screen.y + screen.h - 47.0 - (1.0 - alpha) * 8.0;

  if (alpha <= 0.0)
    return;

  pm_setup_fill_rounded (cr, x, y, w, 28.0, 14.0, 0.10, 0.11, 0.14, 0.88 * alpha);
  pm_setup_draw_text_center (cr,
                             text,
                             x + w / 2.0,
                             y + 18.0,
                             8.6,
                             TRUE,
                             1.0,
                             1.0,
                             1.0,
                             alpha);
}

static void
pm_setup_draw_dialog (cairo_t     *cr,
                      PmSetupRect  screen,
                      const char  *title,
                      const char  *body,
                      const char  *positive,
                      double       progress)
{
  double alpha = pm_setup_ease (progress);
  double w = screen.w - 26.0;
  double h = 92.0;
  double x = screen.x + 13.0;
  double y = screen.y + screen.h / 2.0 - h / 2.0 + (1.0 - alpha) * 12.0;
  double pad = 12.0;
  double content_w = w - pad * 2.0;

  if (alpha <= 0.0)
    return;

  cairo_save (cr);
  pm_setup_clip_screen (cr, screen);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.20 * alpha);
  cairo_paint (cr);
  pm_setup_fill_rounded (cr, x, y, w, h, 18.0, 1.0, 1.0, 1.0, alpha);
  pm_setup_stroke_rounded (cr, x, y, w, h, 18.0, 0.80, 0.84, 0.90, alpha, 1.0);
  pm_setup_draw_text_fit (cr,
                          title,
                          x + pad,
                          y + 24.0,
                          content_w,
                          9.4,
                          TRUE,
                          0.08,
                          0.09,
                          0.12,
                          alpha);
  pm_setup_draw_text_fit (cr,
                          body,
                          x + pad,
                          y + 43.0,
                          content_w,
                          7.0,
                          FALSE,
                          0.37,
                          0.40,
                          0.46,
                          alpha);
  pm_setup_draw_text_fit (cr,
                          _("Cancel"),
                          x + w - 76.0,
                          y + h - 17.0,
                          38.0,
                          7.6,
                          TRUE,
                          0.25,
                          0.47,
                          0.78,
                          alpha);
  pm_setup_draw_text_fit (cr,
                          positive,
                          x + w - 34.0,
                          y + h - 17.0,
                          28.0,
                          7.6,
                          TRUE,
                          0.25,
                          0.47,
                          0.78,
                          alpha);
  cairo_restore (cr);
}

static void
pm_setup_draw_desktop (cairo_t    *cr,
                       double      x,
                       double      y,
                       double      w,
                       double      h,
                       const char *title,
                       double      progress)
{
  progress = pm_setup_ease (progress);
  pm_setup_fill_rounded (cr, x, y, w, h, 8.0, 0.93, 0.95, 0.98, 1.0);
  pm_setup_stroke_rounded (cr, x, y, w, h, 8.0, 0.72, 0.76, 0.84, 1.0, 1.0);
  pm_setup_fill_rounded (cr, x, y, w, 17.0, 8.0, 0.12, 0.14, 0.18, 1.0);
  pm_setup_draw_text (cr, title, x + 8.0, y + 12.0, 7.5, TRUE, 1.0, 1.0, 1.0, 1.0);
  pm_setup_fill_rounded (cr,
                         x + 13.0,
                         y + 28.0,
                         (w - 26.0) * progress,
                         18.0,
                         5.0,
                         0.18,
                         0.76,
                         0.43,
                         0.35 + progress * 0.45);
  pm_setup_fill_rounded (cr,
                         x + 13.0,
                         y + 54.0,
                         w - 26.0,
                         6.0,
                         3.0,
                         0.72,
                         0.77,
                         0.84,
                         0.8);
  pm_setup_fill_rounded (cr,
                         x + 13.0,
                         y + 66.0,
                         (w - 26.0) * 0.64,
                         6.0,
                         3.0,
                         0.72,
                         0.77,
                         0.84,
                         0.8);
}

static void
pm_setup_draw_wifi_link (cairo_t *cr,
                         double   x0,
                         double   y0,
                         double   x1,
                         double   y1,
                         double   progress)
{
  double dash = 6.0 + sin (progress * 2.0 * G_PI) * 3.0;

  cairo_save (cr);
  cairo_set_source_rgba (cr, 0.18, 0.76, 0.43, 0.65);
  cairo_set_line_width (cr, 2.0);
  cairo_set_dash (cr, &dash, 1, progress * 12.0);
  cairo_move_to (cr, x0, y0);
  cairo_curve_to (cr, x0 + 26.0, y0 - 28.0, x1 - 30.0, y1 - 24.0, x1, y1);
  cairo_stroke (cr);
  cairo_restore (cr);
}

static void
pm_setup_draw_arrow (cairo_t *cr,
                     double   x0,
                     double   y0,
                     double   cx0,
                     double   cy0,
                     double   cx1,
                     double   cy1,
                     double   x1,
                     double   y1,
                     double   red,
                     double   green,
                     double   blue,
                     double   alpha)
{
  double angle = atan2 (y1 - cy1, x1 - cx1);
  double head = 7.0;

  if (alpha <= 0.0)
    return;

  cairo_save (cr);
  cairo_set_source_rgba (cr, red, green, blue, alpha);
  cairo_set_line_width (cr, 2.2);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to (cr, x0, y0);
  cairo_curve_to (cr, cx0, cy0, cx1, cy1, x1, y1);
  cairo_stroke (cr);

  cairo_move_to (cr, x1, y1);
  cairo_line_to (cr,
                 x1 - head * cos (angle - G_PI / 6.0),
                 y1 - head * sin (angle - G_PI / 6.0));
  cairo_line_to (cr,
                 x1 - head * cos (angle + G_PI / 6.0),
                 y1 - head * sin (angle + G_PI / 6.0));
  cairo_close_path (cr);
  cairo_fill (cr);
  cairo_restore (cr);
}

static void
pm_setup_draw_welcome_scene (cairo_t *cr,
                             double   t)
{
  PmSetupRect phone = { 45.0, 12.0, 172.0, 268.0 };
  PmSetupRect screen = pm_setup_draw_phone_shell (cr, phone.x, phone.y, phone.w, phone.h);
  PmSetupRect settings_icon = { 0 };
  double mirror = pm_setup_ease (pm_setup_interval (t, 1.0, 2.4));

  pm_setup_draw_home_screen (cr, screen, &settings_icon);
  pm_setup_draw_desktop (cr, 205.0, 90.0, 94.0, 88.0, "Specula", mirror);
  pm_setup_draw_wifi_link (cr,
                           phone.x + phone.w - 5.0,
                           phone.y + 95.0,
                           205.0,
                           126.0,
                           t);
  pm_setup_draw_touch (cr,
                       settings_icon.x + settings_icon.w / 2.0,
                       settings_icon.y + settings_icon.h / 2.0,
                       pm_setup_interval (t, 3.0, 3.8));
  pm_setup_draw_text_center (cr,
                             "Android",
                             phone.x + phone.w / 2.0,
                             phone.y + phone.h + 12.0,
                             9.0,
                             TRUE,
                             0.25,
                             0.29,
                             0.35,
                             1.0);
  pm_setup_draw_text_center (cr,
                             "Linux",
                             252.0,
                             192.0,
                             9.0,
                             TRUE,
                             0.25,
                             0.29,
                             0.35,
                             1.0);
}

static void
pm_setup_draw_open_settings_scene (cairo_t *cr,
                                   double   t)
{
  PmSetupRect phone = { 73.0, 10.0, 174.0, 270.0 };
  PmSetupRect screen = pm_setup_draw_phone_shell (cr, phone.x, phone.y, phone.w, phone.h);
  PmSetupRect settings_icon = { 0 };
  double open = pm_setup_ease (pm_setup_interval (t, 1.0, 2.0));

  cairo_save (cr);
  pm_setup_clip_screen (cr, screen);

  cairo_save (cr);
  cairo_translate (cr, -open * screen.w * 0.42, 0.0);
  pm_setup_draw_home_screen (cr, screen, &settings_icon);
  cairo_restore (cr);

  cairo_save (cr);
  cairo_translate (cr, (1.0 - open) * screen.w, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("Settings"),
                                 pm_setup_main_settings_rows,
                                 G_N_ELEMENTS (pm_setup_main_settings_rows),
                                 -1,
                                 0.0,
                                 TRUE,
                                 FALSE);
  cairo_restore (cr);
  cairo_restore (cr);

  pm_setup_draw_touch (cr,
                       settings_icon.x + settings_icon.w / 2.0,
                       settings_icon.y + settings_icon.h / 2.0,
                       pm_setup_interval (t, 0.45, 1.25));
}

static void
pm_setup_draw_about_scene (cairo_t *cr,
                           double   t)
{
  PmSetupRect phone = { 73.0, 10.0, 174.0, 270.0 };
  PmSetupRect screen = pm_setup_draw_phone_shell (cr, phone.x, phone.y, phone.w, phone.h);
  double drag = pm_setup_interval (t, 0.9, 2.8);
  double open = pm_setup_ease (pm_setup_interval (t, 4.0, 4.75));
  double scroll = 104.0 * pm_setup_ease (drag);
  double about_y = screen.y + 48.0 + PM_SETUP_SETTINGS_ROW_ABOUT * 40.0 - scroll + 17.5;

  cairo_save (cr);
  pm_setup_clip_screen (cr, screen);
  cairo_translate (cr, -open * screen.w, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("Settings"),
                                 pm_setup_main_settings_rows,
                                 G_N_ELEMENTS (pm_setup_main_settings_rows),
                                 PM_SETUP_SETTINGS_ROW_ABOUT,
                                 scroll,
                                 TRUE,
                                 FALSE);
  cairo_translate (cr, screen.w, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("About phone"),
                                 pm_setup_about_phone_rows,
                                 G_N_ELEMENTS (pm_setup_about_phone_rows),
                                 -1,
                                 0.0,
                                 FALSE,
                                 TRUE);
  cairo_restore (cr);

  pm_setup_draw_drag (cr,
                      screen.x + screen.w - 24.0,
                      screen.y + 205.0,
                      screen.y + 116.0,
                      drag);
  pm_setup_draw_touch (cr,
                       screen.x + screen.w / 2.0,
                       about_y,
                       pm_setup_interval (t, 3.3, 4.1));
}

static void
pm_setup_draw_build_scene (cairo_t *cr,
                           double   t)
{
  PmSetupRect phone = { 73.0, 10.0, 174.0, 270.0 };
  PmSetupRect screen = pm_setup_draw_phone_shell (cr, phone.x, phone.y, phone.w, phone.h);
  double taps = pm_setup_interval (t, 0.7, 2.45);
  int count = CLAMP ((int) floor (taps * 7.0) + 1, 1, 7);
  double tap_progress = fmod (MAX (t - 0.7, 0.0), 0.25) / 0.25;
  double build_y = screen.y + 48.0 + PM_SETUP_ABOUT_PHONE_ROW_BUILD * 40.0 + 17.5;

  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("About phone"),
                                 pm_setup_about_phone_rows,
                                 G_N_ELEMENTS (pm_setup_about_phone_rows),
                                 PM_SETUP_ABOUT_PHONE_ROW_BUILD,
                                 0.0,
                                 FALSE,
                                 TRUE);
  if (t > 0.7 && t < 2.45) {
    char label[32];
    g_snprintf (label, sizeof label, _("Tap %d of 7"), count);
    pm_setup_draw_touch (cr, screen.x + screen.w / 2.0, build_y, tap_progress);
    pm_setup_fill_rounded (cr,
                           screen.x + 38.0,
                           screen.y + 178.0,
                           screen.w - 76.0,
                           27.0,
                           14.0,
                           0.10,
                           0.11,
                           0.14,
                           0.80);
    pm_setup_draw_text_center (cr,
                               label,
                               screen.x + screen.w / 2.0,
                               screen.y + 195.0,
                               9.0,
                               TRUE,
                               1.0,
                               1.0,
                               1.0,
                               1.0);
  }
  pm_setup_draw_toast (cr,
                       screen,
                       _("You are now a developer!"),
                       pm_setup_interval (t, 2.65, 3.35));
}

static void
pm_setup_draw_usb_scene (cairo_t *cr,
                         double   t)
{
  PmSetupDrawRow developer_rows[PM_SETUP_DEVELOPER_N_ROWS];
  PmSetupRect phone = { 73.0, 10.0, 174.0, 270.0 };
  PmSetupRect screen = pm_setup_draw_phone_shell (cr, phone.x, phone.y, phone.w, phone.h);
  double back_progress = pm_setup_ease (pm_setup_interval (t, 0.65, 1.35));
  double open_developer = pm_setup_ease (pm_setup_interval (t, 2.15, 2.85));
  double switch_progress = pm_setup_ease (pm_setup_interval (t, 3.2, 4.05));
  double settings_scroll = 134.0;
  double developer_y = screen.y + 48.0 + PM_SETUP_SETTINGS_ROW_DEVELOPER * 40.0 - settings_scroll + 17.5;
  double switch_y = screen.y + 48.0 + PM_SETUP_DEVELOPER_ROW_USB * 40.0 + 17.5;
  double settings_x = (back_progress - 1.0 - open_developer) * screen.w;
  double about_x = back_progress * screen.w;
  double developer_x = (1.0 - open_developer) * screen.w;

  pm_setup_developer_rows_for_state (developer_rows, switch_progress, 0.0);

  cairo_save (cr);
  pm_setup_clip_screen (cr, screen);

  cairo_save (cr);
  cairo_translate (cr, settings_x, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("Settings"),
                                 pm_setup_main_settings_rows,
                                 G_N_ELEMENTS (pm_setup_main_settings_rows),
                                 PM_SETUP_SETTINGS_ROW_DEVELOPER,
                                 settings_scroll,
                                 TRUE,
                                 FALSE);
  cairo_restore (cr);

  cairo_save (cr);
  cairo_translate (cr, about_x, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("About phone"),
                                 pm_setup_about_phone_rows,
                                 G_N_ELEMENTS (pm_setup_about_phone_rows),
                                 PM_SETUP_ABOUT_PHONE_ROW_BUILD,
                                 0.0,
                                 FALSE,
                                 TRUE);
  cairo_restore (cr);

  cairo_save (cr);
  cairo_translate (cr, developer_x, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("Developer options"),
                                 developer_rows,
                                 PM_SETUP_DEVELOPER_N_ROWS,
                                 PM_SETUP_DEVELOPER_ROW_USB,
                                 0.0,
                                 FALSE,
                                 FALSE);
  cairo_restore (cr);
  cairo_restore (cr);

  pm_setup_draw_touch (cr,
                       screen.x + 19.0,
                       screen.y + 35.0,
                       pm_setup_interval (t, 0.20, 0.90));
  pm_setup_draw_touch (cr,
                       screen.x + screen.w / 2.0,
                       developer_y,
                       pm_setup_interval (t, 1.55, 2.25));
  pm_setup_draw_touch (cr,
                       screen.x + screen.w - 34.0,
                       switch_y,
                       pm_setup_interval (t, 3.0, 3.75));
  pm_setup_draw_dialog (cr,
                        screen,
                        _("Allow USB debugging?"),
                        _("Computer RSA key fingerprint"),
                        _("OK"),
                        pm_setup_interval (t, 4.2, 5.0));
  pm_setup_draw_touch (cr,
                       screen.x + screen.w - 36.0,
                       screen.y + screen.h / 2.0 + 39.0,
                       pm_setup_interval (t, 5.25, 6.0));
}

static void
pm_setup_draw_wireless_scene (cairo_t *cr,
                              double   t)
{
  PmSetupDrawRow developer_rows[PM_SETUP_DEVELOPER_N_ROWS];
  PmSetupDrawRow wireless_rows[] = {
    { "Wi", N_("Use wireless debugging"), N_("Current Wi-Fi network"), TRUE, 0.0 },
    { "Net", N_("Wi-Fi network"), N_("Same network as Linux"), FALSE, 0.0 },
    { "Name", N_("Device name"), N_("Android device"), FALSE, 0.0 },
  };
  PmSetupRect phone = { 73.0, 10.0, 174.0, 270.0 };
  PmSetupRect screen = pm_setup_draw_phone_shell (cr, phone.x, phone.y, phone.w, phone.h);
  double open = pm_setup_ease (pm_setup_interval (t, 0.65, 1.35));
  double switch_progress = pm_setup_ease (pm_setup_interval (t, 2.0, 2.9));

  pm_setup_developer_rows_for_state (developer_rows, 1.0, switch_progress * 0.35);
  wireless_rows[0].switch_progress = switch_progress;

  cairo_save (cr);
  pm_setup_clip_screen (cr, screen);
  cairo_translate (cr, -open * screen.w, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("Developer options"),
                                 developer_rows,
                                 PM_SETUP_DEVELOPER_N_ROWS,
                                 PM_SETUP_DEVELOPER_ROW_WIRELESS,
                                 0.0,
                                 FALSE,
                                 FALSE);
  cairo_translate (cr, screen.w, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("Wireless debugging"),
                                 wireless_rows,
                                 G_N_ELEMENTS (wireless_rows),
                                 0,
                                 0.0,
                                 FALSE,
                                 FALSE);
  cairo_restore (cr);

  pm_setup_draw_touch (cr,
                       screen.x + screen.w / 2.0,
                       screen.y + 48.0 + PM_SETUP_DEVELOPER_ROW_WIRELESS * 40.0 + 17.5,
                       pm_setup_interval (t, 0.30, 0.95));
  pm_setup_draw_touch (cr,
                       screen.x + screen.w - 34.0,
                       screen.y + 48.0 + 17.5,
                       pm_setup_interval (t, 1.75, 2.45));
  pm_setup_draw_dialog (cr,
                        screen,
                        _("Allow wireless debugging?"),
                        _("Allow on this Wi-Fi network"),
                        _("Allow"),
                        pm_setup_interval (t, 3.1, 3.9));
  pm_setup_draw_touch (cr,
                       screen.x + screen.w - 38.0,
                       screen.y + screen.h / 2.0 + 39.0,
                       pm_setup_interval (t, 4.4, 5.2));
}

static void
pm_setup_draw_pairing_phone_screen (cairo_t     *cr,
                                    PmSetupRect  screen,
                                    double       opened)
{
  PmSetupDrawRow wireless_rows[] = {
    { "Wi", N_("Use wireless debugging"), N_("On"), TRUE, 1.0 },
    { "Pair", N_("Pair device with code"), N_("Shows code and address"), FALSE, 0.0 },
    { "IP", N_("IP address & port"), "192.168.1.25:5555", FALSE, 0.0 },
  };
  PmSetupDrawRow code_rows[] = {
    { "Code", "123456", N_("Pairing code"), FALSE, 0.0 },
    { "IP", "192.168.1.25:37123", N_("Pairing address"), FALSE, 0.0 },
    { "Time", N_("Keep this screen open"), N_("Code expires soon"), FALSE, 0.0 },
  };

  cairo_save (cr);
  pm_setup_clip_screen (cr, screen);
  cairo_translate (cr, -opened * screen.w, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("Wireless debugging"),
                                 wireless_rows,
                                 G_N_ELEMENTS (wireless_rows),
                                 1,
                                 0.0,
                                 FALSE,
                                 FALSE);
  cairo_translate (cr, screen.w, 0.0);
  pm_setup_draw_settings_screen (cr,
                                 screen,
                                 _("Pair device"),
                                 code_rows,
                                 G_N_ELEMENTS (code_rows),
                                 0,
                                 0.0,
                                 FALSE,
                                 FALSE);
  cairo_restore (cr);
}

static void
pm_setup_draw_adw_entry_row (cairo_t     *cr,
                             PmSetupRect  row,
                             const char  *title,
                             const char  *value,
                             double       fill_progress)
{
  double progress = pm_setup_ease (fill_progress);
  gboolean has_value = value != NULL && progress > 0.0;

  if (has_value) {
    pm_setup_draw_text_fit (cr,
                            title,
                            row.x + 8.0,
                            row.y + 11.0,
                            row.w - 16.0,
                            4.7,
                            FALSE,
                            0.67,
                            0.67,
                            0.70,
                            1.0);
    pm_setup_draw_text_fit (cr,
                            value,
                            row.x + 8.0,
                            row.y + 23.5,
                            row.w - 16.0,
                            6.5,
                            FALSE,
                            1.0,
                            1.0,
                            1.0,
                            0.96 * progress);
  } else {
    pm_setup_draw_text_fit (cr,
                            title,
                            row.x + 8.0,
                            row.y + 17.8,
                            row.w - 16.0,
                            5.7,
                            FALSE,
                            0.67,
                            0.67,
                            0.70,
                            1.0);
  }
}

static void
pm_setup_draw_adw_entry_group (cairo_t     *cr,
                               PmSetupRect  group,
                               const char  *top_title,
                               const char  *top_value,
                               double       top_progress,
                               const char  *bottom_title,
                               const char  *bottom_value,
                               double       bottom_progress)
{
  PmSetupRect top = { group.x, group.y, group.w, group.h / 2.0 };
  PmSetupRect bottom = { group.x, group.y + group.h / 2.0, group.w, group.h / 2.0 };

  pm_setup_fill_rounded (cr,
                         group.x,
                         group.y,
                         group.w,
                         group.h,
                         6.0,
                         0.215,
                         0.215,
                         0.235,
                         1.0);
  pm_setup_stroke_rounded (cr,
                           group.x,
                           group.y,
                           group.w,
                           group.h,
                           6.0,
                           0.15,
                           0.15,
                           0.17,
                           1.0,
                           0.9);

  cairo_save (cr);
  cairo_set_source_rgba (cr, 0.11, 0.11, 0.13, 0.82);
  cairo_set_line_width (cr, 0.8);
  cairo_move_to (cr, group.x, group.y + group.h / 2.0);
  cairo_line_to (cr, group.x + group.w, group.y + group.h / 2.0);
  cairo_stroke (cr);
  cairo_restore (cr);

  pm_setup_draw_adw_entry_row (cr, top, top_title, top_value, top_progress);
  pm_setup_draw_adw_entry_row (cr, bottom, bottom_title, bottom_value, bottom_progress);
}

static void
pm_setup_draw_connect_device_sheet (cairo_t     *cr,
                                    double       x,
                                    double       y,
                                    double       w,
                                    double       h,
                                    double       copy_progress,
                                    PmSetupRect *addr_row,
                                    PmSetupRect *code_row)
{
  PmSetupRect pair_group = { x + 8.0, y + 74.0, w - 16.0, 57.0 };
  PmSetupRect addr = { pair_group.x, pair_group.y, pair_group.w, pair_group.h / 2.0 };
  PmSetupRect code = {
    pair_group.x, pair_group.y + pair_group.h / 2.0, pair_group.w, pair_group.h / 2.0
  };

  pm_setup_fill_rounded (cr, x, y, w, h, 10.0, 0.12, 0.12, 0.14, 1.0);
  pm_setup_stroke_rounded (cr, x, y, w, h, 10.0, 0.25, 0.25, 0.29, 1.0, 0.9);

  cairo_save (cr);
  cairo_set_source_rgba (cr, 0.95, 0.25, 0.29, 1.0);
  cairo_arc (cr, x + 12.0, y + 16.0, 5.0, 0.0, G_PI * 2.0);
  cairo_fill (cr);
  cairo_set_source_rgba (cr, 0.27, 0.27, 0.31, 0.72);
  cairo_set_line_width (cr, 0.8);
  cairo_move_to (cr, x, y + 31.0);
  cairo_line_to (cr, x + w, y + 31.0);
  cairo_stroke (cr);
  cairo_restore (cr);

  pm_setup_draw_text_center (cr,
                             _("Set up device"),
                             x + w / 2.0,
                             y + 19.5,
                             7.2,
                             TRUE,
                             1.0,
                             1.0,
                             1.0,
                             1.0);

  pm_setup_draw_text (cr,
                      _("Pairing"),
                      x + 9.0,
                      y + 52.0,
                      7.2,
                      TRUE,
                      1.0,
                      1.0,
                      1.0,
                      1.0);
  pm_setup_draw_text_fit (cr,
                          _("Android 11+: Pair device with code."),
                          x + 9.0,
                          y + 63.0,
                          w - 18.0,
                          5.0,
                          FALSE,
                          0.68,
                          0.68,
                          0.71,
                          1.0);
  pm_setup_draw_adw_entry_group (cr,
                                 pair_group,
                                 _("Pairing address (ip:port)"),
                                 "192.168.1.25:37123",
                                 pm_setup_interval (copy_progress, 0.0, 0.55),
                                 _("Pairing code"),
                                 "123456",
                                 pm_setup_interval (copy_progress, 0.35, 0.9));
  pm_setup_fill_rounded (cr,
                         x + 30.0,
                         y + 139.0,
                         w - 60.0,
                         21.0,
                         10.5,
                         0.21,
                         0.52,
                         0.90,
                         1.0);
  pm_setup_draw_text_center (cr,
                             _("Pair & Connect"),
                             x + w / 2.0,
                             y + 153.0,
                             5.8,
                             TRUE,
                             1.0,
                             1.0,
                             1.0,
                             0.95);

  if (addr_row != NULL)
    *addr_row = addr;
  if (code_row != NULL)
    *code_row = code;
}

static void
pm_setup_draw_pairing_scene (cairo_t *cr,
                             double   t)
{
  PmSetupRect phone = { 8.0, 13.0, 145.0, 260.0 };
  PmSetupRect screen = pm_setup_draw_phone_shell (cr, phone.x, phone.y, phone.w, phone.h);
  PmSetupRect addr_row = { 0 };
  PmSetupRect code_row = { 0 };
  double open = pm_setup_ease (pm_setup_interval (t, 0.65, 1.35));
  double copy = pm_setup_ease (pm_setup_interval (t, 2.4, 4.0));
  double desktop_x = 162.0;
  double desktop_y = 5.0;

  pm_setup_draw_pairing_phone_screen (cr, screen, open);
  pm_setup_draw_touch (cr,
                       screen.x + screen.w / 2.0,
                       screen.y + 48.0 + 1.0 * 40.0 + 17.5,
                       pm_setup_interval (t, 0.25, 0.95));

  pm_setup_draw_connect_device_sheet (cr,
                                      desktop_x,
                                      desktop_y,
                                      152.0,
                                      180.0,
                                      copy,
                                      &addr_row,
                                      &code_row);
  pm_setup_draw_arrow (cr,
                       screen.x + screen.w - 8.0,
                       screen.y + 48.0 + 1.0 * 40.0 + 17.5,
                       168.0,
                       114.0,
                       desktop_x - 6.0,
                       addr_row.y + addr_row.h / 2.0,
                       addr_row.x - 2.0,
                       addr_row.y + addr_row.h / 2.0,
                       0.20,
                       0.50,
                       0.86,
                       0.75 * pm_setup_interval (copy, 0.0, 0.65));
  pm_setup_draw_arrow (cr,
                       screen.x + screen.w - 8.0,
                       screen.y + 48.0 + 0.0 * 40.0 + 17.5,
                       170.0,
                       74.0,
                       desktop_x - 8.0,
                       code_row.y + code_row.h / 2.0,
                       code_row.x - 2.0,
                       code_row.y + code_row.h / 2.0,
                       0.18,
                       0.67,
                       0.38,
                       0.75 * pm_setup_interval (copy, 0.35, 1.0));
}

static double
pm_setup_cycle_for_step (PmSetupStep step)
{
  switch (step) {
    case PM_SETUP_BUILD_NUMBER:
      return 4.4;
    case PM_SETUP_ABOUT:
    case PM_SETUP_USB_DEBUGGING:
    case PM_SETUP_WIRELESS_DEBUGGING:
    case PM_SETUP_PAIRING:
      return 6.2;
    case PM_SETUP_WELCOME:
    case PM_SETUP_SETTINGS:
    case PM_SETUP_N_STEPS:
      break;
  }

  return 5.4;
}

static void
pm_setup_animation_draw (GtkDrawingArea *area,
                         cairo_t        *cr,
                         int             width,
                         int             height,
                         gpointer        user_data)
{
  PmSetupAnimation *animation = user_data;
  double canvas_w = 320.0;
  double canvas_h = 292.0;
  double scale = MIN (width / canvas_w, height / canvas_h);
  double x = (width - canvas_w * scale) / 2.0;
  double y = (height - canvas_h * scale) / 2.0;
  double elapsed = (g_get_monotonic_time () - animation->start_us) / 1000000.0;
  double cycle = pm_setup_cycle_for_step (animation->step);
  double t = fmod (elapsed, cycle);

  cairo_save (cr);
  cairo_translate (cr, x, y);
  cairo_scale (cr, scale, scale);

  switch (animation->step) {
    case PM_SETUP_WELCOME:
      pm_setup_draw_welcome_scene (cr, t);
      break;
    case PM_SETUP_SETTINGS:
      pm_setup_draw_open_settings_scene (cr, t);
      break;
    case PM_SETUP_ABOUT:
      pm_setup_draw_about_scene (cr, t);
      break;
    case PM_SETUP_BUILD_NUMBER:
      pm_setup_draw_build_scene (cr, t);
      break;
    case PM_SETUP_USB_DEBUGGING:
      pm_setup_draw_usb_scene (cr, t);
      break;
    case PM_SETUP_WIRELESS_DEBUGGING:
      pm_setup_draw_wireless_scene (cr, t);
      break;
    case PM_SETUP_PAIRING:
      pm_setup_draw_pairing_scene (cr, t);
      break;
    case PM_SETUP_N_STEPS:
      break;
  }

  cairo_restore (cr);
}

static gboolean
pm_setup_animation_frame (GtkWidget     *widget,
                          GdkFrameClock *clock,
                          gpointer       user_data)
{
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static void
pm_setup_animation_free (gpointer data)
{
  g_free (data);
}

static GtkWidget *
setup_animation_for_step (PmWindow    *self,
                          PmSetupStep  step)
{
  GtkWidget *area = gtk_drawing_area_new ();
  PmSetupAnimation *animation = g_new0 (PmSetupAnimation, 1);

  animation->step = step;
  animation->start_us = g_get_monotonic_time ();

  gtk_widget_add_css_class (area, "specula-setup-animation");
  gtk_widget_set_halign (area, GTK_ALIGN_CENTER);
  self->setup_animation_areas[step] = area;
  g_object_set_data (G_OBJECT (area), "specula-setup-animation", animation);
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (area), 320);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (area), 292);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area),
                                  pm_setup_animation_draw,
                                  animation,
                                  NULL);
  gtk_widget_add_tick_callback (area,
                                pm_setup_animation_frame,
                                animation,
                                pm_setup_animation_free);

  return area;
}

static void
pm_window_reset_setup_animation (PmWindow    *self,
                                 PmSetupStep  step)
{
  GtkWidget *area;
  PmSetupAnimation *animation;

  if (step >= PM_SETUP_N_STEPS)
    return;

  area = self->setup_animation_areas[step];
  if (area == NULL)
    return;

  animation = g_object_get_data (G_OBJECT (area), "specula-setup-animation");
  if (animation == NULL)
    return;

  animation->start_us = g_get_monotonic_time ();
  gtk_widget_queue_draw (area);
}

static void
pm_window_show_setup_step (PmWindow *self, guint step)
{
  guint previous = self->setup_step;
  self->setup_step = MIN (step, PM_SETUP_N_STEPS - 1);

  /* The whole wizard lives on a single page; the step lives in the inner
   * stacks. The instruction text always slides: SLIDE_LEFT_RIGHT picks the
   * direction from child order automatically, so a later step slides in from
   * the right (toward the left) and an earlier step slides back from the left.
   *
   * The animation crossfades between steps, with one exception: the welcome
   * logo slides in/out so entering the wizard (welcome -> step 1) and stepping
   * back to it feels continuous. Only transitions that touch the welcome step
   * slide; every other animation swap fades. */
  adw_view_stack_set_visible_child_name (self->stack, "setup");

  gboolean welcome_involved = (previous == PM_SETUP_WELCOME ||
                               self->setup_step == PM_SETUP_WELCOME);
  gtk_stack_set_transition_type (self->setup_anim_stack,
                                 welcome_involved
                                   ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT
                                   : GTK_STACK_TRANSITION_TYPE_CROSSFADE);

  gtk_stack_set_visible_child_name (self->setup_anim_stack,
                                    pm_setup_page_names[self->setup_step]);
  gtk_stack_set_visible_child_name (self->setup_instruction_stack,
                                    pm_setup_page_names[self->setup_step]);

  /* Buttons are anchored once and never move between steps; only their state
   * changes: no Back on the first step, and the final step finishes setup. */
  gtk_widget_set_visible (self->setup_back_button,
                          self->setup_step != PM_SETUP_WELCOME);
  gtk_button_set_label (GTK_BUTTON (self->setup_next_button),
                        self->setup_step + 1 >= PM_SETUP_N_STEPS
                          ? _("Go to Home")
                          : _("Continue"));

  pm_window_reset_setup_animation (self, self->setup_step);
  adw_window_title_set_title (self->title, _("First Time Setup"));
  adw_window_title_set_subtitle (self->title, NULL);
}

static void
pm_window_finish_setup (PmWindow *self)
{
  if (self->setup_review_active) {
    self->setup_review_active = FALSE;
    if (self->session != NULL)
      pm_window_apply_state (self, pm_session_get_state (self->session), NULL);
    else
      pm_window_apply_state (self, PM_STATE_IDLE, NULL);
    return;
  }

  self->setup_complete = TRUE;
  pm_window_save_prefs (self);
  pm_window_apply_state (self, PM_STATE_IDLE, NULL);
}

static void
on_setup_next_clicked (GtkButton *button, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  if (self->setup_step + 1 >= PM_SETUP_N_STEPS) {
    pm_window_finish_setup (self);
    return;
  }

  pm_window_show_setup_step (self, self->setup_step + 1);
}

static void
on_setup_back_clicked (GtkButton *button, gpointer user_data)
{
  PmWindow *self = PM_WINDOW (user_data);

  if (self->setup_step == 0)
    return;

  pm_window_show_setup_step (self, self->setup_step - 1);
}

static GtkWidget *
setup_nav_button_new (const char *label,
                      gboolean    suggested,
                      GCallback   callback,
                      gpointer    user_data)
{
  GtkWidget *button = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (button, "pill");
  if (suggested)
    gtk_widget_add_css_class (button, "suggested-action");
  gtk_widget_set_hexpand (button, TRUE);
  g_signal_connect (button, "clicked", callback, user_data);
  return button;
}

typedef struct {
  const char *title;
  const char *body;
} PmSetupText;

static const PmSetupText pm_setup_texts[PM_SETUP_N_STEPS] = {
  [PM_SETUP_WELCOME] = {
    N_("Welcome to Phone Mirror"),
    N_("We will help you prepare your Android phone. Keep your phone nearby and connect it to the same Wi-Fi network as this computer."),
  },
  [PM_SETUP_SETTINGS] = {
    N_("Open Settings"),
    N_("Unlock your Android phone. Find the Settings app and open it."),
  },
  [PM_SETUP_ABOUT] = {
    N_("Find About phone"),
    N_("In Settings, scroll toward the bottom. Tap About phone. On some phones, open System first, then tap About phone."),
  },
  [PM_SETUP_BUILD_NUMBER] = {
    N_("Turn on Developer options"),
    N_("Find Build number. Tap Build number seven times. If Android asks for your PIN, enter it. Stop when the phone says you are now a developer."),
  },
  [PM_SETUP_USB_DEBUGGING] = {
    N_("Turn on USB debugging"),
    N_("Go back to Settings. Open Developer options. On some phones, open System first, then turn on USB debugging. If Android asks, tap OK."),
  },
  [PM_SETUP_WIRELESS_DEBUGGING] = {
    N_("Turn on Wireless debugging"),
    N_("Stay in Developer options. Turn on Wireless debugging. Keep your phone and this computer on the same network. If Android asks, tap Allow."),
  },
  [PM_SETUP_PAIRING] = {
    N_("Get the pairing code"),
    N_("Tap Wireless debugging, then Pair device with pairing code. Leave that screen open. On the home page, choose Set Up Device, enter the pairing address and code, then choose Pair & Connect."),
  },
};

/* The instruction text for one step: the "Step N of M" eyebrow, the title and
 * the body. These live in the sliding stack, so they swipe as a unit. */
static GtkWidget *
setup_instruction_new (PmSetupStep step)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_valign (box, GTK_ALIGN_START);

  g_autofree char *step_text =
    step == PM_SETUP_WELCOME
      ? g_strdup (_("Welcome"))
      : g_strdup_printf (_("Step %u of %u"), step, PM_SETUP_N_STEPS - 1);
  GtkWidget *step_label = phone_label_new (step_text, "specula-setup-step");
  gtk_label_set_xalign (GTK_LABEL (step_label), 0.5);
  gtk_box_append (GTK_BOX (box), step_label);

  GtkWidget *title_label = phone_label_new (gettext (pm_setup_texts[step].title),
                                            "specula-setup-title");
  gtk_label_set_xalign (GTK_LABEL (title_label), 0.5);
  gtk_label_set_justify (GTK_LABEL (title_label), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (box), title_label);

  GtkWidget *body_label = phone_label_new (gettext (pm_setup_texts[step].body),
                                           "specula-setup-body");
  gtk_label_set_xalign (GTK_LABEL (body_label), 0.5);
  gtk_label_set_justify (GTK_LABEL (body_label), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (box), body_label);

  return box;
}

/* The wizard is one page split into three independent regions:
 *   - the animation stack swaps instantly with the step,
 *   - the instruction stack slides between steps (direction from child order),
 *   - the button row is anchored once and never moves between steps.
 * Both inner stacks are homogeneous so the page geometry - and therefore the
 * vertical position of the buttons - stays fixed across every step. */
static GtkWidget *
setup_root_new (PmWindow *self)
{
  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (outer, 8);
  gtk_widget_set_margin_bottom (outer, 8);
  gtk_widget_set_margin_start (outer, 14);
  gtk_widget_set_margin_end (outer, 14);
  gtk_widget_set_valign (outer, GTK_ALIGN_CENTER);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 360);
  gtk_widget_set_hexpand (clamp, TRUE);
  gtk_box_append (GTK_BOX (outer), clamp);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (content, "specula-setup-card");
  gtk_widget_set_valign (content, GTK_ALIGN_CENTER);
  adw_clamp_set_child (ADW_CLAMP (clamp), content);

  /* Animation stack: instant swap between most steps (see show_setup_step),
   * kept in lockstep with the step. The welcome step is not an animation but
   * the app logo. */
  self->setup_anim_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->setup_anim_stack,
                                 GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_set_transition_duration (self->setup_anim_stack, 280);
  gtk_widget_set_halign (GTK_WIDGET (self->setup_anim_stack), GTK_ALIGN_CENTER);
  for (guint i = 0; i < PM_SETUP_N_STEPS; i++) {
    GtkWidget *child;
    if (i == PM_SETUP_WELCOME) {
      child = gtk_image_new_from_icon_name (PM_APP_ID);
      gtk_image_set_pixel_size (GTK_IMAGE (child), 180);
      gtk_widget_set_halign (child, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (child, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (child, "specula-setup-animation");
    } else {
      child = setup_animation_for_step (self, (PmSetupStep) i);
    }
    gtk_stack_add_named (self->setup_anim_stack, child, pm_setup_page_names[i]);
  }
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->setup_anim_stack));

  /* Instruction stack: slides left toward the next step, right toward the
   * previous one. SLIDE_LEFT_RIGHT derives the direction from child order, so
   * adding the steps in order is all that's needed. Homogeneous keeps a fixed
   * height regardless of which body is longest. */
  self->setup_instruction_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->setup_instruction_stack,
                                 GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_stack_set_transition_duration (self->setup_instruction_stack, 280);
  gtk_stack_set_hhomogeneous (self->setup_instruction_stack, TRUE);
  gtk_stack_set_vhomogeneous (self->setup_instruction_stack, TRUE);
  for (guint i = 0; i < PM_SETUP_N_STEPS; i++)
    gtk_stack_add_named (self->setup_instruction_stack,
                         setup_instruction_new ((PmSetupStep) i),
                         pm_setup_page_names[i]);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->setup_instruction_stack));

  /* Buttons: anchored below the stacks so their vertical position is identical
   * on every step. Back is hidden (not removed) on the first step so the row's
   * height never changes. */
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign (buttons, GTK_ALIGN_FILL);
  gtk_widget_set_margin_top (buttons, 6);

  self->setup_back_button =
    setup_nav_button_new (_("Back"), FALSE,
                          G_CALLBACK (on_setup_back_clicked), self);
  gtk_box_append (GTK_BOX (buttons), self->setup_back_button);

  self->setup_next_button =
    setup_nav_button_new (_("Continue"), TRUE,
                          G_CALLBACK (on_setup_next_clicked), self);
  gtk_box_append (GTK_BOX (buttons), self->setup_next_button);

  gtk_box_append (GTK_BOX (content), buttons);

  return outer;
}

/* --- construction --------------------------------------------------------- */

/* Append an item that hides itself when its backing action is disabled, so a
 * single menu can serve both the connected and main-menu states: toggling the
 * action's enabled flag in pm_window_apply_menu_state() makes the row follow. */
static void
menu_append_state_item (GMenu      *section,
                        const char *label,
                        const char *action)
{
  GMenuItem *item = g_menu_item_new (label, action);
  g_menu_item_set_attribute (item, "hidden-when", "s", "action-disabled");
  g_menu_append_item (section, item);
  g_object_unref (item);
}

static GtkWidget *
build_header_menu (void)
{
  GMenu *menu = g_menu_new ();

  /* State-dependent top section. Disconnect + Lockscreen PIN show only while
   * mirroring; First Time Setup shows only in the main menu. They are mutually
   * exclusive, so this one section renders correctly in either state and its
   * separator disappears with it when empty. */
  GMenu *state_section = g_menu_new ();
  menu_append_state_item (state_section, _("_Disconnect"), "win.disconnect");
  menu_append_state_item (state_section, _("_Lockscreen PIN…"), "win.lockscreen-pin");
  menu_append_state_item (state_section, _("_First Time Setup"), "win.first-setup");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (state_section));
  g_object_unref (state_section);

  /* Connection actions - always available. */
  GMenu *connect_section = g_menu_new ();
  g_menu_append (connect_section, _("_Manual Connection…"), "win.manual-connect");
  g_menu_append (connect_section, _("_Set Up Device…"), "win.setup");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (connect_section));
  g_object_unref (connect_section);

  /* App-level entries. */
  GMenu *app_section = g_menu_new ();
  g_menu_append (app_section, _("_Settings"), "win.settings");
  g_menu_append (app_section, _("_About Phone Mirror"), "app.about");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (app_section));
  g_object_unref (app_section);

  GMenu *quit_section = g_menu_new ();
  g_menu_append (quit_section, _("_Quit"), "app.quit");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (quit_section));
  g_object_unref (quit_section);

  GtkWidget *button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (button), G_MENU_MODEL (menu));
  g_object_unref (menu);
  return button;
}

static void
pm_window_dispose (GObject *object)
{
  PmWindow *self = PM_WINDOW (object);

  pm_window_cancel_chrome_tick (self);
  g_clear_object (&self->session);
  G_OBJECT_CLASS (pm_window_parent_class)->dispose (object);
}

static gboolean
pm_window_close_request (GtkWindow *window)
{
  PmWindow *self = PM_WINDOW (window);
  if (self->session != NULL)
    pm_session_stop (self->session);
  return GDK_EVENT_PROPAGATE;
}

static void
pm_window_class_init (PmWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pm_window_dispose;
  GTK_WINDOW_CLASS (klass)->close_request = pm_window_close_request;
}

static void
pm_window_init (PmWindow *self)
{
  /* Restore persisted preferences (defaults: mouse mode on, aspect lock on). */
  PmPrefs prefs;
  pm_prefs_load (&prefs);
  self->free_resize = prefs.free_resize;
  self->mouse_mode = prefs.mouse_mode;
  self->audio = prefs.audio;
  self->video_bitrate = prefs.video_bitrate;
  self->display_mode = prefs.display_mode;
  self->display_width = prefs.display_width;
  self->display_height = prefs.display_height;
  self->display_dpi = prefs.display_dpi;
  self->screen_off = prefs.screen_off;
  self->setup_complete = prefs.setup_complete;

  gtk_window_set_title (GTK_WINDOW (self), _("Phone Mirror"));
  gtk_window_set_default_size (GTK_WINDOW (self), 420, 760);

  /* Window actions. Register before building action-backed menu widgets. */
  static const GActionEntry win_actions[] = {
    { "setup", on_setup_action, NULL, NULL, NULL },
    { "manual-connect", on_manual_connect_action, NULL, NULL, NULL },
    { "disconnect", on_disconnect_action, NULL, NULL, NULL },
    { "first-setup", on_first_setup_action, NULL, NULL, NULL },
    { "lockscreen-pin", on_lockscreen_pin_action, NULL, NULL, NULL },
    { "settings", on_settings_action, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   win_actions, G_N_ELEMENTS (win_actions),
                                   self);
  pm_window_set_disconnect_available (self, FALSE);

  /* Header bar */
  self->header_bar = ADW_HEADER_BAR (adw_header_bar_new ());
  self->title = ADW_WINDOW_TITLE (adw_window_title_new (_("Phone Mirror"), NULL));
  adw_header_bar_set_title_widget (self->header_bar, GTK_WIDGET (self->title));

  pm_window_install_pin_css (self);
  pm_window_install_setup_css (self);
  pm_window_install_resize_css (self);

  self->pin_button = GTK_TOGGLE_BUTTON (gtk_toggle_button_new ());
  self->pin_image = GTK_IMAGE (gtk_image_new ());
  gtk_image_set_pixel_size (self->pin_image, 16);
  gtk_button_set_child (GTK_BUTTON (self->pin_button), GTK_WIDGET (self->pin_image));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->pin_button), _("Keep controls visible"));
  gtk_widget_add_css_class (GTK_WIDGET (self->pin_button), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (self->pin_button), "image-button");
  gtk_widget_add_css_class (GTK_WIDGET (self->pin_button), "specula-pin-button");
  gtk_widget_set_visible (GTK_WIDGET (self->pin_button), FALSE);
  pm_window_update_pin_icon (self);
  g_signal_connect (self->pin_button, "toggled", G_CALLBACK (on_pin_toggled), self);
  adw_header_bar_pack_end (self->header_bar, GTK_WIDGET (self->pin_button));
  adw_header_bar_pack_end (self->header_bar, build_header_menu ());

  /* --- Page: searching/connecting ------------------------------------- */
  self->status_page = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (self->status_page, "phone-symbolic");
  adw_status_page_set_title (self->status_page, "Phone Mirror");
  adw_status_page_set_description (self->status_page,
    _("Press Connect to look for your paired device."));

  self->spinner = GTK_SPINNER (gtk_spinner_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->spinner), 32, 32);

  self->connect_button = GTK_BUTTON (gtk_button_new_with_label (_("Connect")));
  gtk_widget_add_css_class (GTK_WIDGET (self->connect_button), "pill");
  gtk_widget_add_css_class (GTK_WIDGET (self->connect_button), "suggested-action");
  gtk_widget_set_halign (GTK_WIDGET (self->connect_button), GTK_ALIGN_CENTER);
  g_signal_connect (self->connect_button, "clicked",
                    G_CALLBACK (on_connect_clicked), self);

  GtkWidget *setup_button = gtk_button_new_with_label (_("Set Up Device…"));
  gtk_widget_add_css_class (setup_button, "pill");
  gtk_widget_set_halign (setup_button, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (setup_button), "win.setup");

  GtkBox *status_extra = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  gtk_box_append (status_extra, GTK_WIDGET (self->spinner));
  gtk_box_append (status_extra, GTK_WIDGET (self->connect_button));
  gtk_box_append (status_extra, setup_button);
  adw_status_page_set_child (self->status_page, GTK_WIDGET (status_extra));

  /* --- Page: live mirror ---------------------------------------------- */
  self->video_view = pm_video_view_new ();
  self->mirror_frame = GTK_ASPECT_FRAME (gtk_aspect_frame_new (0.5, 0.0, 9.0 / 16.0, FALSE));
  gtk_widget_add_css_class (GTK_WIDGET (self->mirror_frame), "background");
  gtk_widget_set_hexpand (GTK_WIDGET (self->mirror_frame), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->mirror_frame), TRUE);
  gtk_aspect_frame_set_child (self->mirror_frame, GTK_WIDGET (self->video_view));

  /* --- View stack ----------------------------------------------------- */
  self->stack = ADW_VIEW_STACK (adw_view_stack_new ());
  adw_view_stack_set_transition_duration (self->stack, 280);
  /* Crossfade between top-level pages. Without this AdwViewStack swaps pages
   * instantly; enabling it animates every page switch, including the wizard's
   * final "Go to Home" handoff from the setup page to the home page. */
  adw_view_stack_set_enable_transitions (self->stack, TRUE);
  /* Don't force every page to share the largest page's size; the mirror page
   * needs to keep its own aspect ratio rather than the setup pages' geometry. */
  adw_view_stack_set_hhomogeneous (self->stack, FALSE);
  adw_view_stack_set_vhomogeneous (self->stack, FALSE);
  adw_view_stack_add_named (self->stack, setup_root_new (self), "setup");
  adw_view_stack_add_named (self->stack, GTK_WIDGET (self->status_page), "searching");
  adw_view_stack_add_named (self->stack, GTK_WIDGET (self->mirror_frame), "mirror");

  /* --- Toolbar shell -------------------------------------------------- */
  self->toolbar_view = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (self->toolbar_view, GTK_WIDGET (self->header_bar));
  adw_toolbar_view_set_content (self->toolbar_view, GTK_WIDGET (self->stack));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      GTK_WIDGET (self->toolbar_view));

  /* Reveal the header again on motion while mirroring. */
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  /* Live aspect-locked resize: drive the size from an edge-drag so
   * the window stays on the phone aspect throughout the drag (the window is
   * non-resizable while locked, so the compositor never free-resizes it).
   * Capture phase so an edge grab is claimed before the video view sees it. */
  GtkGesture *resize_drag = gtk_gesture_drag_new ();
  g_signal_connect (resize_drag, "drag-begin", G_CALLBACK (on_resize_drag_begin), self);
  g_signal_connect (resize_drag, "drag-update", G_CALLBACK (on_resize_drag_update), self);
  g_signal_connect (resize_drag, "drag-end", G_CALLBACK (on_resize_drag_end), self);
  self->resize_drag = GTK_EVENT_CONTROLLER (resize_drag);
  gtk_widget_add_controller (GTK_WIDGET (self), self->resize_drag);

  /* Resize-cursor feedback over the edge bands. */
  self->resize_motion = gtk_event_controller_motion_new ();
  g_signal_connect (self->resize_motion, "motion", G_CALLBACK (on_resize_motion), self);
  gtk_widget_add_controller (GTK_WIDGET (self), self->resize_motion);

  /* Inert until mirroring with the aspect lock on; see fit_to_stream. */
  pm_window_set_resize_gesture_active (self, FALSE);

  /* Backstop: snap back onto the phone aspect if the window is ever resized
   * while temporarily made resizable (e.g. an open dialog sheet). */
  g_signal_connect (self, "notify::default-width",
                    G_CALLBACK (on_window_size_changed), self);
  g_signal_connect (self, "notify::default-height",
                    G_CALLBACK (on_window_size_changed), self);

  /* --- Controller ----------------------------------------------------- */
  self->session = pm_session_new ();
  pm_session_set_video_view (self->session, self->video_view);
  pm_session_set_mouse_mode (self->session, self->mouse_mode);
  pm_session_set_audio_enabled (self->session, self->audio);
  pm_session_set_video_bitrate (self->session, self->video_bitrate);
  pm_session_set_display_mode (self->session, self->display_mode);
  pm_session_set_virtual_display (self->session, self->display_width,
                                  self->display_height, self->display_dpi);
  pm_session_set_screen_off (self->session, self->screen_off);
  g_signal_connect (self->session, "state-changed",
                    G_CALLBACK (on_session_state_changed), self);
  g_signal_connect (self->session, "stream-changed",
                    G_CALLBACK (on_session_stream_changed), self);
  g_signal_connect (self->session, "startup-check-failed",
                    G_CALLBACK (on_startup_check_failed), self);

  if (!self->setup_complete) {
    pm_window_show_setup_step (self, PM_SETUP_WELCOME);
    return;
  }

  pm_window_apply_state (self, PM_STATE_IDLE, NULL);

  if (pm_device_has_pairing ()) {
    self->startup_search_active = TRUE;
    self->defer_initial_present = TRUE;
    pm_session_start_silent (self->session, NULL);
  }
}

PmWindow *
pm_window_new (PmApplication *app)
{
  return g_object_new (PM_TYPE_WINDOW, "application", app, NULL);
}

gboolean
pm_window_should_defer_present (PmWindow *self)
{
  g_return_val_if_fail (PM_IS_WINDOW (self), FALSE);
  return self->defer_initial_present;
}
