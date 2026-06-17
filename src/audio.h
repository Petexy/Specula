/* audio.h - desktop audio sink for the phone's streamed PCM.
 *
 * The on-device server captures the phone's audio output and streams it as raw
 * little-endian 16-bit PCM. PmAudio plays that PCM out the desktop's default
 * audio device (via PulseAudio's blocking "simple" API, which PipeWire also
 * serves), so the computer behaves like a pair of headphones plugged into the
 * phone.
 *
 * Like PmNet/PmDecoder this is a plain object with no GTK dependency; it lives
 * on PmSession's audio worker thread and all of its calls block.
 */
#pragma once

#include "pm-types.h"

G_BEGIN_DECLS

typedef struct _PmAudio PmAudio;

PmAudio *pm_audio_new (void);
void     pm_audio_free (PmAudio *self);

/* Open a playback stream for signed 16-bit little-endian PCM at the given
 * sample rate and channel count. Returns FALSE (with @error set) if no audio
 * server is reachable - the caller treats audio as best-effort and carries on
 * with video only. */
gboolean pm_audio_open (PmAudio *self, guint32 rate, guint8 channels, GError **error);

/* Write @len bytes of interleaved PCM to the sink, blocking until the server
 * accepts them. Returns FALSE on a fatal write error. */
gboolean pm_audio_play (PmAudio *self, const guint8 *data, gsize len, GError **error);

G_END_DECLS
