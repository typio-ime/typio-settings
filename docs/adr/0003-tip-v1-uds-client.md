# ADR-0003: TIP v1 UDS client — drop GDBus, push events replace property polling

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Project maintainers

## Context

`typio-settings` historically talked to the daemon via D-Bus (`org.typio.InputMethod1`):

- A `GDBusProxy` per app instance, watched via `g_bus_watch_name`.
- All UI state derived from `g_dbus_proxy_get_cached_property` — a flat list of D-Bus properties (`ActiveKeyboardEngine`, `AvailableKeyboardEngines`, `ConfigText`, `EngineDisplayNames`, …).
- Engine activation called `ActivateEngine`; whole-config writes called `SetConfigText`; rime-specific actions called `SetRimeSchema` and `DeployRimeConfig`.
- The `g-properties-changed` signal triggered `settings_refresh_from_proxy` to re-render the entire UI.

The daemon-side control surface that supported all of this was deleted in typio-wayland ADR-0008. The `org.typio.InputMethod1` interface, `state/dbus.c`, `ipc/dbus_protocol.h`, the `enable_status_bus` build option, and every legacy method (`GetAll`, `SetRimeSchema`, `SetConfigText`, `InvokeEngineCommand`, …) are gone. The new contract is **TIP v1**: JSON-RPC 2.0 over a UDS, resource-oriented method namespaces (`config.*`, `engine.*`, `daemon.*`, `events.subscribe`), camelCase JSON, dotted config keys against the unified `TypioConfig` tree, and push-based events.

`typio-settings` will not compile against the new daemon as-is — every `g_dbus_proxy_*` call references a method or property that no longer exists.

## Decision

Replace GDBus with a thin in-process TIP v1 client and rebind the settings UI to the new vocabulary in the same change.

### Client architecture

- New `tip_client.{c,h}` implementing:
  - UDS connect + length-prefixed framing (4-byte BE + JSON body)
  - Synchronous request/response: `tip_client_call(client, "engine.use", json_params)`
  - A second long-lived connection for `events.subscribe`; its read side is integrated into the GTK main loop via a `GSource` so notifications dispatch like any other event source
  - A small JSON helper (hand-rolled, no external dep) for the limited shapes the settings app cares about
- A typed in-memory snapshot (`TipSnapshot`) maintained by the client:
  - Active keyboard / voice engines
  - Engine list (name, kind, displayName, active)
  - Engine descriptors (cached on demand via `engine.describe`)
  - Raw config text (refreshed via `config.show`)
- On `engine.changed`, `engine.modeChanged`, `daemon.shuttingDown` notifications the snapshot updates and emits a Glib signal (or callback) for the UI to refresh.

### UI rebind

- Replace `g_dbus_proxy_get_cached_property(proxy, "ActiveKeyboardEngine")` with `tip_snapshot_active_keyboard()`.
- Replace `g_dbus_proxy_call_sync(..., "ActivateEngine", ...)` with `tip_client_call("engine.use", {name})`.
- Replace `g_dbus_proxy_call(..., "SetRimeSchema", schema)` with `tip_client_call("config.set", {key:"engines.rime.schema", value:schema})`.
- Replace `g_dbus_proxy_call(..., "DeployRimeConfig")` with `tip_client_call("engine.invoke", {name:"rime", command:"deploy"})`.
- Replace `g_dbus_proxy_call(..., "SetConfigText", text)` with per-binding writes: the form bindings (`SettingsBinding[]`) already track dotted keys, so each dirty binding calls `tip_client_call("config.set", {key, value})` on save. The raw text view becomes read-only (sourced from `config.show`).
- Replace `g_signal_connect(proxy, "g-properties-changed", ...)` with `tip_client_subscribe(client, topics)` and a topic dispatcher.

### Removed surface

- `GDBusProxy`, `g_bus_watch_name`, `on_proxy_properties_changed`, `on_name_appeared`, `on_name_vanished`.
- `#include "typio/ipc/dbus_protocol.h"` (header no longer exists in libtypio per ADR-0007 already).
- Every reference to the legacy property/method constants.
- The "name vanished / availability label" pattern keyed to a D-Bus name; replaced by a single "connected to typiod?" indicator backed by the UDS connect attempt.
- Direct whole-blob `SetConfigText` write path.

### Build deps

- Drop `gio-2.0` if `gtk4` no longer brings it transitively (it does — keep `gtk4` only, which already pulls in glib).
- No new external dep: the JSON encoder/decoder is small enough to hand-roll for the shapes used here.

## Alternatives considered

- **Keep GDBus by reintroducing a D-Bus wrapper in the daemon.** Rejected per project mandate (greenfield, no backward compatibility). The daemon's removed D-Bus surface was the explicit subject of typio-wayland ADR-0008 and ADR-0007; reintroducing it would invalidate that decision and double the transport count again.
- **Keep `SetConfigText` semantics: settings reads the raw TOML, lets the user edit it, writes the whole file back.** Rejected because the new daemon protocol intentionally does not have a whole-file write; ADR-0008 picked typed key/value writes precisely to avoid the "replace 200 KB of TOML to flip one boolean" pattern. The structured form already has the per-key writes for free.
- **Vendor a full JSON library (json-c, jansson).** Rejected as overkill for the few message shapes this client exchanges; hand-rolling the parser keeps the dependency surface flat and avoids a packaging step downstream.
- **Use libsoup or a generic JSON-RPC client.** Same overkill argument; libsoup is HTTP-focused and the UDS path has no HTTP layer.

## Consequences

- Positive: settings app and daemon share one transport again (UDS only), keeping ADR-0008's "one source of truth" property end-to-end.
- Positive: UI updates are push-based — the panel reflects any change made via `typioctl` or any other client within one socket round-trip, with no polling.
- Positive: ~250 lines of GDBus boilerplate deleted from settings.
- Positive: write paths align with the unified config tree, so what the settings panel writes is exactly what `typioctl config set` writes — no parallel persistence model.
- Trade-off: settings now requires the daemon's UDS socket at `$XDG_RUNTIME_DIR/typio/daemon.sock`. The previous D-Bus name-watching gracefully handled the daemon coming and going; the UDS client polls a reconnect every few seconds when the socket is missing.
- Trade-off: per-binding writes generate one RPC each on rapid edits; the existing autosave debouncer (75–250 ms) is repurposed to coalesce them.
- Negative (accepted): raw-text editing of `core.toml` from inside the settings panel becomes read-only. Users who want bulk text edits use `$EDITOR` directly or `typioctl config edit` (also read-only preview in TIP v1) — both edit the on-disk file.

## Related

- [typio-wayland ADR-0008: TIP v1 — IPC Protocol with resource namespaces](../../../typio-wayland/docs/adr/0008-ipc-protocol-resource-namespaces-uds-only.md) — the contract this client implements.
- [libtypio ADR-0008: engine properties unified into the config schema](../../../libtypio/docs/adr/0008-engine-properties-unified-into-config-schema.md) — explains why `SetRimeSchema` is replaced by `config.set engines.rime.schema`.
- [typioctl ADR-0004: Resource+verb CLI schema on TIP v1](../../../typioctl/docs/adr/0004-resource-verb-cli-schema-on-tip.md) — sibling client; same RPC vocabulary.
