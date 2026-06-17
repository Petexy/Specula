/* pm-settings-dialog.h - app settings as a bottom-sheet dialog. */
#pragma once

#include <adwaita.h>
#include "pm-types.h"

G_BEGIN_DECLS

#define PM_TYPE_SETTINGS_DIALOG (pm_settings_dialog_get_type ())
G_DECLARE_FINAL_TYPE (PmSettingsDialog, pm_settings_dialog, PM, SETTINGS_DIALOG, AdwDialog)

/* Current values of every toggle in the dialog. */
typedef struct {
  gboolean      free_resize;     /* resize the window without keeping the aspect ratio */
  gboolean      mouse_mode;      /* TRUE: forward pointer as mouse, FALSE: as touch */
  gboolean      audio;           /* TRUE: play the phone's audio on the desktop */
  guint         video_bitrate;   /* video bitrate in Mbps (scrcpy default is 8) */
  PmDisplayMode display_mode;    /* mirror the phone or spawn a virtual display */
  guint         display_width;   /* virtual display width in pixels  (Virtual only) */
  guint         display_height;  /* virtual display height in pixels (Virtual only) */
  guint         display_dpi;     /* virtual display density in DPI   (Virtual only) */
  gboolean      screen_off;      /* TRUE: blank the phone panel while mirroring */
} PmSettings;

/* Invoked whenever any setting changes, with the full current state. */
typedef void (*PmSettingsChangedCb) (const PmSettings *settings, gpointer user_data);

PmSettingsDialog *pm_settings_dialog_new (const PmSettings    *initial,
                                          PmSettingsChangedCb  cb,
                                          gpointer             user_data);

G_END_DECLS
