# Live camera work plan

Three threads of work on `apps/tinytag_detect/live_camera.cc` and the sensor
path, in the order they are being tackled. Goal throughout: **lowest latency and
highest throughput**, with per-frame cost kept off the detector thread.

Status key: DONE = implemented and building, cross-compiled and copied to the
board, but **not yet validated on hardware** unless stated.

---

## 1. Capture side: always work on the freshest frame  -- DONE, VERIFIED ON HARDWARE

**Problem.** `CVI_VPSS_GetChnFrame` returns the *oldest* queued frame. Once
detection costs more than a frame period the queue backs up and the detector
works on progressively staler images: latency grows while throughput does not.
The earlier saturated run showed this as `wait 0.01` with `busy 34-37 ms`.

**Rejected approach.** Draining the queue from the detector by polling with a
short timeout. It taxes exactly the saturated case it is meant to help, and it
depends on `GetChnFrame`'s behaviour for `s32MilliSec == 0`, which is not
documented and which nothing in this SDK exercises (50 call sites use 2000, 15
use 1000, two use -1). Guessing wrong risks a hang.

**Implemented.** A dedicated capture thread blocks on `GetChnFrame` and writes
into a single-frame "latest value" slot, releasing whatever it displaces:

    capture thread            detector
    ---------------           ------------------------------
    GetChnFrame (blocking)    take_latest_frame()
    if slot full:             blocks only when no frame exists yet
      release stale, ++dropped
    slot = newest

The detector never polls and never drains. It blocks only when the camera has
genuinely produced nothing yet -- the healthy case, visible as `wait` of
several ms. Two VB blocks are held steady-state (one in the slot, one in the
detector) against a pool of five.

Frames superseded before collection are reported per second as `stale`.

**Why this scales the right way.** Staleness is bounded by one capture period,
so a faster camera *reduces* latency instead of building a backlog:

| capture | slot refresh | worst-case age when detection starts |
|---|---|---|
| 30 fps | 33 ms | 33 ms |
| 60 fps | 16.7 ms | 16.7 ms |
| 130 fps | 7.7 ms | 7.7 ms |

**Cost that scales with capture rate, not detection rate:** the capture thread
wakes once per captured frame and performs ~2 ioctls. On a single Cortex-A53
(this build exposes exactly one core) that preempts a CPU-bound detector at the
capture rate. Expected low single-digit percent; `busy` and `stale` will show
it if worse.

**Verified on hardware** (2026-09-17). `u32TimeRef`, the camera's own frame
counter, is reported per second as `seq mean/max/sum`; summing the gaps gives
the capture rate independently of how many frames were processed.

    keeping up   31.2 fps | wait 10.20 | busy 20.83 |  0 stale | seq mean 1.00 max 1 sum 32
    saturated    20.8 fps | wait  0.00 | busy 46.48 | 11 stale | seq mean 1.48 max 2 sum 31

Three checks agree on the saturated run: processed + stale = 20.8 + 11 = 31.8
against `sum 31`; `seq mean` of 1.48 against the predicted 31/20.8 = 1.49; and
the capture rate holding at ~31 regardless of detector load, which shows VPSS
is not dropping internally.

`seq max 2` is the latency bound made visible -- never more than one frame
superseded, so detection always starts on a frame at most one capture period
(~32 ms) old even while each pass takes 46.5 ms. The previous design would have
grown a backlog of ~0.45 frames per frame until the 5-block pool filled: frames
4-5 periods stale, 130-160 ms of latency, with `seq sum` falling below the
capture rate as VPSS began dropping.

Note on crop cost: these two runs happened to give 1.91 and 1.92 ms per ROI,
but that is coincidence, not a rule. Crop-decode cost tracks ROI **area**, not
count -- a closer tag means a bigger ROI and more work. A later run with the
same 8 proposals measured 2.40 ms per ROI. `--max` bounds the number of crops,
never the cost of each.

---

## 2. Preview: exact luma copy, colour overlay, no detector-thread cost

