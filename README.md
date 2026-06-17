# Phone Mirror

Wire-free Android screen mirroring and control for the Linux desktop. Written in
pure C with GTK4 and libadwaita, Wayland-native, FFmpeg decode, scrcpy-style
on-device server. The phone's audio is forwarded too, so the desktop acts as its
headphones.

> Phone Mirror is the user-facing name; the project's internal name (binary,
> app id, source) is Specula, much like GNOME *Files* is *Nautilus*. The
> binary is `specula` and the application id is `io.github.petexy.Specula`.
> Source: <https://github.com/Petexy/specula>.

> Status: working implementation. Compiles cleanly, launches, and runs the full
> UI state machine (Searching, Connecting, Mirroring). Already real:
> mDNS/DNS-SD discovery (Avahi) with a TCP-probe fallback, a device
> setup dialog (connect by IP, one-time `adb pair`), persisted pairing,
> HiDPI-correct touch/scroll/keyboard input (incl. UTF-8 text injection),
> forwarded audio, optional lockscreen-PIN auto-unlock, a battery-saving
> screen-off mode, and a distraction-free mirror window (undecorated, with
> auto-hiding controls and aspect-ratio-locked resizing). The phone is locked
> again when the session ends. The live pipeline needs a paired device and a
> scrcpy-server jar; an explicit `PM_DEMO=1` mode can render a generated test
> pattern when no phone is available. See [ARCHITECTURE.md](ARCHITECTURE.md) for
> the design and roadmap.

## Quick start

```sh
meson setup build
ninja -C build
./build/src/specula           # click "Connect" to find and mirror your phone
```

### Dependencies

Runtime/build libraries (all detected via `pkg-config`):

- `gtk4` ≥ 4.10, `libadwaita-1` ≥ 1.4
- `libavcodec`, `libavformat`, `libavutil`, `libswscale` (FFmpeg)
- `libpulse-simple` (audio playback; PipeWire's PulseAudio shim works too)
- `gio-2.0`, `gthread-2.0` (part of GLib)
- `adb` on `PATH` (android-tools) for the live pipeline

On Arch/CachyOS:

```sh
sudo pacman -S gtk4 libadwaita ffmpeg libpulse android-tools meson avahi
```

mDNS discovery uses Avahi (`avahi-client`, `avahi-glib`) and the running
`avahi-daemon`; without it the app falls back to a direct TCP probe of the saved
host:port.

## Running the live pipeline

1. **Pair once** (either method):
   - *Wi-Fi (Android 11+):* enable Wireless debugging, then
     `adb pair <ip>:<pair-port>` with the on-device code.
   - *USB handoff:* `adb tcpip 5555` while plugged in, then unplug.
2. **Server component:** install `scrcpy` so the app can find its bundled
   server and version, or download a matching `scrcpy-server` jar and point the
   app at it (the wire protocol is version-coupled, see [ARCHITECTURE.md §6](ARCHITECTURE.md)):
   ```sh
   PM_SERVER_JAR=/path/to/scrcpy-server ./build/src/specula
   ```
   If you provide a standalone server file without the `scrcpy` binary on
   `PATH`, also set `PM_SCRCPY_VERSION` to that server's version.
3. Click **Connect**. The app runs `adb connect → push → forward → app_process`,
   opens the video/control sockets, decodes, and renders.

Persisted pairing lives at `$XDG_CONFIG_HOME/specula/device.ini`.

For renderer/UI demos without a phone, run:

```sh
PM_DEMO=1 ./build/src/specula
```

## Layout

```
meson.build            top-level build (deps, warnings, config header)
src/
  meson.build          executable target
  main.c               entry point
  pm-application.{c,h} AdwApplication: lifecycle, dark mode, actions
  pm-window.{c,h}      AdwApplicationWindow: AdwViewStack states, actions
  pm-connect-dialog.{c,h} device setup: connect by IP + async adb pair
  pm-settings-dialog.{c,h} live mirror options (display, audio, bitrate, input)
  pm-session.{c,h}     pipeline controller + state machine (worker thread)
  pm-video-view.{c,h}  renderer surface (GdkTexture) + coordinate mapping
  discovery.{c,h}      device discovery: Avahi mDNS + TCP-probe fallback
  adb.{c,h}            adb CLI wrapper (GSubprocess)
  net.{c,h}            blocking TCP client (TCP_NODELAY)
  decoder.{c,h}        FFmpeg decode + swscale → GdkTexture
  audio.{c,h}          PulseAudio sink for the phone's raw-PCM audio stream
  input.{c,h}          GTK events → scrcpy control messages (HiDPI-correct)
  protocol.{c,h}       scrcpy-compatible control serialization
  failsafe.{c,h}       crash handler: release input and lock the phone on exit
  device.{c,h}         paired-device persistence
  pinstore.{c,h}       optional encrypted lockscreen-PIN storage
  prefs.{c,h}          persisted user preferences
  pm-types.h           shared types
data/                  desktop entry, metainfo, icons
ARCHITECTURE.md        design blueprint, threading model, roadmap
```

## License

GPL-3.0-or-later.
