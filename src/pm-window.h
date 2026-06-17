/* pm-window.h - main application window (AdwApplicationWindow). */
#pragma once

#include <adwaita.h>
#include "pm-application.h"

G_BEGIN_DECLS

#define PM_TYPE_WINDOW (pm_window_get_type ())
G_DECLARE_FINAL_TYPE (PmWindow, pm_window, PM, WINDOW, AdwApplicationWindow)

PmWindow *pm_window_new (PmApplication *app);
gboolean pm_window_should_defer_present (PmWindow *self);

G_END_DECLS
