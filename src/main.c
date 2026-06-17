/* main.c - entry point.
 *
 * Phone Mirror: wire-free Android screen mirroring for the Linux desktop.
 * Pure C, GTK4 + libadwaita front-end, FFmpeg decode, scrcpy-style server.
 */
#include <locale.h>
#include <glib/gi18n.h>

#include "pm-application.h"
#include "pm-config.h"

int
main (int argc, char *argv[])
{
  /* Honour the user's locale (LANG / LC_* environment) so the UI is shown in
   * the system language automatically, and point gettext at our installed
   * message catalogs. */
  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, PM_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_autoptr (PmApplication) app = pm_application_new ();
  return g_application_run (G_APPLICATION (app), argc, argv);
}
