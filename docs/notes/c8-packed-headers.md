# C8: HEVC odd-frame-size rounding — what a fix would require, and why it wasn't attempted unilaterally

Status: **investigated, designed, NOT implemented.** This is a real design
decision with a real risk to a documented use case, not a bug fix — it
needs sign-off, not code, from this pass.

## The symptom, restated

`docs/backlog.md` C8: ffmpeg rounds HEVC dimensions up to a multiple of 8
*before* calling `vaCreateContext()`. `VAEncSequenceParameterBufferHEVC`
carries no conformance-window fields (unlike its H.264 counterpart), so this
driver's `write_sps()` never receives the true requested size at all — only
the rounded one. 854x480 arrives, decodes, and is reported by `lab dims` as
856x480. Not a bug in this driver's own SPS-writing logic (which is correct
given the information it has); the true size is architecturally unavailable
through the normal surface/context creation path.

## Where the true size actually is, and the only way to reach it

Read `va_backend.c`'s `bc250_GetConfigAttributes()` (the
`VAConfigAttribEncPackedHeaders` case) and `bc250_RenderPicture()`'s handling
of `VAEncPackedHeaderDataBufferType`. Both packed-header buffer types are
explicitly, deliberately ignored today — this driver always writes its own
SPS/PPS/slice headers and tells libva it supports `VA_ENC_PACKED_HEADER_NONE`
so no caller is misled into thinking otherwise.

ffmpeg's own HEVC VA-API encoder path (`vaapi_encode_h265.c`) builds a real
SPS *of its own*, including a correct `conformance_window_flag` and offsets
computed from `avctx->width`/`avctx->height` (the true, user-requested size)
against the CTU-aligned surface size it actually allocated — this is
standard, well-established ffmpeg behaviour, not something specific to this
driver. If packed headers were advertised as present, ffmpeg would hand that
SPS to `bc250_RenderPicture()` via `VAEncPackedHeaderDataBufferType`
(sequence type). **That is the only channel through which the true requested
size could ever reach this driver** — there is no other VA-API surface that
carries it once `vaCreateContext()` has already been called with the rounded
one.

## The narrow fix, and why it should stay narrow

The temptation is to advertise `VA_ENC_PACKED_HEADER_SEQUENCE`, accept
ffmpeg's packed SPS, and splice its bytes into the bitstream directly. **Do
not do that** — this driver's own SPS/PPS carry real, hard-won correctness
work this session alone (Table 8-10 chroma-QP-consistent scan/quant
selection, C9's deblocking-off signalling, `sign_data_hiding`/
`transform_skip`/`cu_qp_delta` all disabled to keep `transform_unit()`
minimal). Splicing in ffmpeg's own SPS/PPS bytes verbatim would silently
readopt whatever PPS flags ffmpeg's generic HEVC encoder profile defaults
to, which this driver's CABAC/transform/quant code does not necessarily
implement — a correctness regression far worse than a cosmetic rounding
report.

The narrow, correct-shaped fix: accept only the **sequence** packed header,
parse *just* `pic_width_in_luma_samples`, `pic_height_in_luma_samples`,
`conformance_window_flag` and (if present) the four `conf_win_*_offset`
fields out of ffmpeg's SPS RBSP (a small, bounded parse — H.265 7.3.2.2's
SPS syntax up through those fields, escape-removed first per 7.3.1.1), and
feed the recovered TRUE display width/height into this driver's own
`write_sps()` so it computes its own, correct conformance window from real
data instead of the rounded surface size. Every other byte of the bitstream
stays exactly as this driver already writes it. `write_pps()`/slice headers
are untouched entirely — packed picture/slice headers would stay unadvertised
and unaccepted, exactly as today.

## The real risk, and why this needs a decision, not a commit

`VAConfigAttribEncPackedHeaders`'s existing comment (read before writing
this note, not assumed) already states the mechanism precisely: **ffmpeg
only builds `AVCodecContext.extradata` from its own self-authored SPS/PPS
when `VA_ENC_PACKED_HEADER_SEQUENCE` is reported present.** Advertising it
— even if this driver never uses ffmpeg's bytes for anything but recovering
two numbers — changes ffmpeg's own behaviour for any container-muxed output.

Concretely: `ffmpeg -c:v hevc_vaapi ... output.mp4` (a real, documented
command in this project's own README) would end up with an `.mp4` whose
`hvcC`/extradata box carries **ffmpeg's own SPS bytes**, while the in-band
bitstream inside every sample carries **this driver's SPS bytes** — narrowly
fixed to agree on width/height/conformance-window, but not necessarily
byte-identical or semantically identical everywhere else (profile-tier-level
fields, VUI, etc.). Most decoders reparse in-band parameter sets and treat
extradata as a redundant hint, so this is unlikely to break real playback —
but it is a real, if narrow, spec-conformance smell in a use case this
project actively documents and supports (recording, not just live
streaming), and "unlikely to break real playback" is exactly the kind of
claim this project's own history says needs measuring, not asserting.

**This does not affect the RTP/Sunshine→Moonlight streaming path at all** —
that path has no container and no extradata; the risk is scoped entirely to
file-based / muxed output.

## What this pass did NOT do, and why

- **Did not touch `va_backend.c`.** Changing `VAConfigAttribEncPackedHeaders`
  is a real behavioural change to what this driver advertises to every VA-API
  caller, not a self-contained bug fix — exactly the kind of cross-cutting,
  irreversible-feeling decision this project's own conventions (and this
  session's handling of A6's throughput-vs-gate tradeoff) treat as needing
  explicit sign-off rather than being decided unilaterally by whichever pass
  happens to be looking at the backlog.
- **Did not implement the SPS RBSP parse.** It's a small, bounded, mechanical
  piece of work once the decision above is made — parsing a handful of
  leading fields out of an RBSP this driver doesn't control the shape of
  (ffmpeg's, not its own) is exactly the kind of "trust external bytes"
  operation this project's `CLAUDE.md` treats with real caution (see the
  `VaApiDriverTest`/surface-lifecycle discipline elsewhere in this file).

## If this gets a go-ahead

1. Advertise `VA_ENC_PACKED_HEADER_SEQUENCE` for **HEVC only** (`profile ==
   VAProfileHEVCMain`) — leave H.264 exactly as it is; H.264 already reports
   its true size correctly via `VAEncSequenceParameterBufferH264`'s own crop
   fields (`b2b7fef`), so nothing there needs this.
2. In `bc250_RenderPicture()`'s `VAEncPackedHeaderDataBufferType` case, when
   the preceding `VAEncPackedHeaderParameterBufferType` said `type ==
   VAEncPackedHeaderSequence` and the active profile is HEVC: RBSP-unescape,
   walk the SPS syntax to `conformance_window_flag`, extract the four offsets
   if present (else 0), and stash the recovered display width/height on
   `bc250_context` for `write_sps()` to use in place of `context->picture_
   width/height`.
3. Extend `tools/hevc_host_drift.sh`'s odd-size cases (854x480, 1366x768,
   1918x1078 — currently marked LIMIT by `lab dims`, not exercised for
   correctness at all) into real board `lab dims`/`lab drift` cases once the
   parse exists, since this is exactly the kind of "trust an external byte
   stream" change this project's own history says needs an independent
   oracle, not a self-consistency check.
4. A real board test of `ffmpeg -c:v hevc_vaapi ... -f mp4 output.mp4`
   followed by re-probing the file's extradata vs. its first in-band SPS
   (`ffprobe`/`mediainfo`) to see empirically whether the mismatch this note
   predicts is cosmetic or actually confuses a real decoder/player — measure
   it, don't assume it away.
