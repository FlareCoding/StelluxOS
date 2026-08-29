---
name: stellux-gfx-testing
description: Verify Stellux userland graphics changes with host differential harnesses, byte-exact screenshot comparison, and in-target perf measurement. Use when changing libstlxgfx, libstlxwin, stlxdm, or any pixel-producing userland code, and when the task needs before/after visual evidence or a pixel-identical guarantee.
---

# Stellux Graphics Testing

Boot and GUI mechanics live in the stellux-qemu-testing skill; this
skill covers what to verify and how for graphics changes specifically.

## Host differential harness (the strongest correctness evidence)

libstlxgfx sources are plain C over stdint and compile on the host.
For any pixel-math change, build a scratch host harness that runs the
old and new formulas over the FULL input domain and asserts equality:

```bash
cc -O2 -I userland/lib/libstlxgfx/include -o /tmp/harness harness.c && /tmp/harness
```

- Per-channel blend math has a small domain: alpha x src x dst is
  256^3, about 16.7M cases, well under a second. Exhaustive beats
  sampling; never sample when exhaustion is affordable.
- Keep harnesses out of the tree (scratch files) unless the owner asks
  for in-tree tests.
- Divide-by-255 shortcuts are a known trap. Exact forms, verified
  exhaustively: truncating `(x + 1 + (x >> 8)) >> 8` and rounding
  `(x + 128) * 257 >> 16`, for x in [0, 65025]. The common shortcuts
  `(x + (x >> 8)) >> 8` and `(x * 257) >> 16` are NOT exact.

## Perf: measure on the target, never trust the host

Host -O2 numbers INVERT on the deployment target. Measured example:
the shift-based /255 is 2x slower than plain division on a native host
(clang lowers constant division to a multiply and vectorizes), yet
2.8x faster under QEMU TCG, where division lowering and vector code
become slow helper sequences. Tune raster hot paths for the slowest
deployed environment, which is TCG.

Run `gfxbench` from the serial shell (userland/apps/gfxbench) for
in-target ns/pixel of the raster hot paths; extend it with a variant
before shipping any new hot loop. Host harnesses remain the right tool
for CORRECTNESS; they are disqualified for performance rankings.

## Byte-exact screenshot comparison

For "output must not change" claims, compare live desktop screenshots
from before and after builds:

1. Build and boot the BEFORE tree first, capture via the monitor
   socket: `screendump /tmp/before.png -f png`, reaching the same GUI
   state each time (dismiss splash, wait for autostart terminal).
2. Apply changes, rebuild, repeat for /tmp/after.png.
3. Compare ignoring known-dynamic regions. The top bar clock and
   network status ALWAYS differ across boots, so crop it off; the
   terminal cursor may differ in blink phase:

```bash
magick /tmp/before.png -crop 1920x1046+0+34 +repage /tmp/b.png
magick /tmp/after.png  -crop 1920x1046+0+34 +repage /tmp/a.png
magick compare -metric AE /tmp/b.png /tmp/a.png /tmp/diff.png
```

AE prints the count of differing pixels on stderr; expect 0, and
inspect /tmp/diff.png (differences render red) before accepting any
nonzero. For "changes by at most N bits" claims, add `-metric PAE`:
a peak of 257 (0.0039) means exactly one 8-bit LSB.

## Pitfalls specific to graphics changes

- Apps do NOT relink when only a lib archive changed. After lib-only
  edits, `make clean` the userland or touch one source file per app,
  and verify with `strings initrd/bin/<app> | grep <marker>`.
- 24bpp paths exist in libstlxgfx but no current surface is 24bpp;
  exercise them in the host harness, not in QEMU.
- The framebuffer mapping is write-combining: reads from it are
  pathologically slow. Never benchmark by reading /dev/gfxfb content
  back, and never add code that reads the scanout mapping.
- SIMD autovectorization differs between clang x86_64 and aarch64.
  A loop restructure can be a big win on one arch and neutral on the
  other; always build (and when perf-critical, measure) both.
