app_id = 'io.github.petexy.Specula'

# Application icon: installed into the hicolor theme so GTK/libadwaita and the
# compositor resolve it by the app id (AdwApplicationWindow / AdwAboutDialog
# already reference PM_APP_ID).
install_data(
  'icons' / app_id + '.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

# Symbolic (monochrome) variant for menus / headerbars. Lives in the symbolic
# context so GTK recolours it to the current theme foreground.
install_data(
  'icons' / app_id + '-symbolic.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'symbolic' / 'apps',
)

# Desktop entry: lets the launcher and the Wayland compositor associate the
# window (app id) with the icon above.
desktop_file = app_id + '.desktop'
install_data(
  desktop_file,
  install_dir : get_option('datadir') / 'applications',
)

# Validate the desktop file when the tool is available (non-fatal otherwise).
desktop_validate = find_program('desktop-file-validate', required : false)
if desktop_validate.found()
  test('validate-desktop-file',
    desktop_validate,
    args : [meson.current_source_dir() / desktop_file],
  )
endif

# AppStream metainfo: describes the app for software centres.
metainfo_file = app_id + '.metainfo.xml'
install_data(
  metainfo_file,
  install_dir : get_option('datadir') / 'metainfo',
)

# Validate the metainfo against the AppStream spec when the tool is available.
appstreamcli = find_program('appstreamcli', required : false)
if appstreamcli.found()
  test('validate-metainfo',
    appstreamcli,
    args : ['validate', '--no-net', meson.current_source_dir() / metainfo_file],
  )
endif

# Refresh the icon cache and desktop database on install.
gnome.post_install(
  gtk_update_icon_cache : true,
  update_desktop_database : true,
)