**Requirement.** `--rtsp-luma` exists to show *exactly what feeds the
algorithm*. A second VPSS channel was considered and rejected: both channels
use the same helper and target size so the luma is very likely identical, but
it is a separate scaler output through a different format path and identity
cannot be proven from source. For a debug view that is the wrong trade -- this
session spent considerable time chasing mirroring, sharpness and contrast, and
a preview that is only *probably* the same would have been a liability.

**Implemented (DONE).**

- Two NV21 preview surfaces allocated once at init via `CVI_SYS_IonAlloc`.
  Nothing is allocated in the frame loop.
- The detector does a byte-exact row-wise copy of the luma plane into the
  current surface, then releases the VPSS frame immediately.
- The preview worker owns everything else: chroma reset, drawing, cache flush,
  encode, RTSP. None of it touches the detector thread.
- Scene stays monochrome (luma is the exact copy, chroma starts neutral
  128/128); the overlay writes real chroma where it draws, so boxes and text
  render in colour over a grey image.
- Chroma is reset only on the rectangles drawn last time (`PreviewSurface::
  dirty`), not the whole plane. Luma needs no reset -- the copy overwrites it.
- Ping-pong means the worker can still be drawing frame N while the detector
  copies N+1.
- The queue drops rather than blocks: if the encoder falls behind, the preview
  frame is discarded instead of stalling detection.

**Remaining: hardware copy (NOT STARTED).** The copy is currently
`std::memcpy` -- a synchronous CPU copy of ~0.9 MB, so there is nothing to
overlap and nothing to check for completion. The SDK exposes an IVE DMA engine:

    CVI_S32 CVI_IVE_DMA(IVE_HANDLE, IVE_DATA_S *src, IVE_DST_DATA_S *dst,
                        IVE_DMA_CTRL_S *ctrl, bool instant);   // cvi_ive.h:398
    IVE_DMA_MODE_DIRECT_COPY = 0x0
    IVE_DMA_MODE_SET_8BYTE   = 0x3     // could neutralise chroma

With `instant=false` the transfer is asynchronous, so:

    map frame -> kick DMA into surface[i]
              -> pre_process + inference + decode   (~5-10 ms)
              -> check DMA complete                  (already done by then)
              -> release VPSS frame

The copy and detection both only *read* the source plane, so they can run
concurrently -- the copy hides entirely behind inference and costs the detector
thread nothing. (An earlier claim in this work that there was "nothing to
overlap with" was wrong: the memcpy was placed after detection out of habit,
not necessity.)

Costs to weigh: IVE is not currently initialised in this app, so it adds
`CVI_IVE_CreateHandle` and a dependency on `libcvi_ive`; and the VPSS frame is
then held across inference rather than released early, trading VB pool
occupancy for CPU time.

**Measured on hardware** (2026-09-17) -- and it justifies doing the DMA work:

    --rtsp        release 0.19 ms    copy path skipped, descriptor work only
    --rtsp-luma   release 5.28 ms    includes the ~0.9 MB luma copy

    31.0 fps | wait 0.75 map 0.47 pre 2.23 infer 2.52 decode 1.08
             | crop 19.17 release 5.28 = busy 30.75 | 8.0 proposals | 1 stale

So the copy costs ~5.1 ms: 0.9 MB read plus 0.9 MB written, ~350 MB/s. It is
slow because the source plane has just been cache-invalidated for DMA
coherency, so every read misses to DRAM -- the same cold-cache effect that
makes live crop-decode dearer than the warm still-image benchmark.

At `busy 30.75 ms` against a ~32 ms frame period the pipeline is already at the
edge (1 stale). Removing 5.1 ms brings it to ~25.6 ms. And since
`pre + infer + decode` = 5.83 ms, a DMA kicked before `pre_process` and checked
after `decode` would hide the copy almost exactly.

**Decision: implement the IVE DMA path.**

Note `memset` is already the cheaper primitive for the chroma reset -- it
writes only, where copying a pre-prepared neutral buffer would read *and*
write, moving twice the memory.

