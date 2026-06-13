# ADR-0002: Independent Settings-Panel Repository and `typio-settings` Naming

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Project maintainers

## Context

The GTK4 settings panel was historically a subdirectory of the Typio monorepo, sharing a release cadence with the daemon and the framework core. Two pressures broke that arrangement:

- **The panel has its own dependency surface.** GTK4, libadwaita, and their plug-in ecosystem evolve on a different cadence from the daemon's Wayland/Vulkan/D-Bus stack. Mixing them in one tree forces every contributor to install both stacks even to work on either.
- **The panel name was ambiguous.** Earlier names overlapped with the daemon (`typio`, `typio-control`) and with the CLI (which became `typioctl` — see the `typioctl` repository's ADRs). The settings panel is the only graphical configuration UI, so the name should say what it is.

## Decision

The settings panel exists as an **independent repository** named `typio-settings`, building a single binary of the same name.

- The panel depends only on `libtypio` (via `libtypio.pc`) and GTK4. It does not link any Wayland host code.
- It edits configuration through `libtypio`'s config + schema APIs (TOML files under XDG config) and reads / writes runtime state through the host's D-Bus interface — never via direct calls into the daemon process.
- The name `typio-settings` is deliberately scope-specific: it edits *settings*. It is not a general-purpose control surface; tray / status menu / runtime control belong to the host or to `typioctl`.

## Alternatives considered

- **Keep the panel in the monorepo.** Rejected: forces every contributor and packager to install both the GTK4 stack and the Wayland/Vulkan stack, even to work on either piece in isolation. Also blocks the panel from following GTK release cadence.
- **Name it `typio-control` (or similar generic).** Rejected: overlaps semantically with `typioctl` (CLI) and with general "control surface" language used for the host's D-Bus interface. Generic naming makes documentation ambiguous.
- **Embed daemon logic via FFI for direct runtime control.** Rejected: would force the panel to link Wayland / Vulkan and defeat the independent-repository split.

## Consequences

- Positive: the panel can adopt new GTK / libadwaita versions and design patterns without coordinating with the daemon release.
- Positive: packagers can ship the panel separately; headless servers omit it.
- Positive: the name `typio-settings` reads cleanly in launchers, application menus, and `.desktop` files.
- Trade-off: the D-Bus interface is now a public contract. Changes must be coordinated with the host.
- Negative (accepted): users who only want CLI-level configuration can use `typioctl` or edit TOML directly — but a graphical user must install one more package.

## Related

- `typio-wayland` repository — owns the D-Bus interface this panel consumes
- `typioctl` repository — the CLI counterpart that edits the same configuration over the same channels
