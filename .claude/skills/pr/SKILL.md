---
name: pr
description: Write the body of an openESL pull request: Summary with small visuals, Evidence before/after, and Merge Danger. Use when opening or rewriting a PR description.
---

# PR body format (openESL)

Inspired by the `/pr` skill from aihero.dev (mattpocock/skills). This is a format for the PR description, not a workflow: it does not push branches or open PRs by itself.

## Project rules (always apply)
- No AI attribution anywhere: no Co-Authored-By trailers, no "Generated with ..." lines, no session or thread links, no mention of AI tools in commit messages, PR title/body, comments or docs.
- No tool names in branch names; use a descriptive name such as `ap-flash-keep-settings`.
- PRs target `development` (not `master`; master is the release branch).
- Build and test locally before pushing; do not use GitHub CI as the iteration loop.
- Show the maintainer the diff and the PR text and wait for a go before committing, pushing or opening the PR.
- PR title and body in English.
- If the repo has a PR template, use its headings and fit these sections into it.

## The body has three sections

### Summary
One or more small visuals, each with one or two lines of text next to it. Pick what fits the change:
- pseudocode of the new logic
- a call tree (e.g. AP: `handleRadio()` -> `sendBlock()` -> ...)
- a file tree of what was added/moved
- a Mermaid diagram (sequence for AP <-> tag radio flows, flowchart for state machines)
- a shaped diff: only the few lines that matter, trimmed

Keep it short. A reader should understand what changed without opening the diff.

### Evidence
Before and after:
- Visual change (dashboard, tag image): screenshots or rendered PNGs of before and after.
- Firmware/behaviour: the test or serial/console output that failed before and passes now, or a log from real hardware (tag size, AP board).
- Say what was run locally (build command, tests, hardware tested on). If something was not tested on hardware, say so plainly.

### Merge Danger
- Door: **one-way** (hard to undo: flash layout, persisted settings format, OTA protocol, radio protocol, anything already-flashed tags depend on) or **two-way** (cheap to revert: UI, docs, dashboard).
- Blast radius: what breaks if this is wrong (e.g. tags stop checking in, AP loses settings, OTA bricks a tag, only the dashboard page).
- Be honest; this is a judgment call that the reviewer will check.

## Notes
- Rewrite the body after large changes to the PR; it does not update itself.
- Review comments are handled separately; this skill only shapes the body.
