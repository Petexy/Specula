/* pm-video-view.h - the widget that displays decoded frames.
 *
 * This is the "renderer" surface. It accepts GdkTexture frames (produced by
 * decoder.c on the UI thread) and paints them with correct aspect ratio.
 * It also exposes the displayed video rectangle so input.c can map pointer
 * coordinates back to device pixels under HiDPI/Wayland scaling.
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PM_TYPE_VIDEO_VIEW (pm_video_view_get_type ())
G_DECLARE_FINAL_TYPE (PmVideoView, pm_video_view, PM, VIDEO_VIEW, GtkWidget)

PmVideoView *pm_video_view_new (void);

/* Set the latest decoded frame. Takes its own reference; safe to unref after.
 * Must be called on the GTK main thread. */
void pm_video_view_set_texture (PmVideoView *self, GdkTexture *texture);

/* Clear the surface (e.g. on disconnect). */
void pm_video_view_clear (PmVideoView *self);

/* Map a widget-space pointer coordinate to normalised device coordinates in
 * [0,1]x[0,1] over the *video content* (letterboxing excluded). Returns FALSE
 * if (x,y) falls outside the displayed video rectangle. Used by input.c. */
gboolean pm_video_view_widget_to_device (PmVideoView *self,
                                         double       widget_x,
                                         double       widget_y,
                                         double      *out_norm_x,
                                         double      *out_norm_y);

/* Like pm_video_view_widget_to_device(), but clamps coordinates outside the
 * video content to the nearest device edge. Used to finish active drags after
 * the pointer has left the widget/content area. */
gboolean pm_video_view_widget_to_device_clamped (PmVideoView *self,
                                                 double       widget_x,
                                                 double       widget_y,
                                                 double      *out_norm_x,
                                                 double      *out_norm_y);

G_END_DECLS
