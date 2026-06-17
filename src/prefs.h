/* prefs.h - persisted user preferences (separate from device pairing). */
#pragma once

#include <glib.h>
#include "pm-types.h"

G_BEGIN_DECLS

typedef struct {
  gboolean      free_resize;    /* default FALSE: keep the phone aspect ratio */
  gboolean      mouse_mode;     /* default TRUE: forward pointer as a mouse */
  gboolean      audio;          /* default TRUE: play the phone's audio on the desktop */
  guint         video_bitrate;  /* default 8: video bitrate in Mbps (scrcpy default) */
  PmDisplayMode display_mode;   /* default PM_DISPLAY_MIRROR: mirror the phone screen */
  guint         display_width;  /* default 1920: virtual display width in pixels  */
  guint         display_height; /* default 1080: virtual display height in pixels */
  guint         display_dpi;    /* default 190: virtual display density in DPI    */
  gboolean      screen_off;     /* default TRUE: blank the phone screen while mirroring */
  gboolean      setup_complete; /* default FALSE: show first-run setup */
} PmPrefs;

/* Fill @out from disk, falling back to the documented defaults for any value
 * that is missing or unreadable. */
void pm_prefs_load (PmPrefs *out);

/* Persist @prefs; failures are logged, not fatal. */
void pm_prefs_save (const PmPrefs *prefs);

G_END_DECLS
