# Live camera work plan

Ordered work on `apps/tinytag_detect/live_camera.cc`, the detector, and the
sensor path. Goal throughout: **lowest end-to-end latency and highest sustained
throughput**, with stale work discarded early and avoidable per-frame work kept
off the detector thread. Correctness and trustworthy measurement come before
optimization.

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

## 2. Preview baseline: exact luma and colour overlay -- SUPERSEDED BY ITEMS 3-4

**Requirement.** `--rtsp-luma` exists to show *exactly what feeds the
algorithm*. A second VPSS channel was considered and rejected: both channels
use the same helper and target size so the luma is very likely identical, but
it is a separate scaler output through a different format path and identity
cannot be proven from source. For a debug view that is the wrong trade -- this
session spent considerable time chasing mirroring, sharpness and contrast, and
a preview that is only *probably* the same would have been a liability.

**Implemented baseline.**

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
- Two surfaces were intended to let the worker draw frame N while the detector
  copies N+1.
- The queue was intended to drop rather than block if the encoder fell behind.

The review after the first DMA experiment found that those last two properties
are not actually guaranteed. There are two surfaces but four FIFO entries, no
surface ownership state, and blind round-robin reuse. A surface can therefore
be overwritten while the worker draws or VENC consumes it. The FIFO also keeps
old preview frames and drops a new one when full, which is the opposite of the
lowest-latency policy. Treat preview results and timings under backlog as
untrusted until item 3 is complete.

**CPU-copy measurement on hardware** (2026-09-17):

    --rtsp        release 0.19 ms    copy path skipped, descriptor work only
    --rtsp-luma   release 5.28 ms    includes the ~0.9 MB luma copy

    31.0 fps | wait 0.75 map 0.47 pre 2.23 infer 2.52 decode 1.08
             | crop 19.17 release 5.28 = busy 30.75 | 8.0 proposals | 1 stale

So the copy costs ~5.1 ms: 0.9 MB read plus 0.9 MB written, ~350 MB/s. It is
slow because the source plane has just been cache-invalidated for DMA
coherency, so every read misses to DRAM -- the same cold-cache effect that
makes live crop-decode dearer than the warm still-image benchmark.

At `busy 30.75 ms` against a ~32 ms frame period the pipeline is already at the
edge (1 stale). Eliminating the copy would bring it to ~25.6 ms.

### IVE DMA result: hardware is faster; the first comparison was invalid

The first measurement appeared to reject IVE:

    CPU memcpy   31.3 fps | wait 11.00 | crop 10.02 | release  5.26 | busy 20.30
    IVE DMA      30.7 fps | wait  0.30 | crop 14.64 | release 10.95 | busy 31.42

That conclusion was wrong. `release` was not an isolated copy timer: it also
included destination cache invalidation, unmap, VPSS release, and queue/vector
work. More importantly, the preview surfaces are allocated with
`CVI_SYS_IonAlloc`, which maps them **uncached**, yet the DMA path invalidated
the entire 0.9 MB destination after every copy. The CPU path did not. The test
therefore compared different work.

Hardware profiling on the same board resolves the engine question:

    31.2 fps | release 8.56 ms | busy 18.29 ms | 0 stale

    /proc/ive/hw_profiling:
    DMA ioctl 3165 us | hardware-valid interval 3087 us

The IVE engine copies the plane in about **3.09 ms**, with only ~0.08 ms of
ioctl/driver overhead. It is about 1.7x faster than the measured 5.26 ms CPU
copy. The remaining ~5.4 ms in `release` is surrounding application work, led
by the invalid cache operation; it is not DMA bandwidth.

**No overlap is exposed by this API.** `CVI_IVE_DMA` is a blocking ioctl:

    CVI_S32 CVI_IVE_DMA(..., CVI_BOOL bInstant)
    { ... ioctl(p->devfd, CVI_IVE_IOC_DMA, &ive_arg); return 0; }

