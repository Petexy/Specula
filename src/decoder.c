/* decoder.c - libavcodec decode + swscale -> GdkMemoryTexture. */
#include "decoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

struct _PmDecoder {
  PmDecoderFrameCb       frame_cb;
  gpointer               user_data;
  GObject               *owner;       /* borrowed; ref held by queued frames */

  const AVCodec         *codec;
  AVCodecContext        *ctx;
  AVCodecParserContext  *parser;
  AVPacket              *pkt;
  AVFrame               *frame;

  struct SwsContext     *sws;
  int                    sws_w, sws_h;   /* dims the sws ctx is built for */
};

/* --- main-thread frame delivery ------------------------------------------- */

typedef struct {
  PmDecoderFrameCb cb;
  gpointer         user_data;
  GObject         *owner;
  GdkTexture      *texture;
} DeliverPayload;

static gboolean
deliver_frame (gpointer data)
{
  DeliverPayload *p = data;
  p->cb (p->texture, p->user_data);
  return G_SOURCE_REMOVE;
}

static void
deliver_payload_free (gpointer data)
{
  DeliverPayload *p = data;
  g_clear_object (&p->texture);
  g_clear_object (&p->owner);
  g_free (p);
}

/* --- lifecycle ------------------------------------------------------------ */

PmDecoder *
pm_decoder_new (PmDecoderFrameCb frame_cb,
                gpointer         user_data,
                GObject         *owner)
{
  PmDecoder *self = g_new0 (PmDecoder, 1);
  self->frame_cb = frame_cb;
  self->user_data = user_data;
  self->owner = owner;
  return self;
}

static enum AVCodecID
to_av_codec_id (PmCodec codec)
{
  switch (codec) {
    case PM_CODEC_H264: return AV_CODEC_ID_H264;
    case PM_CODEC_H265: return AV_CODEC_ID_HEVC;
    case PM_CODEC_AV1:  return AV_CODEC_ID_AV1;
    default:            return AV_CODEC_ID_NONE;
  }
}

gboolean
pm_decoder_open (PmDecoder *self, PmCodec codec, GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);

  enum AVCodecID id = to_av_codec_id (codec);
  self->codec = avcodec_find_decoder (id);
  if (self->codec == NULL) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "no decoder for codec %d", (int) codec);
    return FALSE;
  }

  self->ctx = avcodec_alloc_context3 (self->codec);
  self->parser = av_parser_init (id);
  self->pkt = av_packet_alloc ();
  self->frame = av_frame_alloc ();
  if (!self->ctx || !self->parser || !self->pkt || !self->frame) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "failed to allocate decoder objects");
    return FALSE;
  }

  /* Favour latency: let the decoder emit frames ASAP. */
  self->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  self->ctx->thread_type = FF_THREAD_SLICE;

  if (avcodec_open2 (self->ctx, self->codec, NULL) < 0) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "avcodec_open2 failed");
    return FALSE;
  }
  return TRUE;
}

/* Convert the current decoded frame to packed RGB and hand a GdkTexture to the
 * main thread. */
static gboolean
emit_frame (PmDecoder *self, GError **error)
{
  int w = self->frame->width;
  int h = self->frame->height;
  if (w <= 0 || h <= 0)
    return TRUE;

  if (self->sws == NULL || self->sws_w != w || self->sws_h != h) {
    sws_freeContext (self->sws);
    self->sws = sws_getContext (w, h, self->ctx->pix_fmt,
                                w, h, AV_PIX_FMT_RGB24,
                                SWS_BILINEAR, NULL, NULL, NULL);
    self->sws_w = w;
    self->sws_h = h;
  }
  if (self->sws == NULL) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "sws_getContext failed");
    return FALSE;
  }

  /* swscale's SIMD paths write the destination in aligned chunks and can run
   * a few bytes past a tightly-packed row/buffer (depends on the device's frame
   * geometry).  Align the stride and over-allocate so those writes stay inside
   * the allocation instead of corrupting the heap. */
  const int stride = FFALIGN (w * 3, 32);
  gsize buf_size = (gsize) stride * h;
  guint8 *rgb = g_malloc (buf_size + 32);
  uint8_t *dst[4] = { rgb, NULL, NULL, NULL };
  int      dst_stride[4] = { stride, 0, 0, 0 };

  sws_scale (self->sws,
             (const uint8_t * const *) self->frame->data, self->frame->linesize,
             0, h, dst, dst_stride);

  /* Build the texture on the main thread to keep all GDK use single-threaded. */
  GBytes *bytes = g_bytes_new_take (rgb, buf_size);
  DeliverPayload *p = g_new0 (DeliverPayload, 1);
  p->cb = self->frame_cb;
  p->user_data = self->user_data;
  if (self->owner != NULL)
    p->owner = g_object_ref (self->owner);
  p->texture = GDK_TEXTURE (gdk_memory_texture_new (w, h,
                              GDK_MEMORY_R8G8B8, bytes, stride));
  g_bytes_unref (bytes);

  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                              deliver_frame, p, deliver_payload_free);
  return TRUE;
}

static gboolean
decode_packet (PmDecoder *self, GError **error)
{
  if (avcodec_send_packet (self->ctx, self->pkt) < 0) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "avcodec_send_packet failed");
    return FALSE;
  }

  for (;;) {
    int ret = avcodec_receive_frame (self->ctx, self->frame);
    if (ret == AVERROR (EAGAIN) || ret == AVERROR_EOF)
      return TRUE;
    if (ret < 0) {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "avcodec_receive_frame failed");
      return FALSE;
    }
    if (!emit_frame (self, error))
      return FALSE;
    av_frame_unref (self->frame);
  }
}

gboolean
pm_decoder_feed (PmDecoder *self, const guint8 *data, gsize len, GError **error)
{
  g_return_val_if_fail (self != NULL && self->parser != NULL, FALSE);

  while (len > 0) {
    int used = av_parser_parse2 (self->parser, self->ctx,
                                 &self->pkt->data, &self->pkt->size,
                                 data, (int) MIN (len, (gsize) G_MAXINT),
                                 AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (used < 0) {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "av_parser_parse2 failed");
      return FALSE;
    }
    data += used;
    len  -= used;

    if (self->pkt->size > 0 && !decode_packet (self, error))
      return FALSE;
  }
  return TRUE;
}

void
pm_decoder_free (PmDecoder *self)
{
  if (self == NULL)
    return;
  if (self->sws)    sws_freeContext (self->sws);
  if (self->frame)  av_frame_free (&self->frame);
  if (self->pkt)    av_packet_free (&self->pkt);
  if (self->parser) av_parser_close (self->parser);
  if (self->ctx)    avcodec_free_context (&self->ctx);
  g_free (self);
}
