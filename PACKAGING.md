# Packaging

Specula keeps only Arch packaging in the repository. Other distributions should
install from source with Meson.

## Arch Package

Build the existing VCS package with:

```sh
makepkg -si
```

The Arch package depends on repository packages for GTK, libadwaita, FFmpeg,
PulseAudio, Avahi, Android platform tools, and scrcpy. `avahi-daemon` still has
to be enabled on the system for mDNS discovery; without it Specula falls back
to direct adb probing.

## Other Distributions

Install dependencies using your distro's normal package names, then build and
install with Meson:

```sh
meson setup build --prefix=/usr
meson compile -C build
meson install -C build
```

Runtime still needs `adb` and `scrcpy` on `PATH`, plus the scrcpy server file
provided by the distro's scrcpy package or by `PM_SERVER_JAR`.
