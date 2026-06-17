/* pm-types.h - shared types and small utilities used across modules.
 *
 * Keep this header free of GTK/FFmpeg includes so low-level modules
 * (net, adb, protocol) can include it without dragging in the UI stack.
 */
#pragma once

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

/* Video codec negotiated with the device server. */
typedef enum {
  PM_CODEC_UNKNOWN = 0,
  PM_CODEC_H264,
  PM_CODEC_H265,
  PM_CODEC_AV1,
} PmCodec;

/* How the on-device server produces the mirrored stream. */
typedef enum {
  PM_DISPLAY_MIRROR = 0,  /* Mirror the phone's existing physical display.   */
  PM_DISPLAY_VIRTUAL,     /* Spawn a separate virtual display on the device.  */
} PmDisplayMode;

/* High-level lifecycle state, mirrored by the UI (AdwViewStack pages). */
typedef enum {
  PM_STATE_IDLE = 0,     /* Nothing happening, no paired device chosen.   */
  PM_STATE_SEARCHING,    /* Discovery thread is probing the network.      */
  PM_STATE_CONNECTING,   /* Device found; adb connect + server handshake. */
  PM_STATE_MIRRORING,    /* Stream is live and rendering.                 */
  PM_STATE_ERROR,        /* Recoverable failure; message is shown.        */
} PmState;

/* Description of a (paired) target device. */
typedef struct {
  char    *serial;       /* adb serial, e.g. "RZ8N..." or "ip:port".      */
  char    *name;         /* Friendly model name for the UI.               */
  char    *host;         /* Last-known IPv4/IPv6 literal.                  */
  guint16  port;         /* adb tcp port (default 5555 or mDNS dynamic).   */
} PmDeviceInfo;

/* Negotiated stream geometry. The device may rotate; (width,height) is the
 * *current* encoded frame size, used for input coordinate mapping. */
typedef struct {
  PmCodec codec;
  guint32 width;
  guint32 height;
} PmStreamInfo;

static inline const char *
pm_state_to_string (PmState state)
{
  switch (state) {
    case PM_STATE_IDLE:       return "idle";
    case PM_STATE_SEARCHING:  return "searching";
    case PM_STATE_CONNECTING: return "connecting";
    case PM_STATE_MIRRORING:  return "mirroring";
    case PM_STATE_ERROR:      return "error";
    default:                  return "?";
  }
}

void pm_device_info_clear (PmDeviceInfo *info);
PmDeviceInfo *pm_device_info_copy (const PmDeviceInfo *info);

G_END_DECLS
