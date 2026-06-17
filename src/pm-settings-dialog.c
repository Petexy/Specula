/* pm-settings-dialog.c - app settings as a bottom-sheet dialog. */
#include "pm-settings-dialog.h"

/* AdwComboRow indices for the pointer-input mode. */
enum { PM_POINTER_MOUSE = 0, PM_POINTER_TOUCH = 1 };

/* AdwComboRow indices for the display mode (matches the "display_modes" model
 * and PmDisplayMode ordering). */
enum { PM_DISPLAY_ROW_MIRROR = 0, PM_DISPLAY_ROW_VIRTUAL = 1 };

struct _PmSettingsDialog {
  AdwDialog           parent_instance;

  AdwSwitchRow       *free_resize_row;
  AdwComboRow        *pointer_row;
  AdwComboRow        *display_row;
  AdwSpinRow         *width_row;
  AdwSpinRow         *height_row;
  AdwSpinRow         *dpi_row;
  AdwSpinRow         *bitrate_row;
  AdwSwitchRow       *screen_off_row;
  AdwSwitchRow       *audio_row;

  PmSettingsChangedCb cb;
  gpointer            user_data;
};

G_DEFINE_FINAL_TYPE (PmSettingsDialog, pm_settings_dialog, ADW_TYPE_DIALOG)

static void
emit_changed (PmSettingsDialog *self)
{
  if (self->cb == NULL)
    return;

  PmSettings settings = {
    .free_resize    = adw_switch_row_get_active (self->free_resize_row),
    .mouse_mode     = adw_combo_row_get_selected (self->pointer_row) == PM_POINTER_MOUSE,
    .audio          = adw_switch_row_get_active (self->audio_row),
    .video_bitrate  = (guint) adw_spin_row_get_value (self->bitrate_row),
    .display_mode   = adw_combo_row_get_selected (self->display_row) == PM_DISPLAY_ROW_VIRTUAL
                        ? PM_DISPLAY_VIRTUAL : PM_DISPLAY_MIRROR,
    .display_width  = (guint) adw_spin_row_get_value (self->width_row),
    .display_height = (guint) adw_spin_row_get_value (self->height_row),
    .display_dpi    = (guint) adw_spin_row_get_value (self->dpi_row),
    .screen_off     = adw_switch_row_get_active (self->screen_off_row),
  };
  self->cb (&settings, self->user_data);
}

/* The resolution/DPI rows only matter for a virtual display, so reveal them
 * only when that mode is selected. */
static void
update_virtual_rows_visible (PmSettingsDialog *self)
{
  gboolean virtual = adw_combo_row_get_selected (self->display_row) == PM_DISPLAY_ROW_VIRTUAL;
  gtk_widget_set_visible (GTK_WIDGET (self->width_row), virtual);
  gtk_widget_set_visible (GTK_WIDGET (self->height_row), virtual);
  gtk_widget_set_visible (GTK_WIDGET (self->dpi_row), virtual);
}

static void
on_setting_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  PmSettingsDialog *self = PM_SETTINGS_DIALOG (user_data);
  /* Cheap and idempotent; keeps the resolution/DPI rows in step with the mode
   * combo without a separate handler. */
  update_virtual_rows_visible (self);
  emit_changed (self);
}

