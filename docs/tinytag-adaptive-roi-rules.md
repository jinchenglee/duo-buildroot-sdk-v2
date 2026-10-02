# TinyTag adaptive ROI decode rules

This is the decision record for the `--adaptive-decode 1` path.
It is **on by default**, with `--adaptive-min-roi-area 10000` (100×100 area),
as requested on 2026-10-02. `--adaptive-decode 0` sends accepted ROIs to the
full-resolution crop decoder. Both modes use the same 640×360 proposal network
and 1280×720 camera frame. The optional adaptive path can avoid a
**full-resolution decode** for a large ROI by trying its half-size crop first
and deferring some full-resolution scans. It does not avoid the network pass or
all crop decoding.

## Sharing overlapping execution crops

`--merge-crops 1` is enabled by default and is independent of whether adaptive
decoding is enabled. After history matching, alignment and resolution selection,
the execution planner combines strongly overlapping crops of the **same
resolution policy**. It requires overlap of at least 50% of the smaller crop
and a bounding union at least 10% cheaper in planned pixels than separate scans.
These are initial heuristics, not empirically tuned latency thresholds.
Repeated merges must continue to meet the area-saving condition. No original
crop coverage is discarded; separate blob IDs and blue debug boxes remain.

One shared scan can return several tags. Results retain an original member ROI
and are associated by tag coverage, previous ownership and proposal confidence.
Each member's audit clock is updated separately; known-tag checks inspect all
group members. Full/audit crops are not merged with half-size crops. A missing
recently tracked tag (at most one missed frame) after a merged full scan triggers
an original-crop recovery scan. This avoids an unconditional second scan; it
does not prove unchanged recall for new tags because crop-border thresholding
can change. Existing full-resolution corner refinement and point LDC remain.

`[crop-merge]` reports planned input/output scan counts and native pixel areas
plus extra recovery scans/pixels. Native areas precede half-size scaling and
exclude recovery work; use actual decoder pixels and timings for net savings.
`--merge-crops 0` restores independent execution crops for comparison. Pink
debug boxes represent executed windows; blue boxes remain individual tracks.

Validation so far: deterministic overlap/containment/area-inflation cases and
10,000 randomized crop-coverage/policy cases passed; still-image and live ARM64
binaries cross-built. Board was removed, so no hardware throughput, latency
or tag-recall improvement is claimed. Future tests should include nearby cube
faces, partial occlusion, entering/new tags, small tags and audit transitions.

## Why route by history

A large proposal contains many pixels, so full-resolution contour extraction
can dominate frame time. Its area does **not** prove that every tag inside is
large: it may contain background or several tags of different sizes. The
network proposal confidence is a mask score, not a calibrated estimate of tag
size or a guarantee that a tag exists. Use measured tag corners and track
history when available. A proposal with no history remains uncertain.

## Runtime rules

1. Keep the full-resolution decoder for small ROIs and for a tracked tag whose
   shortest observed side is below 64 full-resolution pixels.
2. For an eligible large ROI (at least `--adaptive-min-roi-area N`
   full-resolution pixels, default 10,000), try
   ArUco on the corresponding 640×360 crop first. This includes **large new
   blobs with no decoded tag**. An empty low-resolution result does not
   immediately trigger an expensive full-resolution decode.
3. For a new empty blob, require persistence before a full-resolution check:
   start after 2 matched frames for mask score ≥0.75, or 4 otherwise. Once
   that check is empty, back off the next check to 4, 8, 16, then at most 32
   frames. Matching neural proposals keep the blob alive even when no tag
   decodes. A disappearing proposal retires on the normal track timer.
4. If a low-resolution pass misses a recently known tag, run the full decoder
   on that ROI in the **same frame**. A low-resolution success cannot establish
   that smaller tags are absent, so run a full-resolution audit at least every
   20 frames after a hit. A full pass that finds tags resets the audit period.
5. For a low-resolution success, scale the four corners to the full frame and
   refine them against the full-resolution luma. Point LDC refinement also
   runs there. A refinement failure falls back to full-resolution decoding.

The deferral deliberately allows a new small tag inside a large empty blob to
be discovered later, at its next full-resolution audit. The initial delay is
bounded by the 2–4 frame persistence rule while the proposal remains matched;
subsequent empty audits can extend it to 32 frames. This is the accuracy and
latency tradeoff to evaluate before making the feature the default.

## Choosing the ROI-size gate

The current 10,000-full-resolution-pixel **area** gate is a convenient starting
value, equivalent to a 100×100-pixel box but also met by other shapes such as
200×50. It is not a calibrated optimum. Careful tests of recall, first-detection
delay, corner quality, and processing time must determine the real threshold.
Lowering it makes more ROIs eligible, including new blobs whose tags may be
too small to decode at half size. The 64-pixel known-tag rule protects tracked
small tags, but cannot protect a tag that has not been found yet. A tag found
only on a later full-resolution audit counts as a delayed first detection,
even if aggregate tag counts look unchanged. Low-resolution corners can also
be too inaccurate to seed full-resolution refinement reliably.

There is a speed floor as well: for small ROIs, the full-resolution decoder
may already be cheaper than copying the 640×360 frame and running a second
decoder. Known-tag fallbacks and periodic audits add to that cost. Therefore
the smallest gate is not automatically the fastest gate.

Before changing the gate, log individual ROI areas, per-frame decoded IDs,
time to first detection, corner differences, and decode time. Sweep several
gates on the same lockstep replay, including 20k, 40k, and 80k pixels. Choose
the smallest gate that preserves per-frame recall and corner quality **and**
improves mean and tail processing time. Do not infer recall from per-second
marker averages alone. The runtime option `--adaptive-min-roi-area N` accepts
a positive integer area in full-resolution pixels. It takes effect only with
`--adaptive-decode 1`; it does not enable adaptive decoding by itself.
For equivalent square sizes, 100×100 = 10000, 150×150 = 22500, 200×200 = 40000,
and 250×250 = 62500. Other rectangle shapes of the same area also qualify.
The chosen gate is reported in the adaptive startup diagnostic.

