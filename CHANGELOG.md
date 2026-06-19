# Changelog

## Unreleased

## v0.1.0 - 2026-06-13

- Initial flux-ui port of the Typio settings panel.
- Direct link against libtypio for config/schema/string APIs; runtime
  state mirrored over the daemon's TIP v1 UDS interface (control +
  events.subscribe stream).
- Native Wayland backend (xdg-shell toplevel, xkbcommon, text-input-v3
  IME, clipboard, HiDPI buffer scale) with Vulkan rendering via flux.
- Reset the project version to `0.1.0` to reflect pre-1.0 status; the
  prior `5.0.0` tag was inherited from a GTK prototype and did not signal
  a stability promise.
- `tip_engine_use` / `tip_engine_next` dispatch to the modality-explicit
  `keyboard.use` / `voice.use` / `keyboard.next` / `voice.next` verbs
  (TIP v2, ADR-0026), resolving the kind from the cached snapshot.