static void
pm_settings_dialog_init (PmSettingsDialog *self)
{
  adw_dialog_set_title (ADW_DIALOG (self), "Settings");
  adw_dialog_set_content_width (ADW_DIALOG (self), 420);

  /* Always slide up from the bottom, regardless of window size - matches the
   * device setup dialog. */
  adw_dialog_set_presentation_mode (ADW_DIALOG (self), ADW_DIALOG_BOTTOM_SHEET);

  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  AdwPreferencesGroup *display_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (display_group, "Display");

  const char *display_modes[] = { "Mirror", "Virtual", NULL };
  self->display_row = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->display_row),
                                 "Display mode");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->display_row),
    "Mirror the phone's screen, or run a separate virtual display on the device.");
  adw_combo_row_set_model (self->display_row,
                           G_LIST_MODEL (gtk_string_list_new (display_modes)));
  adw_preferences_group_add (display_group, GTK_WIDGET (self->display_row));

  /* Virtual-display geometry. Hidden unless the Virtual mode is selected (see
   * update_virtual_rows_visible). Even dimensions keep the H.264 encoder happy. */
  self->width_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (320, 7680, 2));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->width_row), "Width");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->width_row),
    "Virtual display width in pixels.");
  adw_preferences_group_add (display_group, GTK_WIDGET (self->width_row));

  self->height_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (240, 4320, 2));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->height_row), "Height");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->height_row),
    "Virtual display height in pixels.");
  adw_preferences_group_add (display_group, GTK_WIDGET (self->height_row));

  self->dpi_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (96, 640, 1));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->dpi_row), "Density (DPI)");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->dpi_row),
    "Pixel density of the virtual display.");
  adw_preferences_group_add (display_group, GTK_WIDGET (self->dpi_row));

  /* Video bitrate, in Mbps. scrcpy defaults to 8; lowering it trades image
   * quality for less bandwidth. Always shown (applies to mirror and virtual). */
  self->bitrate_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (1, 50, 1));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->bitrate_row),
                                 "Video bitrate (Mbps)");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->bitrate_row),
    "Lower it to save bandwidth. Reconnects to apply.");
  adw_preferences_group_add (display_group, GTK_WIDGET (self->bitrate_row));

  /* Blank the phone's own panel while mirroring to save its battery. The stream
   * keeps flowing and input still works; only the device backlight goes dark.
   * Reconnects to apply, like the other connect-time display options. */
  self->screen_off_row = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->screen_off_row),
                                 "Turn off phone screen");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->screen_off_row),
    "Dark the phone's panel to save battery once you unlock it. Reconnects to apply.");
  adw_preferences_group_add (display_group, GTK_WIDGET (self->screen_off_row));

  AdwPreferencesGroup *window_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (window_group, "Window");

  self->free_resize_row = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->free_resize_row),
                                 "Free resize");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->free_resize_row),
    "Resize the window freely instead of keeping the phone's aspect ratio.");
  adw_preferences_group_add (window_group, GTK_WIDGET (self->free_resize_row));

  AdwPreferencesGroup *input_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (input_group, "Input");

  const char *pointer_modes[] = { "Mouse", "Touch", NULL };
  self->pointer_row = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->pointer_row),
                                 "Pointer input");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->pointer_row),
    "How mouse input is sent to the phone.");
  adw_combo_row_set_model (self->pointer_row,
                           G_LIST_MODEL (gtk_string_list_new (pointer_modes)));
  adw_preferences_group_add (input_group, GTK_WIDGET (self->pointer_row));

  AdwPreferencesGroup *audio_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (audio_group, "Audio");

  self->audio_row = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->audio_row),
                                 "Play phone audio");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->audio_row),
    "Route the phone's sound to this computer. Reconnects to apply.");
  adw_preferences_group_add (audio_group, GTK_WIDGET (self->audio_row));

  adw_preferences_page_add (page, display_group);
  adw_preferences_page_add (page, window_group);
  adw_preferences_page_add (page, input_group);
  adw_preferences_page_add (page, audio_group);

  AdwToolbarView *tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (tv, adw_header_bar_new ());
  adw_toolbar_view_set_content (tv, GTK_WIDGET (page));

  adw_dialog_set_child (ADW_DIALOG (self), GTK_WIDGET (tv));
}

static void
pm_settings_dialog_class_init (PmSettingsDialogClass *klass)
{
}

PmSettingsDialog *
pm_settings_dialog_new (const PmSettings    *initial,
                        PmSettingsChangedCb  cb,
                        gpointer             user_data)
{
  PmSettingsDialog *self = g_object_new (PM_TYPE_SETTINGS_DIALOG, NULL);
  self->cb = cb;
  self->user_data = user_data;

  /* Seed the initial state before wiring handlers to avoid callbacks for
   * programmatic values. */
  adw_switch_row_set_active (self->free_resize_row, initial->free_resize);
  adw_combo_row_set_selected (self->pointer_row,
                              initial->mouse_mode ? PM_POINTER_MOUSE : PM_POINTER_TOUCH);
  adw_combo_row_set_selected (self->display_row,
                              initial->display_mode == PM_DISPLAY_VIRTUAL
                                ? PM_DISPLAY_ROW_VIRTUAL : PM_DISPLAY_ROW_MIRROR);
  adw_spin_row_set_value (self->width_row, initial->display_width);
  adw_spin_row_set_value (self->height_row, initial->display_height);
  adw_spin_row_set_value (self->dpi_row, initial->display_dpi);
  adw_spin_row_set_value (self->bitrate_row, initial->video_bitrate);
  adw_switch_row_set_active (self->screen_off_row, initial->screen_off);
  adw_switch_row_set_active (self->audio_row, initial->audio);

  /* Match the resolution/DPI row visibility to the seeded mode before the user
   * touches anything. */
  update_virtual_rows_visible (self);

  g_signal_connect (self->free_resize_row, "notify::active",
                    G_CALLBACK (on_setting_changed), self);
  g_signal_connect (self->pointer_row, "notify::selected",
                    G_CALLBACK (on_setting_changed), self);
  g_signal_connect (self->display_row, "notify::selected",
                    G_CALLBACK (on_setting_changed), self);
  g_signal_connect (self->width_row, "notify::value",
                    G_CALLBACK (on_setting_changed), self);
  g_signal_connect (self->height_row, "notify::value",
                    G_CALLBACK (on_setting_changed), self);
  g_signal_connect (self->dpi_row, "notify::value",
                    G_CALLBACK (on_setting_changed), self);
  g_signal_connect (self->bitrate_row, "notify::value",
                    G_CALLBACK (on_setting_changed), self);
  g_signal_connect (self->screen_off_row, "notify::active",
                    G_CALLBACK (on_setting_changed), self);
  g_signal_connect (self->audio_row, "notify::active",
                    G_CALLBACK (on_setting_changed), self);

  return self;
}
