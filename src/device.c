/* device.c - paired-device persistence + PmDeviceInfo helpers. */
#include "device.h"
#include <gio/gio.h>
#include <glib/gstdio.h>

#define PM_CONFIG_GROUP "device"

void
pm_device_info_clear (PmDeviceInfo *info)
{
  if (info == NULL)
    return;
  g_clear_pointer (&info->name, g_free);
  g_clear_pointer (&info->host, g_free);
  info->port = 0;
}

PmDeviceInfo *
pm_device_info_copy (const PmDeviceInfo *info)
{
  PmDeviceInfo *c = g_new0 (PmDeviceInfo, 1);
  if (info != NULL) {
    c->name   = g_strdup (info->name);
    c->host   = g_strdup (info->host);
    c->port   = info->port;
  }
  return c;
}

static char *
config_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "specula", "device.ini", NULL);
}

gboolean
pm_device_has_pairing (void)
{
  g_autofree char *path = config_path ();
  return g_file_test (path, G_FILE_TEST_EXISTS);
}

gboolean
pm_device_load (PmDeviceInfo *out, GError **error)
{
  g_return_val_if_fail (out != NULL, FALSE);

  g_autofree char *path = config_path ();
  g_autoptr (GKeyFile) kf = g_key_file_new ();

  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, error))
    return FALSE;

  out->name   = g_key_file_get_string  (kf, PM_CONFIG_GROUP, "name",   NULL);
  out->host   = g_key_file_get_string  (kf, PM_CONFIG_GROUP, "host",   NULL);
  out->port   = (guint16) g_key_file_get_integer (kf, PM_CONFIG_GROUP, "port", NULL);
  if (out->port == 0)
    out->port = 5555;
  return TRUE;
}

gboolean
pm_device_save (const PmDeviceInfo *info, GError **error)
{
  g_return_val_if_fail (info != NULL, FALSE);

  g_autofree char *path = config_path ();
  g_autofree char *dir  = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0) {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                 "Could not create config dir: %s", dir);
    return FALSE;
  }

  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_string  (kf, PM_CONFIG_GROUP, "name",   info->name   ? info->name   : "");
  g_key_file_set_string  (kf, PM_CONFIG_GROUP, "host",   info->host   ? info->host   : "");
  g_key_file_set_integer (kf, PM_CONFIG_GROUP, "port",   info->port);

  return g_key_file_save_to_file (kf, path, error);
}

void
pm_device_forget (void)
{
  g_autofree char *path = config_path ();
  g_unlink (path);
}
