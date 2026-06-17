/* prefs.c - persisted user preferences. */
#include "prefs.h"
#include <gio/gio.h>

#define PM_PREFS_GROUP "settings"

static char *
prefs_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "specula", "settings.ini", NULL);
}

/* Read one boolean, leaving *value untouched if the key is absent/invalid. */
static void
read_bool (GKeyFile *kf, const char *key, gboolean *value)
{
  g_autoptr (GError) error = NULL;
  gboolean v = g_key_file_get_boolean (kf, PM_PREFS_GROUP, key, &error);
  if (error == NULL)
    *value = v;
}

/* Read one display-mode enum, leaving *value untouched on a missing/invalid
 * key (and clamping any out-of-range integer to a known mode). */
static void
read_display_mode (GKeyFile *kf, const char *key, PmDisplayMode *value)
{
  g_autoptr (GError) error = NULL;
  gint v = g_key_file_get_integer (kf, PM_PREFS_GROUP, key, &error);
  if (error == NULL)
    *value = (v == PM_DISPLAY_VIRTUAL) ? PM_DISPLAY_VIRTUAL : PM_DISPLAY_MIRROR;
}

/* Read one positive integer, leaving *value untouched if the key is absent or
 * non-positive (so a corrupt 0 never reaches the server as a bad geometry). */
static void
read_uint (GKeyFile *kf, const char *key, guint *value)
{
  g_autoptr (GError) error = NULL;
  gint v = g_key_file_get_integer (kf, PM_PREFS_GROUP, key, &error);
  if (error == NULL && v > 0)
    *value = (guint) v;
}

void
pm_prefs_load (PmPrefs *out)
{
  g_return_if_fail (out != NULL);

  /* Defaults. */
  out->free_resize = FALSE;
  out->mouse_mode = TRUE;
  out->audio = TRUE;
  out->video_bitrate = 8;
  out->display_mode = PM_DISPLAY_MIRROR;
  out->display_width = 1920;
  out->display_height = 1080;
  out->display_dpi = 190;
  out->screen_off = TRUE;
  out->setup_complete = FALSE;

  g_autofree char *path = prefs_path ();
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    return;   /* no file yet → keep defaults */

  read_bool (kf, "free-resize", &out->free_resize);
  read_bool (kf, "mouse-mode", &out->mouse_mode);
  read_bool (kf, "audio", &out->audio);
  read_uint (kf, "video-bitrate", &out->video_bitrate);
  read_display_mode (kf, "display-mode", &out->display_mode);
  read_uint (kf, "display-width", &out->display_width);
  read_uint (kf, "display-height", &out->display_height);
  read_uint (kf, "display-dpi", &out->display_dpi);
  read_bool (kf, "screen-off", &out->screen_off);
  read_bool (kf, "setup-complete", &out->setup_complete);
}

void
pm_prefs_save (const PmPrefs *prefs)
{
  g_return_if_fail (prefs != NULL);

  g_autofree char *path = prefs_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0) {
    g_warning ("prefs: could not create config dir: %s", dir);
    return;
  }

  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_boolean (kf, PM_PREFS_GROUP, "free-resize", prefs->free_resize);
  g_key_file_set_boolean (kf, PM_PREFS_GROUP, "mouse-mode", prefs->mouse_mode);
  g_key_file_set_boolean (kf, PM_PREFS_GROUP, "audio", prefs->audio);
  g_key_file_set_integer (kf, PM_PREFS_GROUP, "video-bitrate", (gint) prefs->video_bitrate);
  g_key_file_set_integer (kf, PM_PREFS_GROUP, "display-mode", prefs->display_mode);
  g_key_file_set_integer (kf, PM_PREFS_GROUP, "display-width", (gint) prefs->display_width);
  g_key_file_set_integer (kf, PM_PREFS_GROUP, "display-height", (gint) prefs->display_height);
  g_key_file_set_integer (kf, PM_PREFS_GROUP, "display-dpi", (gint) prefs->display_dpi);
  g_key_file_set_boolean (kf, PM_PREFS_GROUP, "screen-off", prefs->screen_off);
  g_key_file_set_boolean (kf, PM_PREFS_GROUP, "setup-complete", prefs->setup_complete);

  g_autoptr (GError) error = NULL;
  if (!g_key_file_save_to_file (kf, path, &error))
    g_warning ("prefs: could not save settings: %s", error->message);
}
