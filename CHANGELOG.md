# Changelog

## 1.0.0 - 2026-06-26

First stable release of Phone Mirror / Specula. This release is based on the
last development release, `v0.8.0`, and focuses on making shutdown, reconnect,
and disconnect paths safer for regular use.

### Fixed

- Prevented queued decoder frame callbacks from outliving the session object
  they target during teardown.
- Avoided sending input-release messages through a freed control socket after a
  spontaneous disconnect.
- Cleaned up input controllers as soon as the app leaves the live mirroring
  state, so the next successful session starts with fresh control state.
- Detached the video view when the main window is disposed, preventing stale
  session-to-widget references during application shutdown.
- Made scrcpy-server argument construction tolerant of optional values, so
  omitted bitrate or virtual-display arguments no longer truncate later server
  options.

### Changed

- Bumped the application version to `1.0.0`.
- Updated Arch packaging metadata for the stable release.
- Updated translation catalog headers to `specula 1.0.0`.
- Expanded the README localization section to document all shipped
  translations: German, Spanish, French, Hindi, Polish, Brazilian Portuguese,
  Russian, and Simplified Chinese.
- Refreshed architecture notes for the current session signals, auto-unlock
  worker, lock behavior, locked-screen unlock UI, and input-controller model.

### Notes

- This release keeps the existing live mirroring feature set: wireless Android
  discovery and pairing, video mirroring, audio forwarding, keyboard/mouse/touch
  control, optional PIN auto-unlock, screen-off mode, virtual-display support,
  and the undecorated mirror window.
