---
name: stellux-gfx2-campaign
description: Continue the Stellux graphics stack rewrite (window protocol, libstlxwin, libstlxui, stlxdm, libstlxgfx repairs). Use when implementing any part of the new graphics stack, picking up the rework in a fresh session, or when the user references the graphics redesign or its stage numbers.
---

# The gfx2 Campaign

A ground-up rewrite of the userland graphics stack. The owner keeps the
full design study outside the repo and drives review from it; ask for
design context when a decision needs more depth than this skill carries.

## Settled architecture (do not relitigate)

- Layers: libstlxgfx (C raster, kept and repaired), libstlxwin (C
  protocol client, new), libstlxui (C++ toolkit, namespace ui::, new),
  stlxdm (compositor, rewritten in C++). Wire structs use the swp_
  prefix in stlxwin_proto.h, shared by library and compositor.
- Transport: one unix stream socket per client for all messages and
  wakeups, client-allocated named /dev/shm buffers for pixels. No
  shared-memory atomics, no cross-process futex (the kernel keys
  futexes per address space), no fd passing (kernel has none).
- Frame lifecycle: atomic COMMIT (buffer + damage + resize ack),
  RELEASE per buffer, FRAME_DONE per presentation (never per commit),
  only-latest-wins for pending commits. The compositor retains the
  displayed buffer, so animating windows settle at two buffers.
- Resize: CONFIGURE carries a serial, a configure obliges an ack
  commit even from idle clients, displayed geometry is always the
  displayed buffer's geometry.
- One pixel format everywhere: XRGB8888 little endian, stride is
  width times four. Present converts if scanout ever differs.
- Server-side decorations. Popups are parented windows with an
  optional grab that also takes keyboard input. Everything an app
  waits on is a pollable fd, zero wakeups when idle.

## The working agreement with the owner

1. Everything lands on the campaign branch as small independently
   buildable commits. Never commit to master.
2. Headers first: a module's first unit is its finished header alone,
   full doxygen, stanza formatting. Review happens before any
   implementation exists.
3. One commit-sized unit per turn: a logical cluster of functions,
   reviewable in about five minutes. Present the diff and evidence,
   then STOP. The owner reviews BEFORE the commit is made. Never
   commit unreviewed work.
4. Delete-first: when a file is replaced rather than repaired, land an
   explicit deletion commit, then fresh files. No incremental mutation
   of old code into new shapes.
5. No dual systems and no backwards compatibility: the old stack may
   break during the rewrite, but every commit must still BUILD on both
   architectures.
6. Comments stay one to two lines, intent only, no semicolons,
   per the style rule.
   Code and skills never reference the owner's external design notes.
7. Evidence per unit: clean builds on x86_64 and aarch64, plus the
   verification the stellux-gfx-testing skill prescribes.

## Stage map

0. Blend hot-path repair in libstlxgfx (byte-exact, measured on target).
1. Wipe the old window protocol halves, land stlxwin_proto.h +
   libstlxwin + the protocol core of the rewritten stlxdm.
2. Damage-driven compositor: scene, damage engine, present backend
   interface (memcpy today, page flip later).
3. Port stlxterm and doom, then delete every old-protocol remnant.
4. Text engine: UTF-8, baseline-anchored API, glyph atlas with LRU.
5. libstlxui toolkit, compositor panels on a direct host, settings app
   as the first consumer.
