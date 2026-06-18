/* pm-connect-dialog.c - device setup dialog. */
#include "pm-connect-dialog.h"
#include "device.h"
#include "adb.h"
#include "pinstore.h"
#include <string.h>
#include <glib/gi18n.h>

struct _PmConnectDialog {
  AdwDialog        parent_instance;

  AdwToastOverlay *toasts;
  AdwEntryRow     *host_row;
  AdwEntryRow     *port_row;
  AdwEntryRow     *pair_addr_row;
  AdwEntryRow     *code_row;
  AdwPasswordEntryRow *pin_row;   /* optional lockscreen PIN (either page) */
  GtkWidget       *pair_button;

  PmConnectCb      cb;
  gpointer         user_data;
};

G_DEFINE_FINAL_TYPE (PmConnectDialog, pm_connect_dialog, ADW_TYPE_DIALOG)

static const char *
row_text (AdwEntryRow *row)
{
  return gtk_editable_get_text (GTK_EDITABLE (row));
}

static guint16
parse_port (const char *s, guint16 fallback)
{
  if (s == NULL || *s == '\0')
    return fallback;
  guint64 v = g_ascii_strtoull (s, NULL, 10);
  return (v > 0 && v <= G_MAXUINT16) ? (guint16) v : fallback;
}

static gboolean
parse_endpoint (const char *s,
                guint16     fallback_port,
                char      **out_host,
                guint16    *out_port)
{
  g_return_val_if_fail (out_host != NULL, FALSE);
  g_return_val_if_fail (out_port != NULL, FALSE);

  g_autofree char *trimmed = g_strdup (s ? s : "");
  g_strstrip (trimmed);
  if (*trimmed == '\0')
    return FALSE;

  const char *host_start = trimmed;
  const char *host_end = trimmed + strlen (trimmed);
  guint16 port = fallback_port;

  if (*trimmed == '[') {
    const char *close = strchr (trimmed, ']');
    if (close == NULL || close == trimmed + 1)
      return FALSE;
    host_start = trimmed + 1;
    host_end = close;
    if (close[1] == ':') {
      guint64 parsed = g_ascii_strtoull (close + 2, NULL, 10);
      if (parsed == 0 || parsed > G_MAXUINT16)
        return FALSE;
      port = (guint16) parsed;
    } else if (close[1] != '\0') {
      return FALSE;
    }
  } else {
    const char *first_colon = strchr (trimmed, ':');
    const char *last_colon = strrchr (trimmed, ':');

    /* A single colon means host:port. Multiple colons are treated as a bare
     * IPv6 literal and use the separate/default port. */
    if (first_colon != NULL && first_colon == last_colon) {
      if (last_colon == trimmed || last_colon[1] == '\0')
        return FALSE;
      guint64 parsed = g_ascii_strtoull (last_colon + 1, NULL, 10);
      if (parsed == 0 || parsed > G_MAXUINT16)
        return FALSE;
      host_end = last_colon;
      port = (guint16) parsed;
    }
  }

  g_autofree char *host = g_strndup (host_start, host_end - host_start);
  g_strstrip (host);
  if (*host == '\0')
    return FALSE;

  *out_host = g_steal_pointer (&host);
  *out_port = port ? port : 5555;
  return TRUE;
}

static void
toast (PmConnectDialog *self, const char *text)
{
  adw_toast_overlay_add_toast (self->toasts, adw_toast_new (text));
}

/* If the user typed a lockscreen PIN, stash it as the "pending" secret. The
 * device's MAC isn't known until it connects, so the session commits the pending
 * PIN to that MAC on first connect (see pm-session.c::maybe_unlock_device).
 * An empty field is left alone so it never wipes a PIN saved earlier. */
static void
stash_pin (PmConnectDialog *self)
{
  if (self->pin_row == NULL)
    return;
  const char *pin = gtk_editable_get_text (GTK_EDITABLE (self->pin_row));
  if (pin == NULL || *pin == '\0')
    return;

  g_autoptr (GError) error = NULL;
  if (!pm_pinstore_set_pending (pin, &error))
    g_warning ("connect-dialog: could not stash PIN: %s", error->message);
}

