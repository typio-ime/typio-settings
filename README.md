# typio-settings

The flux-ui settings panel for the [Typio](https://github.com/) input method
framework. Installs the `typio-settings` binary.

It edits Typio's configuration through libtypio's config + schema APIs and
reflects/changes runtime state over the host's TIP v1 UDS interface.

## Building

Requires installed [libtypio](../libtypio), [flux](../flux), and
[flux-ui](../flux-ui) (providing `libtypio.pc`, `flux.pc`, and `flux-ui.pc`).

```sh
meson setup build              # add PKG_CONFIG_PATH=<prefix>/lib/pkgconfig
ninja -C build
ninja -C build install
```

## Note

This panel currently links libtypio's config/schema/engine-label APIs and
still references rime-specific schema helpers directly. As a transitional
measure, the Rime schema dropdown ships with a no-op list loader: it
shows "Unselected" plus whatever schema is currently configured in TOML,
but cannot enumerate available schemas. Browsing the full list will
require either reintroducing a schema-list API in libtypio or moving the
generic engine property/command mechanism planned for libtypio, after
which the panel will render engine settings generically from whatever
the active engine reports.
