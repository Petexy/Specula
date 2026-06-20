project('specula', 'c',
  version : '0.5.0',
  license : 'GPL-3.0-or-later',
  meson_version : '>= 1.0.0',
  default_options : [
    'warning_level=2',
    'c_std=c11',
    'buildtype=debugoptimized',
  ],
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# UI stack
gtk_dep   = dependency('gtk4',         version : '>= 4.10')
adw_dep   = dependency('libadwaita-1', version : '>= 1.4')

# Video decode stack (FFmpeg)
avcodec_dep  = dependency('libavcodec')
avformat_dep = dependency('libavformat')
avutil_dep   = dependency('libavutil')
swscale_dep  = dependency('libswscale')

# Audio playback: the phone's audio is streamed as raw PCM and played out the
# desktop's default sink via PulseAudio's simple API (also served by PipeWire).
pulse_simple_dep = dependency('libpulse-simple')

# GLib threading / sockets come transitively via gtk (gio/gobject/glib).
gio_dep    = dependency('gio-2.0')
gthread_dep = dependency('gthread-2.0')

# mDNS / DNS-SD discovery (optional: degrade to a direct TCP probe without it).
avahi_client_dep = dependency('avahi-client', required : get_option('mdns'))
avahi_glib_dep   = dependency('avahi-glib',   required : get_option('mdns'))
have_avahi = avahi_client_dep.found() and avahi_glib_dep.found()

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)

# GNOME helper module: used by data/ for post-install icon-cache / desktop-db
# refresh.
gnome = import('gnome')

# Internationalisation: gettext-based translations live under po/.
i18n = import('i18n')

project_deps = [
  gtk_dep, adw_dep,
  avcodec_dep, avformat_dep, avutil_dep, swscale_dep,
  pulse_simple_dep,
  gio_dep, gthread_dep, m_dep,
]
if have_avahi
  project_deps += [avahi_client_dep, avahi_glib_dep]
endif

# ---------------------------------------------------------------------------
# Global config
# ---------------------------------------------------------------------------
conf = configuration_data()
conf.set_quoted('PM_VERSION', meson.project_version())
conf.set_quoted('PM_APP_ID', 'io.github.petexy.Specula')
# Source icons dir for uninstalled runs; the installed copy lives in the
# hicolor theme.
conf.set_quoted('PM_SOURCE_ICONS_DIR', meson.project_source_root() / 'data' / 'icons')
gettext_package = 'specula'
conf.set_quoted('GETTEXT_PACKAGE', gettext_package)
# Where the compiled .mo catalogs are installed; used at runtime for
# bindtextdomain() so translations resolve from the system locale dir.
conf.set_quoted('PM_LOCALEDIR', get_option('prefix') / get_option('localedir'))
conf.set10('PM_HAVE_AVAHI', have_avahi)
configure_file(output : 'pm-config.h', configuration : conf)
config_inc = include_directories('.')

add_project_arguments(
  ['-DGTK_DISABLE_DEPRECATED', '-DADW_DISABLE_DEPRECATED'],
  language : 'c',
)

# These two warnings fire constantly on idiomatic GLib/GTK code (fixed
# callback signatures; GActionEntry's trailing padding field). Silence them
# the way GNOME projects do, while keeping the rest of -Wall -Wextra.
add_project_arguments(
  cc.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
  ]),
  language : 'c',
)

subdir('data')
subdir('po')
subdir('src')
