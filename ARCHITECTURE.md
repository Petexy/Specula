# Phone Mirror: Architecture Blueprint

Wire-free Android screen mirroring & control for the Linux desktop, in pure C.
GTK4 + libadwaita front-end, FFmpeg decode, scrcpy-style on-device server.

This document describes the architecture, and the `src/` tree implements it in
full. Every module has a real interface (`*.h`), and the live pipeline
(discovery, adb, sockets, decode, audio, input, the on-device server handshake)
is working end to end. The remaining `TODO`s are narrow refinements (routing
certain printable keycodes through `INJECT_TEXT`, correlating an mDNS service
with its saved pairing), not missing subsystems.

---

## 1. Module map

| File | Responsibility |
|------|----------------|
| `main.c` | Entry point; creates and runs `PmApplication`. |
| `pm-application.{c,h}` | `AdwApplication` subclass: lifecycle, dark-mode via `AdwStyleManager`, global actions/accels. |
| `pm-window.{c,h}` | `AdwApplicationWindow`: the `AdwViewStack` (a status page for Idle/Searching/Connecting/Error, a mirror page for Mirroring) plus all the window chrome. Drives aspect-ratio-locked resizing of the undecorated mirror window (custom resize edges/cursors), the auto-hiding header bar with a pin toggle, the first-run setup wizard, the lockscreen-PIN entry, the floating Unlock button and "Unlocking…" overlay shown over a locked mirror, and persists prefs. Observes `PmSession`. |
| `pm-session.{c,h}` | **Controller.** Owns the lifecycle state machine and worker thread; the single object the UI talks to. Emits `state-changed` plus `stream-changed`, `startup-check-failed`, `pin-required`, `unlocking`, `unlock-failed`, and `locked-changed`. |
| `pm-video-view.{c,h}` | **Renderer surface.** `GtkWidget` showing decoded `GdkTexture`s with correct aspect ratio; maps widget→device coordinates for input. |
| `pm-connect-dialog.{c,h}` | `AdwDialog` to set up a device: connect by IP[:port] and one-time `adb pair` (run async via `GTask`). Persists via `device.c`. |
| `pm-settings-dialog.{c,h}` | `AdwDialog` (bottom sheet) for the live mirror options: aspect-ratio lock, mouse vs touch, audio on/off, video bitrate, mirror vs virtual display (with width/height/DPI), and screen-off. Reports each change through a callback. |
| `discovery.{c,h}` | Background discovery: **mDNS/DNS-SD via Avahi** (`_adb-tls-connect._tcp`, `_adb._tcp`) plus a direct TCP-probe fallback thread. |
| `adb.{c,h}` | Thin wrapper over the `adb` CLI via `GSubprocess` (connect/push/forward/spawn-server). |
| `net.{c,h}` | Blocking TCP client for the video & control sockets (`TCP_NODELAY`). |
| `decoder.{c,h}` | FFmpeg (`libavcodec`) decode + `swscale` → `GdkTexture`, delivered on the main thread. |
| `audio.{c,h}` | PulseAudio (`libpulse-simple`) playback sink for the phone's raw-PCM audio stream. |
| `input.{c,h}` | GTK event controllers → scrcpy control messages, HiDPI/Wayland-correct. |
| `protocol.{c,h}` | scrcpy-compatible control-message serialization. |
| `failsafe.{c,h}` | Crash fail-safe for device-side state. Installs fatal-signal handlers that, before the process dies, flush pre-serialised control messages to release any held pointer/keys, restore the panel if it was blanked, and lock the phone, so a crash never strands it. Uses only async-signal-safe operations. |
| `device.{c,h}` | Paired-device persistence (GKeyFile under `$XDG_CONFIG_HOME`). |
| `prefs.{c,h}` | Persisted user preferences (GKeyFile), separate from pairing: the mirror options above plus a first-run `setup_complete` flag. Loads with documented defaults for missing values. |
| `pinstore.{c,h}` | Optional lockscreen-PIN storage, **encrypted at rest** (HMAC-SHA256 stream cipher, encrypt-then-MAC) and keyed by the device's Wi-Fi MAC so it survives DHCP IP changes. Drives the connect-time auto-unlock (`adb.c::pm_adb_unlock_with_pin`). |
| `pm-types.h` | Shared enums/structs (`PmState`, `PmCodec`, `PmDeviceInfo`, `PmStreamInfo`). |

Dependency direction is strictly downward: UI (`window`, `video-view`) →
controller (`session`) → services (`discovery`, `adb`, `net`, `decoder`,
`input`, `protocol`, `device`). Low-level modules never include GTK
(`pm-types.h` deliberately pulls in only GLib), so they stay unit-testable and
thread-safe.

---

## 2. Threading model