The official [CV180x/CV181x IVE API reference](https://doc.sophgo.com/cvitek-develop-docs/master/docs_latest_release/CV180x_CV181x/en/01.software/MPI/IVE_API_Reference/build/html/3_API_Reference.html#dma)
defines `bInstant=true` as busy-waiting and `false` as interrupt mode. The local
driver implements those as polling and a completion wait respectively; both
return only after completion. The local header's
"Dummy variable" comment is wrong, and `CVI_IVE_QUERY` cannot create overlap
after an ordinary DMA call has already waited.

The wrapper has two further correctness problems to address in item 4:
`CVI_IVE_CreateHandle` returns a non-null global handle even when device open
fails, and `CVI_IVE_DMA` discards the ioctl result and returns zero. The current
application always reports success and does not perform the pixel validation
its comment claims.

---

## 3. Fix preview ownership and stale-frame policy -- DONE, VERIFIED ON HARDWARE

Correctness comes first because the current race invalidates later benchmarks.

1. Give every preview surface explicit `FREE`, `QUEUED`, and `IN_WORKER`
   ownership. The detector may only write a `FREE` surface.
2. Replace the four-frame FIFO with a one-frame latest-value slot. When a newer
   preview supersedes a queued one, return the old surface immediately. Never
   replace a surface already owned by the worker.
3. If no surface is free, drop the new preview without blocking detection.
   Count and report preview drops separately from capture `stale` frames.
4. Remove the unused `not_full` condition and avoid per-frame vector allocation;
   proposal, tag, and crop containers have small known caps and can be reused.
5. Add a sequence number to each item and log overwrite/encode gaps so a stress
   run proves displayed frames remain fresh and surfaces are never reused
   concurrently.
6. Test by deliberately slowing the worker beyond one frame period. Required
   result: detector fps is unchanged, preview sequence gaps grow, queue age
   remains at one pending frame, and no tearing or stale overlay appears.

Implemented as a one-item latest-value slot with explicit `FREE`, `FILLING`,
`QUEUED`, and `IN_WORKER` surface states. A detector publication may reclaim a
still-queued surface but can never touch the worker-owned surface. Proposal,
tag, crop, and dirty-region vectors reserve their bounded capacity before the
frame loop. The per-second `[preview]` line reports published/dequeued/encoded
counts, encode failures, replacements, no-surface drops, displayed sequence
gaps, queue age, pending depth, and persistent ownership errors.

The Docker cross-build passes. Hardware validation used normal luma preview and
a deliberately backlogged worker:

    /app/tinytag_detect/run_live.sh --rtsp-luma
    TINYTAG_LIVE_PREVIEW_DELAY_MS=100 /app/tinytag_detect/run_live.sh --rtsp-luma

Normal operation delivered all 32 published frames in the one-second window,
with no replacement, failure, or ownership error. Mean queue age was 0.50 ms.
With a 100 ms injected worker delay, the two observed windows maintained
31.0-31.4 detector fps and satisfied the latest-value accounting exactly:

    published 32 = dequeued 9 + replaced 23; pending 1; ownership-errors 0
    published 32 = dequeued 8 + replaced 24; pending 1; ownership-errors 0

Displayed sequence gaps rose to four as expected, while maximum queue age
stayed within roughly one 32 ms frame period (32.87 and 27.31 ms) instead of
accumulating. No capture frames became stale. The RTSP client disconnected
during the stress run without affecting detector throughput or ownership.

---

## 4. Choose exact-luma transport and preserve DMA knowledge -- DONE, VERIFIED ON HARDWARE

### 4a. Zero-copy borrowed Y plane -- DONE, VERIFIED ON HARDWARE

This is the first transport experiment because it is simpler than DMA and
removes the work instead of accelerating it. It affects only `--rtsp-luma`:

    detector finishes with original VPSS YUV400 frame
        -> queue that Y physical plane plus a neutral chroma plane
        -> worker draws overlay and sends the composite NV21 descriptor to VENC
        -> worker releases the original VPSS frame

The current code already proves VENC accepts separately allocated Y and chroma
physical planes. Use the ownership/latest-value machinery from item 3. On
replacement or queue failure, immediately release the superseded VPSS frame.
Track the number of VB blocks simultaneously held; with five detector-channel
blocks, encoder or RTSP blocking must never deadlock capture.

The detector transfers its existing cached Y mapping and original VPSS frame to
the latest-value item. The worker draws into that exact plane, flushes it,
combines it with a neutral chroma-only surface for VENC, then unmaps and releases
the VPSS frame. Superseding a pending item performs the same unmap/release before
surface reuse.

Hardware validation kept 31.2-31.3 detector fps, zero capture staleness, zero
encode/ownership/no-surface failures, and correct preview output. Normal mode
encoded all 32 published frames and borrowed at most two VPSS frames. With a
100 ms worker delay it satisfied `32 published = 9 dequeued + 23 replaced`,
held at most two borrowed frames, and did not exhaust the five-block pool.

The stressed run exposed the actual detector-side handoff as 0.16 ms. Normal
mode reported 5.83 ms in the broad `release` bucket because notifying the worker
lets its overlay work preempt the detector on the single Linux CPU; this is a
measurement/scheduling issue, not a remaining image copy. Item 7 must split CPU
work from wall-clock preemption before using that bucket for comparisons.

### 4b. Retire copied preview transports -- DONE

Borrowed Y is now unconditional for `--rtsp-luma`. Removed completely from the
live camera are the validation switch, CPU copy, IVE DMA path and options,
private Y surfaces, unnecessary Y cache operations, IVE handle, and static IVE
link. Only the two small neutral chroma/overlay surfaces remain. Removing the
static archive reduced `tinytag_detect_live` from 1,181,128 to 410,920 bytes.

The final zero-copy-only cleanup build passed its hardware smoke test at 31.3
fps: all 32 frames were published/dequeued/encoded, queue age was 0.05 ms mean
and 0.07 ms maximum, capture `stale` and every preview failure/drop/error counter
were zero, and borrowed-frame pressure remained at one current/two maximum.
The broad `release` bucket read 6.35 ms because the worker consumed the item
almost immediately and preempted the detector while that wall-clock timer was
open; the 0.05 ms queue age confirms this is scheduling attribution, not a
remaining copy.

IVE DMA measurements remain useful platform data, but DMA no longer belongs in
a preview path that does not copy. Continue it only as the standalone benchmark
below and as a possible future AMP/offload transport.

### 4c. Record a reusable standalone DMA baseline -- DONE, VERIFIED ON HARDWARE

`tinytag_ive_dma_bench` is now a separate dynamically linked executable; IVE
remains absent from `tinytag_detect_live`. `run_dma_bench.sh` sweeps representative
sizes with cached/uncached ION and both completion modes, then prints the last
kernel profiling record. Each case reports p50/p95/p99/max wall time, median
thread CPU time, effective payload bandwidth, cache clean/invalidate time, and
byte/padding/guard validation. The benchmark launcher accepts
`TINYTAG_DMA_ITERATIONS` and `TINYTAG_DMA_WARMUP` for repeatable runs.

Source inspection resolves the misleading API documentation around
`bInstant`: `cvi_ive.h` calls it a dummy variable, but the CV181x kernel driver
passes it into `cvi_ive_go()`. Value 1 polls the frame-done register with 10 us
delays; value 0 enables the engine interrupt and sleeps on `frame_done` with a
timeout. Both calls are still synchronous from the application's perspective;
the flag changes how the kernel waits, not whether the userspace call returns
before completion. The benchmark therefore compares `instant=0` IRQ wait with
`instant=1` kernel polling and uses thread CPU time to expose their CPU cost.

Run on the board while the live camera is stopped:

```sh
/app/tinytag_detect/run_dma_bench.sh
```

After this isolated baseline, repeat it concurrently with the live pipeline to
measure contention; do not use the concurrent run as the raw-engine baseline.

The isolated 100-iteration hardware sweep passed byte, padding, and guard
validation in all 22 cases. At 1280x720, uncached DMA sustained 291-292 MB/s:
IRQ wait was 3165 us p50 with only 86.5 us of caller CPU, while polling was
3156 us p50 and consumed 3152 us of caller CPU. Cached mappings did not change
engine time; the deliberately separate one-time maintenance measurements were
about 216 us to clean both 0.9 MB buffers and 104-107 us to invalidate the
destination. The padded 1272-wide/1280-stride case also validated at 290-291
MB/s. This confirms the original 3087 us hardware/3165 us ioctl observation.

Transfer scaling was approximately 205 MB/s at 57.6 KB, 268-272 MB/s at 230.4
KB, and 292 MB/s at 921.6 KB. A 32-byte operation took 82-86 us, so IVE is not
appropriate for small control messages. Polling showed occasional multi-ms
outliers at 230.4 KB; IRQ wait kept median caller CPU near 80-87 us through
720p and is the preferable mode at those sizes.

The 1080p IRQ result exposed a driver defect rather than a bandwidth limit:
polling completed in 7003 us p50 at 296 MB/s, but IRQ mode took 11998 us and
about 6008 us of caller CPU. The CV181x driver hard-codes `TIMEOUT_MS` to 1 and
waits only `msecs_to_jiffies(1)` for `frame_done`; a ~7 ms transfer exceeds that
deadline. `CVI_IVE_DMA()` then discards the ioctl return code, making the
timeout invisible to the application. Treat IRQ mode as untrustworthy above
the one-tick completion boundary until the kernel timeout and userspace error
propagation are fixed. Use polling for a standalone transfer of this size,
accepting that it occupies the Linux CPU for the full operation.

1. Add a small, independently runnable benchmark using preallocated physically
   contiguous buffers. Sweep representative transfer sizes (small control
   blocks, model tensors, 640x360, 1280x720, and 1080p planes), aligned and
   deliberately misaligned strides, and cached versus uncached mappings.
2. For each case record IVE hardware time from `/proc/ive/hw_profiling`, ioctl
   wall time, effective payload bandwidth, Linux CPU time, and polling versus
   interrupt-wait behavior. Include warm-up and p50/p95/p99, not one sample.
3. Check byte-for-byte correctness, guard bytes, overlap restrictions, maximum
   dimensions/stride, concurrency with VI/VPSS/VENC/TPU, and whether IVE
   serializes simultaneous clients. Record failures as well as throughput.
4. Preserve the present first data point as the baseline: a ~0.9 MB luma plane
   takes 3087 us in hardware and 3165 us in the ioctl, about 299 MB/s of payload
   and ~78 us of non-engine overhead. Do not mix cache maintenance or application
   queue/release time into this number.
5. Before AMP use, determine which memory region is visible to Linux, C906, and
   IVE; which side owns each buffer; and which side performs clean/invalidate at
   every ownership transition. Benchmark the complete Linux-to-C906-to-Linux
   handoff, including mailbox/IPI and cache costs, because raw DMA time alone
   will not predict offload value.
6. Keep a checked-in results table with SDK/kernel/clock/memory configuration so
   later AMP experiments can compare against the same reproducible baseline.

---

## 5. Remove capture mapping/cache overhead -- DONE, VERIFIED ON HARDWARE

1. `CVI_SYS_MmapCache` already invalidates the mapped range; the explicit
   `CVI_SYS_IonInvalidateCache` immediately afterwards duplicated the operation.
   **DONE, VERIFIED ON HARDWARE:** local `cvi_sys.c` proves that
   `CVI_SYS_MmapCache()` calls `CVI_SYS_IonInvalidateCache()` after a successful
   map. The second invalidation has been removed from both detector capture and
   colour-preview capture. Verify image identity and `map` timing on hardware
   before beginning persistent mapping.

   Hardware verification passed in normal and 100 ms preview-stress modes.
   `map` fell from about 0.26 ms to 0.16-0.17 ms. Both runs held 31.2-31.3 fps,
   processed every capture sequence with zero stale frames, and reported zero
   preview failures or ownership errors. Under stress, `32 published = 9
   dequeued + 23 replaced`, pending depth stayed one, and borrowed-frame
   pressure stayed at two.
2. Cache mappings by VPSS physical address instead of mmap/munmap on every
   frame. The pool has a small fixed set of blocks, so map each address once and
   unmap them during teardown. Continue the one required invalidate per reuse.
   **DONE, VERIFIED ON HARDWARE:** the detector now retains one mapping per
   physical block, uses `MmapCache` (including its internal invalidate) on the
   first encounter, explicitly invalidates on subsequent VPSS reuse, and unmaps
   all blocks only after capture/preview workers have stopped. The camera line
   reports cumulative mapping hits, misses, and cached block count; the expected
   steady state is five misses followed exclusively by hits.

   Hardware verification passed. The cache learned exactly five physical
   blocks and remained at five misses while hits rose from 136 to 168 across
   the reported windows. Normal `map` fell from 0.16-0.17 ms to 0.11 ms, with
   31.2 fps, every sequence processed, and zero preview errors. The 100 ms
   worker-stress window maintained 31.4 fps and exact latest-value accounting
   (`32 published = 8 dequeued + 24 replaced`), with two borrowed frames and no
   ownership errors. Its 0.29 ms map wall time occurred without any new miss or
   mapping and is scheduler noise, not remapping.
3. Preserve the verified one-frame latest-value capture slot and VPSS channel
   depth of one. Re-run both keeping-up and deliberately saturated tests after
   every change; `seq mean/max/sum` and `processed + stale` must retain the same
   invariants.
4. Evaluate `VI_ONLINE_VPSS_ONLINE` and VPSS buffer-wrap/low-delay facilities as
   separate runtime experiments. Do not assume a mode name means lower usable
   latency; measure stability, frame age, memory bandwidth, and image identity.

   **VI-online experiment DONE, not promoted.** Set
   `TINYTAG_LIVE_VI_ONLINE=1` to select `VI_ONLINE_VPSS_ONLINE`; the verified
   `VI_OFFLINE_VPSS_ONLINE` path remains the default. Compare startup, image and
   detection identity, capture sequences/staleness, detector/preview throughput,
   and any nonzero first-frame PTS. Existing application timers cannot by
   themselves prove sensor-to-result latency, so a throughput-neutral result is
   not sufficient reason to change the default.

   Both modes produce a nonzero PTS that tracks elapsed board time and appears
   to be monotonic microseconds. The camera diagnostics now report guarded
   `steady_clock - PTS` mean/max age at detector acquisition. This enables a
   direct relative online/offline freshness comparison; samples are suppressed
   if the clocks differ or the result falls outside a sane 0-10 second range.

   Direct A/B hardware runs produced the same 3.73 ms mean acquisition age;
   offline/online max was 4.25 ms and online/online max was 4.29 ms. Frame
   layout, mapping, detection output, and ownership were identical. The first
   reported windows contained only 12 startup frames (`seq sum 11`) and are not
   steady-state throughput measurements, but their matched PTS age provides no
   reason to change the verified offline/online default.

   **VPSS wrap rejected for the detector channel by source audit.** The CV181x
   driver treats channel wrap as VPSS-to-VENC slice-buffer mode: it requires the
   downstream binding to be VENC and skips normal completed VB output for that
   channel. This application obtains complete YUV400 frames with
   `CVI_VPSS_GetChnFrame()` for CPU/TPU processing, so enabling wrap would remove
   the frame contract the detector requires. It remains potentially useful only
   for a separate VPSS channel bound directly to VENC, not the exact-luma
   borrowed-Y preview path.

---

## 6. Isolate preview/VENC/RTSP from detection -- DONE FOR PRIMARY LUMA PATH

1. Keep preview waiting isolated from detection. Preview latency and frame rate
   are secondary; dropping or displaying late frames is acceptable, but a slow
   VENC/RTSP client must never reduce detector freshness or throughput. The
   latest-value slot may retain at most one worker frame and one pending frame,
   leaving three blocks in the five-block detector pool for capture/detection.
2. Audit every VENC error path so acquired streams and input-frame ownership are
   released exactly once. Report send, query, get-stream, and RTSP drops.

   **Items 1-3 implemented and verified on hardware.** The existing
   generous 2000 ms VENC wait remains because preview latency is not critical
   and accepted asynchronous encodes must be drained before releasing their
   borrowed VPSS input. If a wait ever times out, the worker continues draining
   to prevent DMA use-after-release. Acquired streams are released exactly once
   even after RTSP write failure. The per-frame `QueryStatus` ioctl was removed:
   it only sizes sample-code allocations, while this app's persistent 64-entry
   pack array exceeds the driver's hard maximum of 12. Preview diagnostics now
   separate submit, empty-pack, pack-capacity, get-stream, completion-timeout, RTSP-write,
   and stream-release events and report VENC/RTSP mean and maximum wall times.

   First hardware A/B windows reported 6.02-6.44 ms mean VENC time, 0.01 ms
   mean RTSP handoff without a blocking client, and zero failures/timeouts in
   every stage. Removing `QueryStatus` preserved correct encoded-frame and
   ownership accounting. A 100 ms injected preview-worker delay preserved full
   detector throughput/freshness, bounded ownership to two borrowed frames,
   and replaced pending preview frames without blocking detection.
3. `CVI_RTSP_WriteFrame` waits for live555 to consume each block and may wait up
   to one second. Keep it strictly on the preview worker and ensure its blocking
   cannot exhaust preview surfaces or VPSS blocks.
4. **DEFERRED, LOW RETURN:** making the optional colour-preview channel
   latest-value would only make its human-visible stream fresher. It cannot
   improve detector latency/throughput, and the production `--rtsp-luma` path
   does not instantiate that channel. Revisit only if colour preview becomes a
   product requirement or measurements show it contends with detection.
5. **DEFERRED, LOW RETURN:** CVI RGN overlay could reduce CPU/cache work only
   for that same optional colour path. Current luma-preview stress tests prove
   detector isolation, so this adds complexity without a detector benefit.
6. Sensor-derived ISP/VENC frame-rate configuration is moved to the 720p60
   pre-investigation phase. The sole current OV5647 mode is 30 fps, so the
   existing value is correct today and changing its plumbing cannot improve
   the present detector.

---

## 7. Make measurements represent real latency and throughput -- DONE FOR SOFTWARE PATH

1. Split detector service time from whole-loop time. Tag printing, overlay
   publication, vector copies, and queue work currently sit outside some
   `busy` modes but inside others.

   **IMPLEMENTED AND HW VERIFIED:** `[camera]` now reports detector-thread CPU
   time alongside wall timing. At 31.2 fps, wall `busy` was 13.90 ms while CPU
   use was only 5.23 ms. The 8.67 ms difference is TPU/VENC waiting and
   scheduler preemption, not detector computation; in particular the broad
   5.51 ms `release` wall bucket is not a CPU-copy cost. Capture still handled
   every sequence and acquisition age was 3.66 ms mean / 4.30 ms max.

   The first CPU implementation stopped at frame release and omitted tag
   printing even though that work delays the next acquisition. It now measures
   through result output and reports the post-release `output` wall bucket plus
   `loop = busy + output`; interval diagnostics themselves remain outside the
   per-frame number.
2. Gate per-tag `printf`/`fflush` behind a debug/output policy or hand results to
   a nonblocking consumer. A blocked stdout pipe must not stall detection.

   **IMPLEMENTED AND HW VERIFIED:** synchronous output added 0.69 ms/frame of
   wall time in the measured scene. Disabling it preserved 31.2-31.3 fps,
   3.62-3.66 ms mean acquisition age, every capture sequence, and zero stale
   frames. The ordinary cost is mostly off-CPU terminal waiting and is absorbed
   by camera wait at 30 fps, but it is unbounded behind a slow pipe and would
   consume useful budget at higher frame rates. `run_live.sh` therefore defaults
   `TINYTAG_LIVE_TAG_OUTPUT` to `0`; set it to `1` when the textual stream is
   explicitly required. The binary itself retains `--tag-output 1` as its
   direct-invocation compatibility default. A nonblocking bounded result
   transport is still required if continuous machine-readable output becomes
   part of the production contract.
3. Report preview fps, drops, sequence gaps, and queue age independently from
   detector fps and capture `stale`.

   **DONE, VERIFIED ON HARDWARE:** detector and preview intervals report their
   own rates, gaps, drops, queue age, VENC/RTSP time, pending/borrowed surfaces,
   and ownership errors. Both normal and 100 ms preview-delay stress runs kept
   detector freshness independent of preview throughput.
4. `u32TimeRef` proves freshness but not sensor-to-result latency. Obtain a
   useful capture timestamp if the VI stack exposes one; otherwise use an
   external LED/display or GPIO timing experiment. Measure both
   sensor-to-detection and sensor-to-displayed-RTSP latency.

   **DEFERRED EXTERNAL MEASUREMENT:** software now reports acquisition age and
   approximate result-age distributions. Absolute photon-to-result and
   photon-to-display latency requires the external LED/GPIO setup; it does not
   block the completed software latency/throughput work.
5. Run long enough to report percentiles and tails, not only one-second means.
   The earlier 7 ms preprocessing spike shows why p95/p99/max matter.

   **DONE, VERIFIED ON HARDWARE:** a bounded, pre-reserved
   ten-second window reports p50/p95/p99/max for detector service wall time,
   detector-thread CPU, crop decode, acquisition age, and approximate
   result age (`acquisition age + detector service`). Sorting/reporting occurs
   only after the measured frame completes and samples are then cleared.
   A 294-frame hardware window reported service p50/p95/p99/max of
   9.98/13.70/14.16/17.84 ms, CPU 4.74/5.72/6.39/6.85 ms, crop
   4.32/5.38/6.23/8.34 ms, acquisition age 3.61/4.37/4.48/4.52 ms, and
   approximate result age 13.58/17.46/17.77/21.49 ms.

---

## 8. Reduce detector CPU cost after transport is correct -- DONE

1. Proposal NMS and thresholding can operate directly on heatmap logits because
   sigmoid is monotonic. Convert the configured probability threshold to one
   logit once, find/top-K logits, and compute sigmoid only for surviving
   proposals. This removes thousands of `exp` calls per frame.

   **DONE, VERIFIED ON HARDWARE:** thresholding and 3x3 peak NMS now read
   logits directly, retain the existing full confidence sort, and evaluate
   sigmoid only for the final capped proposals. Scratch capacity is reserved
   once at model load, so the steady-state path does not allocate. A 100-frame
   before/after run on the same saved luma frame produced identical confidence,
   ordering, ROI coordinates, crop count, and decoded ID. Live proposal decode
   fell from about 0.91 ms to 0.07 ms, saving roughly 0.84 ms/frame (13x for the
   stage).
2. Replace full sorting with a bounded top-K selection if profiling shows it is
   material. Preserve exact ordering/tie behavior and validate proposals against
   the current implementation.

   **REJECTED AS IMMATERIAL:** the entire optimized proposal stage is now only
   0.07 ms/frame. A different selection algorithm would risk tie-order changes
   for a negligible fraction of that already-small cost.
3. Prototype a synchronized 640x360 YUV400 VPSS channel plus an
   `--aligned_input` cvimodel. Match it to the 1280x720 crop-decode frame by
   `u32TimeRef`. This could remove the CPU resize and input copy, but reject it
   if the two channels cannot be paired without extra age or drops.

   **DONE, VERIFIED ON HARDWARE:** the compiler can emit an
   aligned-input model and the live application recognizes that input contract,
   adds an independent 640x360 YUV400 VPSS channel, and binds its physical
   address directly to the TPU tensor. Sixteen validation frames passed against
   the ordinary v40c model (mean output MAE 0.04434, worst 0.04795). The staged
   aligned model is therefore a v40c variant, not the older v4c network.

   Channel 0 and channel 1 are drained by independent latest-value capture
   threads. The detector takes the newest 640x360 channel-1 frame and starts
   synchronous TPU inference immediately; it does not wait for the larger
   1280x720 channel-0 frame. Only after inference does it require the same
   `u32TimeRef` from channel 0 for CPU crop/decode. Thus a later channel-0
   completion is hidden whenever its lag is no greater than inference time.
   A newer channel-0 sequence causes the inference result to be discarded
   rather than decoded against the wrong image. Diagnostics report residual
   post-inference `pair` wait, signed `full-minus-model ready` time, and pairing
   mismatches so hardware decides whether this topology is beneficial.

   The aligned-model hardware run passed: `pre` fell from 1.65 ms to 0.01 ms,
   detector `busy` from 13.34 ms to 10.11 ms, with zero pair wait, mismatch,
   stale frame, or ownership error. Acquisition age remained effectively
   unchanged (3.64 versus 3.72 ms). Channel readiness was simultaneous within
   userspace measurement noise: channel 0 minus channel 1 averaged -0.04 ms.

   **Ordinary-model compact exception verified and promoted.** The SDK's
   general rule remains correct: `--aligned_input` records
   the VPSS stride-aware tensor contract, and arbitrary VPSS frames must not be
   rebound to a compact model. This tensor is a narrow exception because it is
   one-plane uint8 grayscale, width 640 is already a multiple of the CV181x
   64-byte alignment, VPSS reports stride 640, and both representations are
   exactly 230400 bytes. The existing aligned-only API retains its safety
   check. A separate opt-in compact API additionally requires fused uint8
   GRAYSCALE, exact dimensions, `stride == width`, sufficient plane length,
   and tensor memory-size equality; any mismatch fails closed.

   The board's same-frame check compared all 75,600 output floats from the
   ordinary CPU-copy input and direct physical input and reported `exact=1,
   max-abs=0`. Steady state held 31.5 fps, 3.76 ms mean acquisition age, zero
   pair mismatch/stale/preview error, and `pre=0.01 ms`. Channel 0 again became
   observable about 0.04 ms before channel 1, within measurement noise.

   `run_live.sh` now enables compact direct input by default with the ordinary
   v40c model. Set `TINYTAG_LIVE_DIRECT_COMPACT_INPUT=0` only for the old
   copied-input A/B baseline. The expensive same-frame proof remains available
   as `TINYTAG_LIVE_VALIDATE_COMPACT_INPUT=1` but is off in production. The
   aligned conversion/model is no longer a deployment requirement for this
   fixed layout; retain the compiler option and aligned-only API for other VPSS
   widths/strides.
4. Only then profile ArUco Nano internals and allocations. Crop decode remains
   the dominant variable cost; optimize measured hot operations without changing
   `roi_expand`, recall, or strict decode behavior.

   **PROFILING DONE, VERIFIED ON HARDWARE:** behavior-neutral timers
   partition each crop into adaptive threshold, contour tracing, quad filtering,
   marker sampling/dictionary decode, and corner refinement. Per-frame totals
   also report processed pixels, contours, candidates, decode attempts, and raw
   markers so scene complexity is not confused with an implementation change.
   A representative baseline was 6.15 ms crop time: threshold 2.17 ms,
   contour 1.61 ms, quad 1.17 ms, marker 1.02 ms, and refine 0.18 ms across
   about 62.6k crop pixels, 24.6 contours, 2.0 candidates, and 6.2 attempts.

   **Allocation experiment rejected.** ArUco Nano reserves capacity for 2,048
   contour vectors and 2,048 points per crop. A candidate instead allocated one
   persistent 1280x720 threshold workspace at decoder construction and reduced
   initial contour/point capacities to 64/512 (still dynamically growable).
   The first implementation accidentally selected the workspace after
   thresholding, erasing the threshold image; fixed-image A/B caught the
   resulting zero-tag/zero-contour false speedup, and that build was never
   promoted. After correcting the ordering, the same saved frame produced
   exactly identical proposals and decoded output. A controlled 500-run test,
   however, regressed median crop decode from 4.188 to 4.281 ms (+2.2%) and
   median pipeline throughput from 124.2 to 122.7 fps. The workspace and
   reduced reserves were therefore removed. Small decoder allocations remain:
   their measured replacement did not advance latency or throughput, so a
   larger static-workspace rewrite is not justified without a new profile.

---

## 9. Make startup, shutdown, and configuration recoverable -- DONE

1. Track initialization state and unwind VI, ISP, VPSS, VENC, RTSP, IVE, ION,
   and worker threads in reverse order on every failure. A bad model or partial
   preview failure must not require a board reboot.
2. Catch detector construction/runtime exceptions at the application boundary,
   stop workers, release outstanding frames, and run the same teardown path.
3. Replace the detached ISP-control thread with a stoppable owned thread so it
   cannot call ISP APIs during teardown.

   **ITEMS 1-3 DONE, VERIFIED ON HARDWARE:** initialization state
   is tracked for SYS/VB, VI/ISP, VPSS/binding, VENC, and RTSP; setup failures
   use the common guarded teardown path. Model construction is caught before
   camera acquisition, detector runtime exceptions release owned frames, and
   the stdin control worker now uses a bounded poll and is joined before camera
   teardown.

   `/etc/hostname` as an invalid model failed with exit status 1 before camera
   initialization. Ctrl-C cleanly stopped both the production compact-direct
   path and copied-input fallback and released the ISP allocation. An automated
   test ran production for eight seconds, sent SIGINT, waited for full exit,
   then launched it again without human delay. Both runs reached 31.2-31.4 fps,
   emitted `[camera] stopping`, and had zero stale frames; the second reported
   no setup failure. The vendor AWB library emits an error during ISP teardown,
   but `ISP Vipipe Free` completes and the automated relaunch proves resources are
   not retained. A partial RTSP setup failure was not manufactured on the live
   camera; its state flags use the same audited common unwind path.
---

## 10. Native 1280x720 @ 60 fps from OV5647  -- NOT STARTED, LOW PRIORITY

**Priority: low.** This needs sensor-driver work against an NDA datasheet.
Items 3-9 address correctness and remove more immediate latency/throughput
bottlenecks without changing the sensor. Complete and measure them before this
work.

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

**Pre-investigation phase.** Before changing sensor registers, identify the
authoritative frame-rate value associated with the selected sensor mode and
thread it through ISP publication and VENC source/output configuration instead
of embedding 30 in the application. Validate the current 1080p30 mode first;
then the eventual 720p60 mode should select 60 through the same path. This work
is deliberately part of 720p60 because 30 is already correct for the only mode
supported today and provides no present detector benefit.

**Sketch.**

1. Add `OV5647_MODE_1280X720P60` to the mode enum.
2. Add `ov5647_linear_720p60_init()` with the register sequence, and select it
   from `ov5647_init()`.
3. Add the mode entry in `ov5647_cmos_param.h`: 1280x720 window, HTS/VTS for
   60 fps, exposure and gain ranges (max exposure is VTS-bound, so it changes
   with the new VTS).
4. Register the mode name in `build/sensors/sensor_list.json` and point
   `device/generic/rootfs_overlay/duos/mnt/data/sensor_cfg.ini` at it.
5. Have the new mode provide 60 fps through the sensor-derived ISP/VENC rate
   path established in pre-investigation; do not add another hardcoded value.
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
- **One Linux CPU core today; C906 AMP later.** The arm64 build's device tree
  declares a single Cortex-A53, so every Linux thread competes with the
  CPU-bound detector. SG2000 also has a C906 core, running FreeRTOS in the
  current configuration. Do not move camera work there during items 3-10, but
  keep buffer ownership, physically addressable memory, cache coherency, and
  message boundaries explicit so selected work can later be evaluated under
  AMP. Any offload must include mailbox/cache/handoff latency and retain a
  Linux-only fallback; moving CPU time is useful only if total latency or
  sustained throughput improves.
- **Build in Docker only** -- see the top-level README. Mixing host and
  container builds leaves root-owned files that break later builds.
- New runtime behaviour should be switchable from `run_live.sh` via a
  `TINYTAG_LIVE_*` variable where it makes sense to compare on hardware,
  without a rebuild.
