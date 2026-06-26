/* decoder.h - FFmpeg (libavcodec) video decode -> GdkTexture.
 *
 * Fed raw Annex-B H.264/H.265 chunks (as they arrive off the socket). Splits
 * them into packets with an AVCodecParser, decodes, converts to RGB, and
 * delivers a GdkTexture to `frame_cb` *on the GTK main thread* for direct use
 * by PmVideoView. If `owner` is non-NULL, each queued callback holds a ref to
 * it until delivery so teardown cannot leave the callback's user_data dangling.
 *
 * The current path is a portable software decode + swscale conversion. The
 * zero-copy upgrade (hardware decode -> DMABUF -> GdkDmabufTexture, no CPU
 * round-trip) is described in ARCHITECTURE.md and slots in behind this same
 * interface.
 */
#pragma once

#include "pm-types.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _PmDecoder PmDecoder;

/* Delivered on the GTK main thread; the texture is transfer-full. Callbacks
 * that retain it must take their own reference; PmVideoView does. */
typedef void (*PmDecoderFrameCb) (GdkTexture *texture, gpointer user_data);

PmDecoder *pm_decoder_new (PmDecoderFrameCb frame_cb,
                           gpointer         user_data,
                           GObject         *owner);
void       pm_decoder_free (PmDecoder *self);

/* Open a decoder for the negotiated codec. */
gboolean pm_decoder_open (PmDecoder *self, PmCodec codec, GError **error);

/* Feed a chunk of the elementary stream. May produce zero or more frames.
 * Safe to call repeatedly from the network worker thread. */
gboolean pm_decoder_feed (PmDecoder *self, const guint8 *data, gsize len, GError **error);

G_END_DECLS