GTK is single-threaded: **all** widget calls happen on the main thread. Blocking
work is pushed to a worker and results are marshalled back.

```
┌──────────────────────────── GTK main thread ────────────────────────────┐
│ AdwApplication loop · all widgets · PmSession state machine             │
│ receives: state changes, decoded GdkTextures (via g_main_context_invoke)│
└─────────────▲───────────────────────────────────────────▲───────────────┘
              │ post_state() / deliver_frame()              │ input writes
              │ (g_main_context_invoke_full, thread-safe)   │ (tiny, inline)
┌─────────────┴───────────────┐                  ┌──────────┴──────────────┐
│   Discovery thread          │   handoff        │  Session worker thread  │
│   (probe / mDNS browse)     ├────────────────▶│  adb → forward → server │
│   one-shot: "device found"  │                  │  → net read loop → feed │
└─────────────────────────────┘                  │     decoder             │
                                                 └──────────┬──────────────┘
                                                            │ spawns once the
                                                            │ audio socket is up
                                                 ┌──────────▼──────────────┐
                                                 │  Audio worker thread    │
                                                 │  read audio socket →    │
                                                 │  PulseAudio sink        │
                                                 └─────────────────────────┘
```

- **Discovery** (`discovery.c`): Avahi browses asynchronously on the main loop
  (avahi-glib poll) *and* a fallback worker thread retries a direct TCP connect;
  whichever resolves first reports on the main thread. The session handler is
  idempotent, so a double-report is harmless.
- **Session worker** (`pm-session.c::live_worker`): runs the entire blocking
  pipeline (adb commands, socket connect, the `read → decode` loop). Stopped via
  an atomic flag; `pm_session_stop()` closes the socket to unblock `read()` and
  joins the thread.
- **Decoder** runs inside the worker; it constructs `GBytes`/`GdkTexture` and
  hands them to the main thread with `g_main_context_invoke_full`, so no GDK
  object is touched off-thread.
- **Audio worker** (`pm-session.c::audio_worker`): spawned by the session worker
  once the audio socket is accepted, it reads the raw-PCM stream and writes it
  straight to the PulseAudio sink, so a slow audio device can never stall the
  video read loop. Best-effort: any error just drops sound and leaves the mirror
  running. The session worker closes its socket (to unblock the read) and joins
  it during teardown.
- **Unlock worker** (`pm-session.c::unlock_worker`): a short-lived thread spawned
  once the stream is live to drive best-effort keyguard auto-unlock (several
  `adb` round trips with deliberate settle delays) without stalling the frame
  loop; joined during teardown.
- **Input** writes are tiny (≤32 bytes) and sent inline from the UI thread.
  *Upgrade:* move to a dedicated control-writer thread + lock-free queue if a
  blocked socket ever stalls the UI.

Synchronisation primitives in use: `g_atomic_int` (stop flags),
`g_main_context_invoke_full` (cross-thread, ref-counted payloads),
`g_thread_join` (clean shutdown). No raw mutexes needed with this design.

---

## 3. Connection lifecycle (zero-click)

```
 IDLE ──user "Connect"──▶ SEARCHING ──device found──▶ CONNECTING ──stream up──▶ MIRRORING
   ▲                           │                            │                       │
   └──────────────── stop / error / disconnect ◀───────────┴───────────────────────┘
```

Mapped to `PmState` (`pm-types.h`); `pm-window.c::pm_window_apply_state()`
projects those five states onto the two `AdwViewStack` pages (a shared status
page for Idle/Searching/Connecting/Error, the mirror page for Mirroring).

**One-time pairing** (out of band, see README): `adb pair` over Wi-Fi or a USB
handoff to `adb tcpip 5555`. The result (serial, host, port) is persisted by
`device.c`.

**Auto-connect path** (`pm-session.c::live_worker`):
1. `adb connect host:port` and confirm an authorised device.
1a. **Auto-unlock** (`maybe_unlock_device`, best-effort): read the device's
   Wi-Fi MAC, commit any PIN entered during pairing or via the **Lockscreen
   PIN…** menu to that MAC (`pinstore.c`), and, if a PIN is stored and the
   keyguard is up, wake + swipe + type it via `adb shell input`. If no PIN is
   stored, the window asks for a one-time PIN that never enters `pinstore.c` and
   submits it only once per confirmation. Every failure here is non-fatal and
   just continues.
2. `adb push scrcpy-server.jar /data/local/tmp/`.
3. `adb forward tcp:27183 localabstract:scrcpy`.
4. `adb shell CLASSPATH=… app_process / …Server …` (long-running `GSubprocess`).
   Server args encode the user's settings: `audio=`, `video_bit_rate=`, and
   `new_display=<w>x<h>/<dpi>` when the **Virtual** display mode is selected
   (omitted to mirror the physical screen; resolution defaults to 1920×1080).
   Changing the display mode or its geometry while live triggers
   `pm_session_reconnect()`, which re-runs this sequence against the same
   endpoint without re-discovering it.
