/* pm-video-view.c - GdkTexture display surface (the renderer).
 *
 * Implementation notes:
 *   - A single GtkPicture child is hosted via GtkBinLayout. GtkPicture already
 *     does GPU-side scaling of a GdkPaintable (a GdkTexture is a paintable),
 *     so the common path gets zero-copy texture display through GTK.
 *   - For a maximally-low-latency / zero-copy path (DMABUF import from a
 *     hardware decoder), replace this widget with a GtkGLArea or a custom
 *     snapshot() implementation that pushes a GdkDmabufTexture. See
 *     ARCHITECTURE.md.
 *   - The current texture reference is retained only to compute the letterboxed
 *     content rectangle for input coordinate mapping.
 */
#include "pm-video-view.h"

struct _PmVideoView {
  GtkWidget   parent_instance;
  GtkPicture *picture;
  GdkTexture *texture;   /* current frame, for size math (may be NULL) */
};

G_DEFINE_FINAL_TYPE (PmVideoView, pm_video_view, GTK_TYPE_WIDGET)

static void
pm_video_view_dispose (GObject *object)
{
  PmVideoView *self = PM_VIDEO_VIEW (object);

  g_clear_object (&self->texture);
  if (self->picture != NULL) {
    gtk_widget_unparent (GTK_WIDGET (self->picture));
    self->picture = NULL;
  }

  G_OBJECT_CLASS (pm_video_view_parent_class)->dispose (object);
}

static void
pm_video_view_class_init (PmVideoViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = pm_video_view_dispose;
  gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass),
                                            GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "videoview");
}

static void
pm_video_view_init (PmVideoView *self)
{
  self->picture = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (self->picture, GTK_CONTENT_FIT_CONTAIN);
  gtk_picture_set_can_shrink (self->picture, TRUE);
  gtk_widget_set_can_target (GTK_WIDGET (self->picture), FALSE);
  gtk_widget_set_parent (GTK_WIDGET (self->picture), GTK_WIDGET (self));

  /* Black backdrop so letterbox bars look intentional. */
  gtk_widget_add_css_class (GTK_WIDGET (self), "background");
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
}

PmVideoView *
pm_video_view_new (void)
{
  return g_object_new (PM_TYPE_VIDEO_VIEW, NULL);
}

void
pm_video_view_set_texture (PmVideoView *self, GdkTexture *texture)
{
  g_return_if_fail (PM_IS_VIDEO_VIEW (self));

  g_set_object (&self->texture, texture);
  gtk_picture_set_paintable (self->picture, GDK_PAINTABLE (texture));
}

void
pm_video_view_clear (PmVideoView *self)
{
  g_return_if_fail (PM_IS_VIDEO_VIEW (self));

  g_clear_object (&self->texture);
  gtk_picture_set_paintable (self->picture, NULL);
}

static gboolean
widget_to_device (PmVideoView *self,
                  double       widget_x,
                  double       widget_y,
                  double      *out_norm_x,
                  double      *out_norm_y,
                  gboolean     clamp)
{
  g_return_val_if_fail (PM_IS_VIDEO_VIEW (self), FALSE);

  if (self->texture == NULL)
    return FALSE;

  double vw = gdk_texture_get_width  (self->texture);
  double vh = gdk_texture_get_height (self->texture);
  double ww = gtk_widget_get_width  (GTK_WIDGET (self));
  double wh = gtk_widget_get_height (GTK_WIDGET (self));
  if (vw <= 0 || vh <= 0 || ww <= 0 || wh <= 0)
    return FALSE;

  /* Replicate GTK_CONTENT_FIT_CONTAIN: scale to fit, centre, letterbox. */
  double scale = MIN (ww / vw, wh / vh);
  double draw_w = vw * scale;
  double draw_h = vh * scale;
  double off_x = (ww - draw_w) / 2.0;
  double off_y = (wh - draw_h) / 2.0;

  double nx = (widget_x - off_x) / draw_w;
  double ny = (widget_y - off_y) / draw_h;
  if (!clamp && (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0))
    return FALSE;   /* clicked the letterbox area */

  *out_norm_x = CLAMP (nx, 0.0, 1.0);
  *out_norm_y = CLAMP (ny, 0.0, 1.0);
  return TRUE;
}

gboolean
pm_video_view_widget_to_device (PmVideoView *self,
                                double       widget_x,
                                double       widget_y,
                                double      *out_norm_x,
                                double      *out_norm_y)
{
  return widget_to_device (self, widget_x, widget_y, out_norm_x, out_norm_y, FALSE);
}

gboolean
pm_video_view_widget_to_device_clamped (PmVideoView *self,
                                        double       widget_x,
                                        double       widget_y,
                                        double      *out_norm_x,
                                        double      *out_norm_y)
{
  return widget_to_device (self, widget_x, widget_y, out_norm_x, out_norm_y, TRUE);
}
