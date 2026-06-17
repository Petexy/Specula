/* pm-application.c - AdwApplication subclass.
 *
 * Responsibilities:
 *   - Own the single primary window (single-instance app).
 *   - Honour the system dark-style preference via AdwStyleManager (default
 *     PREFER follows the desktop; this is the GNOME-HIG behaviour).
 *   - Register global actions (quit, about) and accelerators.
 */
#include "pm-application.h"
#include "pm-window.h"
#include "pm-config.h"

struct _PmApplication {
  AdwApplication parent_instance;
};

G_DEFINE_FINAL_TYPE (PmApplication, pm_application, ADW_TYPE_APPLICATION)

static void
pm_application_activate (GApplication *app)
{
  GtkWindow *window;

  /* Single-window app: present the existing window if already created. */
  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window == NULL) {
    GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));
    window = windows != NULL ? GTK_WINDOW (windows->data) : NULL;
  }
  if (window == NULL)
    window = GTK_WINDOW (pm_window_new (PM_APPLICATION (app)));

  if (!pm_window_should_defer_present (PM_WINDOW (window)))
    gtk_window_present (window);
}

static void
on_quit_action (GSimpleAction *action,
                GVariant      *param,
                gpointer       user_data)
{
  GApplication *app = G_APPLICATION (user_data);
  g_application_quit (app);
}

static void
on_about_action (GSimpleAction *action,
                 GVariant      *param,
                 gpointer       user_data)
{
  GtkApplication *app = GTK_APPLICATION (user_data);
  const char *developers[] = {
    "Piotr 'Linexy' Lewandowski https://github.com/Petexy",
    NULL
  };

  adw_show_about_dialog (GTK_WIDGET (gtk_application_get_active_window (app)),
                         "application-name", "Phone Mirror",
                         "application-icon", PM_APP_ID,
                         "version", PM_VERSION,
                         "developers", developers,
                         "license-type", GTK_LICENSE_GPL_3_0,
                         NULL);
}

static const GActionEntry app_actions[] = {
  { "quit",  on_quit_action,  NULL, NULL, NULL },
  { "about", on_about_action, NULL, NULL, NULL },
};

static void
pm_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (pm_application_parent_class)->startup (app);

  /* When launched uninstalled from the build tree, the app icon is not yet in
   * the system hicolor theme; add the source icons dir so it still resolves by
   * app id. Harmless once installed (the themed copy takes precedence). */
  GdkDisplay *display = gdk_display_get_default ();
  if (display != NULL)
    gtk_icon_theme_add_search_path (gtk_icon_theme_get_for_display (display),
                                    PM_SOURCE_ICONS_DIR);

  /* Follow the system-wide light/dark preference. ADW_COLOR_SCHEME_DEFAULT
   * lets AdwStyleManager track the desktop setting (this is also the default,
   * set here explicitly to document the intent). */
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                      ADW_COLOR_SCHEME_DEFAULT);

  g_action_map_add_action_entries (G_ACTION_MAP (app),
                                   app_actions, G_N_ELEMENTS (app_actions),
                                   app);

  const char *quit_accels[] = { "<Ctrl>q", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "app.quit", quit_accels);
}

static void
pm_application_class_init (PmApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  app_class->activate = pm_application_activate;
  app_class->startup  = pm_application_startup;
}

static void
pm_application_init (PmApplication *self)
{
}

PmApplication *
pm_application_new (void)
{
  return g_object_new (PM_TYPE_APPLICATION,
                       "application-id", PM_APP_ID,
                       "flags", G_APPLICATION_DEFAULT_FLAGS,
                       NULL);
}