/* Add a masked, optional PIN entry to `page`. AdwPasswordEntryRow (an
 * AdwEntryRow) has no per-row subtitle, so the explanation lives on the group.
 * Shared by both pages. */
static void
add_pin_group (PmConnectDialog *self, AdwPreferencesPage *page)
{
  AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (grp, _("Auto-unlock"));
  adw_preferences_group_set_description (grp,
    _("Optional. Unlocks your phone automatically when it connects. Otherwise, "
      "the app will show a black screen while you unlock your phone."));

  self->pin_row = ADW_PASSWORD_ENTRY_ROW (adw_password_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->pin_row),
                                 _("Lockscreen PIN"));
  adw_preferences_group_add (grp, GTK_WIDGET (self->pin_row));

  adw_preferences_page_add (page, grp);
}

/* Add a trailing, untitled group holding just an action button, so the button
 * sits at the very bottom of the page below every input group. */
static void
add_button_group (AdwPreferencesPage *page, GtkWidget *button)
{
  AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_add (grp, button);
  adw_preferences_page_add (page, grp);
}

/* --- connect -------------------------------------------------------------- */

static void
on_connect_clicked (GtkButton *button, gpointer user_data)
{
  PmConnectDialog *self = user_data;
  const char *entered_host = row_text (self->host_row);
  guint16 fallback_port = parse_port (row_text (self->port_row), 5555);
  g_autofree char *host = NULL;
  guint16 port = 0;

  if (!parse_endpoint (entered_host, fallback_port, &host, &port)) {
    toast (self, _("Enter the device IP address"));
    return;
  }

  PmDeviceInfo info = { .host = host, .port = port,
                        .serial = NULL, .name = NULL };
  g_autoptr (GError) error = NULL;
  if (!pm_device_save (&info, &error)) {
    toast (self, error->message);
    return;
  }

  if (self->cb != NULL)
    self->cb (host, port, NULL, self->user_data);

  adw_dialog_close (ADW_DIALOG (self));
}

/* --- pair (async) --------------------------------------------------------- */

typedef struct {
  char *host;
  guint16 port;
  char *code;
} PairData;

static void
pair_data_free (gpointer data)
{
  PairData *p = data;
  g_free (p->host);
  g_free (p->code);
  g_free (p);
}

static void
pair_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  PairData *p = task_data;
  GError *error = NULL;
  if (pm_adb_pair (p->host, p->port, p->code, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

static void
pair_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PmConnectDialog *self = user_data;
  PairData *p = g_task_get_task_data (G_TASK (res));
  g_autoptr (GError) error = NULL;
  gboolean ok = g_task_propagate_boolean (G_TASK (res), &error);

  gtk_widget_set_sensitive (self->pair_button, TRUE);
  if (!ok) {
    toast (self, error->message);
    return;
  }

  if (self->cb != NULL)
    self->cb (p->host, 0, NULL, self->user_data);

  adw_dialog_close (ADW_DIALOG (self));
}

static void
on_pair_clicked (GtkButton *button, gpointer user_data)
{
  PmConnectDialog *self = user_data;
  const char *addr = row_text (self->pair_addr_row);
  const char *code = row_text (self->code_row);

  const char *colon = addr ? strrchr (addr, ':') : NULL;
  if (colon == NULL || code == NULL || *code == '\0') {
    toast (self, _("Enter pairing address (ip:port) and code"));
    return;
  }

  PairData *p = g_new0 (PairData, 1);
  p->host = g_strndup (addr, colon - addr);
  p->port = parse_port (colon + 1, 0);
  p->code = g_strdup (code);
  if (p->port == 0) {
    toast (self, _("Invalid pairing port"));
    pair_data_free (p);
    return;
  }

  stash_pin (self);

  gtk_widget_set_sensitive (self->pair_button, FALSE);
  toast (self, _("Pairing…"));

  GTask *task = g_task_new (self, NULL, pair_done, self);
  g_task_set_task_data (task, p, pair_data_free);
  g_task_run_in_thread (task, pair_thread);
  g_object_unref (task);
}

/* --- construction --------------------------------------------------------- */

static GtkWidget *
action_button (const char *label, gboolean suggested, GCallback cb, gpointer data)
{
  GtkWidget *b = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (b, "pill");
  if (suggested)
    gtk_widget_add_css_class (b, "suggested-action");
  gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (b, 6);
  g_signal_connect (b, "clicked", cb, data);
  return b;
}

static void
set_page (PmConnectDialog    *self,
          const char         *title,
          AdwPreferencesPage *page)
{
  adw_dialog_set_title (ADW_DIALOG (self), title);

  AdwToolbarView *tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (tv, adw_header_bar_new ());
  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toasts, GTK_WIDGET (page));
  adw_toolbar_view_set_content (tv, GTK_WIDGET (self->toasts));

  adw_dialog_set_child (ADW_DIALOG (self), GTK_WIDGET (tv));
}

