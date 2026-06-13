# typio-settings Documentation

This directory contains documentation for `typio-settings` — the GTK4 settings
panel for the Typio input method framework.

## What's Inside

| Document | Description |
|----------|-------------|
| [`explanation/control-surfaces.md`](explanation/control-surfaces.md) | Design rules, editing model, and component guidelines for `typio-settings` and other control surfaces |
| [`explanation/config-runtime-ownership.md`](explanation/config-runtime-ownership.md) | Ownership model for persisted config, runtime state, local staged edits, and view state |
| [`explanation/configuration-system.md`](explanation/configuration-system.md) | How Typio's configuration schema, load/apply/default lifecycle works |
| [`reference/dbus-interface.md`](reference/dbus-interface.md) | D-Bus status bus interface — the protocol `typio-settings` uses to talk to the daemon |
| [`reference/ipc-protocol.md`](reference/ipc-protocol.md) | UDS IPC transport — the primary protocol used by the `typio` CLI |
| [`reference/configuration.md`](reference/configuration.md) | User-facing configuration keys, types, defaults, and descriptions |