For example, compare a smaller gate live against the default on the same scene:

```sh
/app/tinytag_detect/run_live.sh --adaptive-decode 1 --adaptive-min-roi-area 22500
/app/tinytag_detect/run_live.sh --adaptive-decode 1 --adaptive-min-roi-area 40000
```

Omit `--quiet` to inspect adaptive timing/counters. Use `--tag-output 1` when
logging decoded IDs for recall comparison, accounting for stdout overhead.

## A/B decision

Run the same scene and camera settings with `--adaptive-decode 0` and `1`, in
alternating order. Compare detector fps, `loop`, `crop`, result age, and the
tail percentiles, as well as decoded tag IDs and corner positions. With mode 1,
record `[adaptive]` low attempts/hits, same-frame fallbacks, full audits,
deferred passes, and low/full pixels and time. Include scenes with empty large
blobs, a large tag, a small tag inside a large blob, multiple tags sharing a
blob, and tags entering or leaving. Favor recall and corner quality over a
small mean-time gain. The current default is enabled by user choice; this
does not establish a measured recall guarantee for the 10,000-pixel gate.

On the board, `/app/tinytag_detect/run_adaptive_bench.sh` performs the
alternating runs and writes `windows.tsv` plus full logs under `/tmp`.

### First board run (2026-09-29)

The first two-round, 20-second live comparison produced **zero low-resolution
attempts in both adaptive rounds**; `[adaptive]` also reported zero small-frame
copy time. The feature was enabled, but no proposal crossed the 40,000-pixel
ROI gate in that scene. The raw logs show about 7–7.5 proposals and 53–56
thousand total crop pixels per frame, or roughly 7–8 thousand pixels per
proposal on average. The individual ROI-size distribution was not logged.

| Round | Mode | fps | crop ms/frame | markers/frame | Low attempts |
|---|---|---:|---:|---:|---:|
| 1 | baseline | 52.08 | 12.89 | 1.792 | 0 |
| 1 | adaptive | 52.76 | 13.45 | 1.934 | 0 |
| 2 | adaptive | 32.64 | 21.71 | 1.837 | 0 |
| 2 | baseline | 44.89 | 16.53 | 1.249 | 0 |

These numbers do not measure the adaptive route or justify changing its
default. Marker workload and crop time varied between runs. The next test
needs a scene with a persistent ROI above 40,000 pixels, preferably including
an empty blob and a small tag inside a large blob. Log per-ROI areas before
considering a lower gate; the mean area alone is insufficient to choose one.

### Large-ROI replay

The supplied `~/large_rois.mp4` is 1280×720, H.264, and 27.56 seconds long.
Frames from 10 seconds onward were re-encoded at high quality without B-frames
for exact-start, lockstep hardware replay. The resulting 279-frame clip is on
the Duo S at `/tmp/tinytag-large-rois-from-10s.mp4`. The original recording
was read only. On the board, run:

```sh
TINYTAG_BENCH_INPUT=/tmp/tinytag-large-rois-from-10s.mp4 \
  /app/tinytag_detect/run_adaptive_bench.sh
```

The runner alternates the baseline and adaptive modes twice, feeds each frame
only after the previous one was taken (`--input-speed 0`), and records the
`processed_frames` and `source_frames` counts. Compare results only when the
counts match across runs and inspect `[adaptive] low attempts` to confirm that
the large-ROI route actually ran. The logs include per-second tag IDs, but
the app does not yet report IDs for each individual replay frame. Subsequent
runs of the updated script have a 120-second timeout per replay; set
`TINYTAG_BENCH_REPLAY_TIMEOUT` to change it. A run already in progress when
the script was updated does not gain that timeout.

The first replay run processed 278/279 frames in every mode, but advanced at
only 1 fps because VDEC held one picture until the next access unit arrived
while the feeder waited for that picture. The feeder now keeps one packet
ahead. The original 1-fps data is useful for decode-work comparison, but its
fps is **not** a detector-throughput measurement:

| Mode | Crop ms/frame, two runs | Loop ms/frame, two runs | Low attempts | Low hits | Deferred full | Full audits |
|---|---:|---:|---:|---:|---:|---:|
| Baseline | 14.65, 14.81 | 17.66, 17.83 | 0 | 0 | 0 | 0 |
| Adaptive | 7.67, 7.71 | 10.82, 10.89 | 133 | 2 | 129 | 22 |

The observed tag IDs were 14 and 25 in every run, and per-second marker means
were about 1.31–1.32. These aggregates cannot establish per-frame recall;
the application currently has no per-frame tag-result export. The adaptive
route mostly deferred empty large blobs. A full-rate replay and a per-frame
tag comparison are needed before enabling this by default.

## Heat and mask threshold controls (2026-10-02)

`--thres_heat` (default 0.30) replaces the removed `--thres` option.
`--thres_mask` (default 0, disabled) optionally rejects current six-channel
proposals whose reported maximum assigned mask score is below the gate, before
`--max`. Fixed mask seed/grow thresholds and mask-only fallback generation stay
in place. History is not filtered by the current-frame mask gate; unmatched
known tracks may receive full-resolution recovery even when their neural ROI
was filtered. This can offset the saving from fewer new proposals. Measure
new-tag recall and first-detection delay as well as total scanned pixels and
crop latency. The default disabled mask gate preserves prior proposal behavior.
