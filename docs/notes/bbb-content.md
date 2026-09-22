# `--content=bbb` — Big Buck Bunny as a known-quantity control source

## Why

Every quality/throughput number this harness produced before this was on
synthetic ffmpeg `lavfi` sources (`testsrc`/`testsrc2`) — deliberately, for
byte-exactness (`CLAUDE.md`'s rule that byte-exactness is only trustworthy
on `testsrc`), but that leaves every *quality* number (PSNR/SSIM, RD
comparisons) measured on content nothing else in codec research uses.
Standard practice in that field is the opposite: known, standardized test
clips, not synthetic patterns — Xiph's [derf collection](https://media.xiph.org/video/derf/)
(the reference corpus behind VP9/AV1 development) and the AV1/JVET Common
Test Conditions both work this way, and Big Buck Bunny is itself part of
the derf collection.

BBB is also, incidentally, a *better* stand-in for game content than most
"real video" test clips would be: it's CG-rendered, not camera-captured,
so it shares the GPU-rendered pedigree game content has (no film grain,
sharp synthetic edges) — while still not being actual gameplay (no HUD, no
player-driven camera, no game-typical UI elements). Documented here
plainly as "a known-quantity control, better than synthetic patterns,
explicitly not game-representative," not oversold as either.

## What was actually fetched

- **File**: `BigBuckBunny4k60fps.mp4`
- **Source**: `https://archive.org/download/big-buck-bunny-4k-60fps/BigBuckBunny4k60fps.mp4`
  (Internet Archive item [`big-buck-bunny-4k-60fps`](https://archive.org/details/big-buck-bunny-4k-60fps),
  hosting the official Blender Foundation release)
- **Resolution/framerate**: 3840x2160, 60fps (confirmed via `ffprobe`, not
  assumed from the filename)
- **Duration**: 634.5s (~10.6 minutes)
- **Size**: 642.0 MB
- **sha256**: `35db9a007021f1b0066993e1d2c4448c83a8b279f799c97d33cbba73980a8a36`
- **License**: Creative Commons Attribution 3.0. "(c) copyright 2008,
  Blender Foundation / www.bigbuckbunny.org" — freely usable including for
  this purpose, attribution preserved here.
- **Fetched to**: `$LAB/content/BigBuckBunny4k60fps.mp4` on the board
  (`/var/home/user/bc250-lab/content/`), 2026-09-22.

Note this is the ORIGINAL 2008 render's later 4K60fps remaster, not the
1080p24 original — the download.blender.org mirror only carries the
original up to 1080p; the 4K60 file lives on Internet Archive under the
official Blender Foundation upload above.

## How the harness uses it

`tools/bc250_lab.sh`'s `input_args()` (shared by `run_encode()`, `drift()`,
and both `scoreboard`'s and `qsweep()`'s reference-generation steps) checks
for `content=bbb` and, on first use at a given test resolution, prepares
and caches a scaled/trimmed copy at `$LAB/content/bbb_<res>.mp4`:

```
ffmpeg -y -v error -ss 30 -i BigBuckBunny4k60fps.mp4 -frames:v 3600 \
    -vf "scale=<W>:<H>" -an -sn -f mp4 bbb_<res>.mp4
```

- `-ss 30`: skips the opening Blender Foundation card, lands in the film.
- `-frames:v 3600`: caps the cached clip at 60s (3600 frames @ 60fps) —
  comfortably more than any `--frames` value this harness currently uses
  (max observed: 300). Widen if a future test needs a longer clip.
- Every per-resolution cache is derived from the **original, untouched**
  4K60 source every time it's (re)built — never from a previously-derived
  cache — the same "always regenerate the true source, never trust a
  cached intermediate" discipline the `lavfi` sources already get for
  free. Cached only because re-decoding+scaling a 642 MB 4K60 source on
  every single test run would be wasteful, not because the cache is
  trusted as ground truth instead of the original.
- No audio/subtitle streams kept (`-an -sn`) — irrelevant to encoder
  benchmarking and just extra decode/mux cost.

This means `--content=bbb` is a drop-in alongside `--content=testsrc2` on
every command that already accepts `--content=`: `qsweep`, `scoreboard`,
`drift`, `bench`/`compare`/`noise` (via `run_encode()`).

## What this is NOT

- **Not a byte-exactness oracle.** `CLAUDE.md`'s byte-exactness rule
  (`testsrc`, all-intra only) is untouched by this; BBB is for
  quality/throughput numbers, not correctness gating.
- **Not game footage.** See "Why" above — a real captured-gameplay
  dataset (e.g. academic sets like GameScope or CGVDS) would be the
  further step if a genuinely game-representative content claim is ever
  needed; that is real, separate, heavier work (licensing, hosting) not
  taken on here.