---

## 3. Native 1280x720 @ 60 fps from OV5647  -- NOT STARTED, LOW PRIORITY

**Priority: low.** This is the hardest of the three and the only one that
needs sensor-driver work against an NDA datasheet. Tasks 1 and 2 deliver most
of the latency benefit without it. Park this until they are validated on
hardware and their gains are measured.

**Why it is still worth doing eventually.** The detector channel is exactly
1280x720. Capturing natively at that
size would remove the VPSS downscale from 1920x1080 entirely, and 60 fps halves
the worst-case frame age in the table above (33 ms -> 16.7 ms). Both feed
straight into the latency goal.

**What exists today.** The CVI driver supports exactly one mode:

    // cvi_mpi/component/isp/sensor/sg200x/ov_ov5647/ov5647_cmos_ex.h:26
    typedef enum _OV5647_MODE_E {
        OV5647_MODE_1920X1080P30 = 0,
        OV5647_MODE_LINEAR_NUM,
        OV5647_MODE_NUM
    } OV5647_MODE_E;

with a single register table `ov5647_linear_1080p30_init()`
(`ov5647_sensor_ctl.c:238`), and `pub_attr.f32FrameRate = 30` set in
`live_camera.cc`. So this is a **driver change**, not a configuration change.

**Sketch.**

1. Add `OV5647_MODE_1280X720P60` to the mode enum.
2. Add `ov5647_linear_720p60_init()` with the register sequence, and select it
   from `ov5647_init()`.
3. Add the mode entry in `ov5647_cmos_param.h`: 1280x720 window, HTS/VTS for
   60 fps, exposure and gain ranges (max exposure is VTS-bound, so it changes
   with the new VTS).
4. Register the mode name in `build/sensors/sensor_list.json` and point
   `device/generic/rootfs_overlay/duos/mnt/data/sensor_cfg.ini` at it.
5. Raise `pub_attr.f32FrameRate` to 60 in `live_camera.cc`.
6. Drop the VPSS downscale once capture is already 1280x720.

**Open questions -- resolve before coding.**

- **Register sequence.** OV5647 datasheet is under NDA. Mainline Linux
  `drivers/media/i2c/ov5647.c` is GPL and a usable cross-reference, but check
  whether it actually carries a 1280x720 mode -- it may only have 640x480,
  1296x972 and 1920x1080. The Raspberry Pi firmware exposes 1280x720 as its
  "mode 6", derived by binning and cropping the full array.
- **Field of view.** 1280x720 out of a 2592x1944 array is a crop, possibly with
  2x2 binning. Compared with today's 1080p-downscaled-to-720p view the FOV will
  change, and probably narrow. That directly changes tag angular size, so
  re-measure px-per-module afterwards -- the current frames sit at a
  comfortable 7.8-8.7, so there is margin, but it is not unlimited.
- **Binned image quality.** Binning trades resolution for sensitivity and can
  soften edges. The decoder cares about crisp local contrast far more than the
  network does.
- **ISP tuning.** `cvi_sdr_bin_OV5647.bin` already logs a pqbin md5 mismatch
  against this middleware. A new sensor mode may need its own tuning; watch
  sharpness and contrast in `tools/tinytag_cvimodel/diagnose_frame.py`.

---

## Cross-cutting constraints

- **Do not change `roi_expand` (1.5) or the operating point** to work around
  problems elsewhere; those values are experimentally tuned, and crop-decode
  cost scales with ROI area on the stage that already dominates the budget.
- **One CPU core.** The arm64 build's device tree declares a single
  Cortex-A53; every added thread costs context switches against a CPU-bound
  detector.
- **Build in Docker only** -- see the top-level README. Mixing host and
  container builds leaves root-owned files that break later builds.
- New runtime behaviour should be switchable from `run_live.sh` via a
  `TINYTAG_LIVE_*` variable where it makes sense to compare on hardware,
  without a rebuild.
