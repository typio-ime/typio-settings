# typio-control

The GTK4 control panel for the [Typio](https://github.com/) input method
framework. Installs the `typio-control` binary.

It edits Typio's configuration through libtypio's config + schema APIs and
reflects/changes runtime state over the host's D-Bus interface.

## Building

Requires an installed [libtypio](../libtypio) (provides `libtypio.pc`) and
GTK4.

```sh
meson setup build              # add PKG_CONFIG_PATH=<prefix>/lib/pkgconfig
ninja -C build
ninja -C build install
```

## Note

This panel currently links libtypio's config/schema/engine-label APIs and
still references rime-specific schema helpers directly. Those will move
behind a generic engine property/command mechanism (a planned libtypio
change), after which the panel will render engine settings generically
from whatever the active engine reports.
