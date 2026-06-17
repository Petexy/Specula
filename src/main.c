/* main.c - entry point.
 *
 * Phone Mirror: wire-free Android screen mirroring for the Linux desktop.
 * Pure C, GTK4 + libadwaita front-end, FFmpeg decode, scrcpy-style server.
 */
#include "pm-application.h"

int
main (int argc, char *argv[])
{
  g_autoptr (PmApplication) app = pm_application_new ();
  return g_application_run (G_APPLICATION (app), argc, argv);
}
