# ADR-0001: Record Architecture Decisions

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Project maintainers

## Context

`typio-settings` is the GTK4 settings panel for the Typio input method. It edits Typio's configuration through `libtypio`'s config and schema APIs and reflects or changes runtime state over the host's D-Bus interface. As the panel grows, design choices about scope, dependencies, and the boundary against the daemon must be remembered.

## Decision

This project uses Architecture Decision Records (ADRs) stored in `docs/adr/`.

- Each ADR is numbered sequentially and is append-only after acceptance.
- To change a past decision, write a new ADR that supersedes the old one and update the old ADR's status field only.
- ADRs are short (ideally one page) and focus on context, decision, alternatives, and consequences.

## Alternatives considered

- **Inline design comments in code**: Rejected. Comments describe *what* the code does, not *why* a larger design choice was made.
- **Long-form architecture documents only**: Rejected. Explanation docs are mutable; ADRs provide an immutable anchor.

## Consequences

- Positive: new contributors understand why key boundaries exist without reading the entire commit history.
- Positive: reviewers can require an ADR for architectural changes.
- Trade-off: maintainers must remember to write ADRs for significant decisions.
