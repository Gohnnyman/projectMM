# Plans

Approved feature plans, one per feature, the **design-intent** record: what we set out to build and why, written in `/plan` mode before implementation. This is one of the four homes for accumulated knowledge, and each holds a different genre:

- **plans/** (here) the forward-looking intent, what a feature meant to do.
- **[../lessons.md](../lessons.md)** the debugging lessons, a bug, its cause, the fix.
- **[../../adr/](../../adr/README.md)** the architectural decisions, why approach A over B.
- **[CLAUDE.md](../../../CLAUDE.md) / [coding-standards.md](../../coding-standards.md)** the durable rules a lesson hardened into.

## Naming

`Plan-YYYYMMDD - <title>.md`, ISO-8601 date first so the directory sorts chronologically. The outcome rides in the filename as a trailing parenthetical: `… (shipped).md` once the design lands, `… (attempted, abandoned).md` if it was tried and dropped. An unmarked plan is still in flight.

## Rules

**Kept, not pruned.** Plans are the permanent design-intent record, they are not deleted when a lesson is absorbed (that is the lessons/ADR rule, not this one). The one exception: a multi-phase effort's per-phase plans may be consolidated into a single `… (shipped).md` once the whole effort lands, provided the consolidated record preserves each phase's intent and outcome.

**Agents write, don't auto-read.** An agent writes a plan here when creating one, but does not read the existing plan files for context unless the product owner points to one (the same rule as the rest of `history/`). This is a product-owner reference archive.