static void
build_pairing_page (PmConnectDialog *self)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  AdwPreferencesGroup *pg = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (pg, _("Pairing"));
  adw_preferences_group_set_description (pg,
    _("Enable Wireless debugging → Pair device with code."));

  self->pair_addr_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->pair_addr_row),
                                 _("Pairing address (ip:port)"));
  self->code_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->code_row), _("Pairing code"));

  adw_preferences_group_add (pg, GTK_WIDGET (self->pair_addr_row));
  adw_preferences_group_add (pg, GTK_WIDGET (self->code_row));
  adw_preferences_page_add (page, pg);

  add_pin_group (self, page);

  self->pair_button = action_button (_("Pair & Connect"), TRUE, G_CALLBACK (on_pair_clicked), self);
  add_button_group (page, self->pair_button);

  set_page (self, _("Set up device"), page);
}

static void
build_connection_page (PmConnectDialog *self)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  AdwPreferencesGroup *cg = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (cg, _("Connection"));
  adw_preferences_group_set_description (cg,
    _("Enter the Wi-Fi debugging address only if automatic discovery cannot find the phone."));

  self->host_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->host_row), _("IP address or ip:port"));
  self->port_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->port_row), _("Port (default 5555)"));

  adw_preferences_group_add (cg, GTK_WIDGET (self->host_row));
  adw_preferences_group_add (cg, GTK_WIDGET (self->port_row));
  adw_preferences_page_add (page, cg);

  /* No PIN field here on purpose: the lockscreen PIN is collected during pairing
   * or set later from the main menu (Lockscreen PIN…), keeping a quick manual
   * connection friction-free. */
  add_button_group (page,
    action_button (_("Connect manually"), TRUE, G_CALLBACK (on_connect_clicked), self));

  if (pm_device_has_pairing ()) {
    PmDeviceInfo saved = { 0 };
    if (pm_device_load (&saved, NULL)) {
      if (saved.host)
        gtk_editable_set_text (GTK_EDITABLE (self->host_row), saved.host);
      g_autofree char *ps = g_strdup_printf ("%u", saved.port);
      gtk_editable_set_text (GTK_EDITABLE (self->port_row), ps);
    }
    pm_device_info_clear (&saved);
  }

  set_page (self, _("Manual connection"), page);
}

static void
pm_connect_dialog_init (PmConnectDialog *self)
{
  adw_dialog_set_content_width (ADW_DIALOG (self), 420);

  /* Always slide up from the bottom, regardless of window size. The default
   * AUTO mode renders as a floating window when the parent is wide (e.g. while
   * mirroring) and only becomes a bottom sheet when narrow. */
  adw_dialog_set_presentation_mode (ADW_DIALOG (self),
                                    ADW_DIALOG_BOTTOM_SHEET);
}

static void
pm_connect_dialog_class_init (PmConnectDialogClass *klass)
{
}

PmConnectDialog *
pm_connect_dialog_new (PmConnectCb cb, gpointer user_data)
{
  PmConnectDialog *self = g_object_new (PM_TYPE_CONNECT_DIALOG, NULL);
  self->cb = cb;
  self->user_data = user_data;
  build_pairing_page (self);
  return self;
}

PmConnectDialog *
pm_manual_connect_dialog_new (PmConnectCb cb, gpointer user_data)
{
  PmConnectDialog *self = g_object_new (PM_TYPE_CONNECT_DIALOG, NULL);
  self->cb = cb;
  self->user_data = user_data;
  build_connection_page (self);
  return self;
}