5. Connect the **video** socket, then the **audio** socket (when audio is
   enabled), then the **control** socket to `127.0.0.1:27183`.
6. Read stream metadata (codec, width, height) → `PmStreamInfo`.
7. Open the decoder, switch to `MIRRORING`, enter the read/decode loop. If audio
   is enabled, the audio worker is spawned alongside it.

**While mirroring** the session holds a session-manager sleep inhibitor
(`gtk_application_inhibit`) so the desktop does not suspend or lock the screen
while you drive the phone rather than the desktop. If screen-off is enabled it
also blanks the phone's panel via `SET_DISPLAY_POWER`, but only once the device
reports unlocked (the keyguard is polled every couple of seconds from the read
loop — the same probe also raises the floating Unlock button whenever the phone
sits on its lockscreen) so it never blacks out a lock screen the user still has
to get past.

**Teardown** (`finalize_screen`, run once per session on a clean stop, a
spontaneous disconnect, or a crash): release any pointer/keys still held on the
device, restore the panel if it was blanked, then lock the phone with a
`KEYCODE_SLEEP` press — unlike `KEYCODE_POWER`'s toggle, SLEEP always sleeps
(never wakes), so it locks regardless of the current panel state and never races
a just-restored display awake on slower OEM panels. The server is deliberately
launched **without**
`power_off_on_close` so a settings reconnect does not lock on every change; the
lock is injected here instead, and is skipped during a reconnect so the next
session inherits a live, unlocked phone. The crash fail-safe (`failsafe.c`)
performs the same release-and-lock from a fatal-signal handler, covering an
abnormal exit that no in-process teardown can.

---

## 4. Video pipeline data flow

```
 device MediaCodec ─H.264/265─▶ TCP ─▶ net.c ─chunks─▶ decoder.c
                                                          │ av_parser_parse2
                                                          │ avcodec_send/receive
                                                          ▼
                                                  AVFrame (YUV)
                                                          │ sws_scale → RGB24
                                                          ▼
                                            GdkMemoryTexture (GBytes)
                                                          │ g_main_context_invoke
                                                          ▼
                                  main thread: PmVideoView ← GtkPicture (GPU scale)
```

The current path is portable **software decode + CPU colour-convert**. See §7
for the zero-copy upgrade.

**Audio** rides a second server socket (accepted between the video and control
sockets). We start the server with `audio=true audio_codec=raw`, so, with frame
metadata already disabled, the phone streams its output as an undelimited
48 kHz / stereo / S16LE PCM byte stream after a 4-byte codec-id header. A
dedicated audio worker thread reads that socket and writes straight to the
desktop's default sink via PulseAudio (`audio.c`); no FFmpeg decode is involved.
Audio is **best-effort**: if the device can't capture (the server replies with a
zero codec id, e.g. Android < 11) or no audio server is reachable, the worker
logs and exits, leaving the video mirror untouched.

---

## 5. Input & coordinate mapping

`input.c` installs GTK controllers on `PmVideoView`:

- One `GtkGestureClick` per mouse button + a `GtkEventControllerMotion` → touch
  DOWN / MOVE / UP (tap, drag, and hover), or full mouse-button events
  (independent button holds and chording) when mouse mode is on.
- `GtkEventControllerScroll` → scroll events at the last pointer position.
- `GtkEventControllerKey`: a printable character with no Ctrl/Alt/Super is
  injected as UTF-8 via `INJECT_TEXT` (so the device IME handles composition and
  capitalisation); everything else maps to a navigation/editing keycode. A few
  printable keycodes in the lookup table are still left for `INJECT_TEXT` to
  cover (`TODO`).

**Wayland / HiDPI correctness:** GTK delivers coordinates in *logical* pixels.
`pm_video_view_widget_to_device()` converts a logical point to a normalised
`[0,1]²` position over the **video content only** (replicating
`GTK_CONTENT_FIT_CONTAIN`, so letterbox clicks are rejected). `protocol.c` then
scales the normalised point by the device frame size the server advertised, so
the mapping is independent of window size, monitor scale factor, and rotation.

---

## 6. On-device server protocol

We reuse scrcpy's server contract (the spec's "`scrcpy-server.jar` style"):
control messages are a 1-byte type tag + fixed big-endian payload
(`protocol.c`). This is version-coupled: the wire layout must match the jar
you ship. Pin a known scrcpy-server release and keep `protocol.c` /
`read_stream_meta()` in lockstep (see README "Server component").

---
