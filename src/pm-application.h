/* pm-application.h - AdwApplication subclass: app lifecycle + actions. */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PM_TYPE_APPLICATION (pm_application_get_type ())
G_DECLARE_FINAL_TYPE (PmApplication, pm_application, PM, APPLICATION, AdwApplication)

PmApplication *pm_application_new (void);

G_END_DECLS
