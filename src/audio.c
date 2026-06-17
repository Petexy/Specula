/* audio.c - PulseAudio "simple" playback sink for the phone's PCM stream. */
#include "audio.h"

#include <gio/gio.h>      /* G_IO_ERROR error domain */
#include <math.h>         /* lrintf */
#include <pulse/simple.h>
#include <pulse/error.h>

/* Target buffer fill on the server. Phone mirroring is interactive, so favour a
 * short queue (low latency / tighter A/V sync) over deep buffering. ~120 ms is
 * comfortably above typical Wi-Fi jitter while staying responsive. */
#define PM_AUDIO_TARGET_LATENCY_MS 120

/* Software make-up gain applied to the phone's PCM before playback. The phone
 * tends to stream at a fairly low level, so lift it a notch (1.5× ≈ +3.5 dB).
 * Samples are saturated, not wrapped, so peaks clip cleanly instead of
 * producing harsh overflow noise. */
#define PM_AUDIO_GAIN 1.5f

struct _PmAudio {
  pa_simple *stream;
  guint32    rate;
  guint8     channels;
};

PmAudio *
pm_audio_new (void)
{
  return g_new0 (PmAudio, 1);
}

gboolean
pm_audio_open (PmAudio *self, guint32 rate, guint8 channels, GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (self->stream == NULL, FALSE);
  g_return_val_if_fail (rate > 0 && channels > 0, FALSE);

  pa_sample_spec spec = {
    .format   = PA_SAMPLE_S16LE,
    .rate     = rate,
    .channels = channels,
  };

  /* tlength caps how much the server buffers; the rest stay at the library
   * defaults (signalled by (uint32_t) -1). */
  const guint32 frame_bytes = (guint32) channels * 2; /* S16 = 2 bytes/sample */
  pa_buffer_attr attr = {
    .maxlength = (uint32_t) -1,
    .tlength   = rate * frame_bytes * PM_AUDIO_TARGET_LATENCY_MS / 1000,
    .prebuf    = (uint32_t) -1,
    .minreq    = (uint32_t) -1,
    .fragsize  = (uint32_t) -1,
  };

  int err = 0;
  self->stream = pa_simple_new (NULL,                    /* default server   */
                                "Phone Mirror",          /* application name  */
                                PA_STREAM_PLAYBACK,
                                NULL,                     /* default device   */
                                "Phone audio",           /* stream description*/
                                &spec,
                                NULL,                     /* default channel map */
                                &attr,
                                &err);
  if (self->stream == NULL) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "could not open audio output: %s", pa_strerror (err));
    return FALSE;
  }

  self->rate = rate;
  self->channels = channels;
  return TRUE;
}

/* Multiply each S16LE sample by PM_AUDIO_GAIN in place, clamping to the 16-bit
 * range so overdriven peaks clip rather than wrap. */
static void
apply_gain (gint16 *samples, gsize count)
{
  for (gsize i = 0; i < count; i++) {
    gint32 v = (gint32) lrintf (samples[i] * PM_AUDIO_GAIN);
    samples[i] = (gint16) CLAMP (v, G_MININT16, G_MAXINT16);
  }
}

gboolean
pm_audio_play (PmAudio *self, const guint8 *data, gsize len, GError **error)
{
  g_return_val_if_fail (self != NULL && self->stream != NULL, FALSE);

  if (len == 0)
    return TRUE;

  /* Boost in a scratch copy so the caller's read buffer stays untouched, then
   * play the amplified PCM. A partial trailing byte (never expected for whole
   * S16 frames) is preserved as-is. */
  g_autofree gint16 *boosted = g_memdup2 (data, len);
  apply_gain (boosted, len / sizeof (gint16));

  int err = 0;
  if (pa_simple_write (self->stream, boosted, len, &err) < 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "audio playback failed: %s", pa_strerror (err));
    return FALSE;
  }
  return TRUE;
}

void
pm_audio_free (PmAudio *self)
{
  if (self == NULL)
    return;
  if (self->stream != NULL) {
    /* Flush whatever is queued so shutdown doesn't block waiting for it to
     * drain; the stream is going away regardless. */
    pa_simple_flush (self->stream, NULL);
    pa_simple_free (self->stream);
  }
  g_free (self);
}
