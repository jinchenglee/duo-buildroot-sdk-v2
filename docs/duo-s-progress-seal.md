# Duo S progress: bare-metal diagnosis, PHY fix and clean image

## Findings and decisions

### Threshold offload: FreeRTOS was not the main bottleneck

The portable adaptive-mean threshold kernel was checked against OpenCV on the
board, including ROI stride, isolated border replication, tiny regions and
rounding. Local optimizations brought its A53 performance close to OpenCV.
OpenCV and `scalar-a53` timings both come from the board, not the x86 host.

The original clock report showed A53 at 800 MHz and C906L at 425 MHz. The
existing SDK performance profile raises them to 1000/700 MHz, and TPU to
700 MHz. This profile is the persistent ARM64 SD board default. DDR remains
the board's 1866 MT/s configuration; no speculative DDR overclock was applied.
See [power/performance profiles](../README.md#duo-s-arm64-sd-power-and-performance-profiles).

Even at 700 MHz the remote scalar threshold kernel was much slower than A53.
A separate bare-metal FIP removed FreeRTOS scheduling and interrupts while
retaining the same kernel, compiler settings and shared-buffer/cache handover:

| ROI | FreeRTOS compute ms | Bare-metal compute ms |
| --- | ---: | ---: |
| 640x360 | 15.11100 | 15.05961 |
| 1280x800 | 67.78074 | 67.83551 |

Busy-poll NOP round trip fell from 11.21 us to 6.21 us, but computation barely
changed. Therefore **removing FreeRTOS did not solve the computation gap**.
The current kernel offload is unsuitable for reducing critical-path latency;
this does not rule out FreeRTOS or other small-core workloads. CPU frequency,
generated instructions, memory/cache behavior and algorithm/vector optimization
remain possible factors. The bare-metal experiment reports firmware identity
and cache/prefetch control registers to support future diagnosis; register values
alone do not establish cacheability of every shared buffer.

Normal SD builds use the vendor FreeRTOS firmware with the experimental
threshold handler, not the separate bare-metal FIP. TinyTag does not offload
thresholding unless explicitly enabled. See the
[FreeRTOS record](duo-s-freertos-threshold-experiment.md),
[bare-metal instructions/results](../apps/tinytag_detect/bare_metal/README.md)
and [bare-metal TSV](benchmarks/duo-s-threshold-bare-metal.tsv).

### Adaptive decoding and model defaults

The live binary includes adaptive decoding and exposes
`--adaptive-min-roi-area N`: a positive full-resolution proposal area in pixels.
The default remains 40000 (200x200 area); it is a provisional heuristic rather
than a validated recall boundary. The user tested 3600 with two tags present.
Adaptive decoding itself remains **off by default**. History guards, periodic
full scans, fallbacks and full-resolution corner refinement remain in effect.
See [adaptive rules and caveats](tinytag-adaptive-roi-rules.md).
`run_live.sh --help` explicitly describes both switches, their defaults,
full-resolution area units, history/full-scan behavior, corner refinement and
the 3600-area example. The final binary includes this expanded help.

Both launchers now default to
`tinytag_v7_synthetic_area_cost.int8.cvimodel`. This is the actual model retrieved
from the working board, SHA256
`f8a9835df3de77fab8241180465a79757d5862aa0e92f8bceb2e1b79dc69478c`.
It is retained in the board overlay for clean builds and fresh clones.
Live board observations are useful validation, not a systematic recall test.
The older checkpoints remain available through environment overrides.
The v7 golden bundle is not supplied; the still-image launcher now names a
v7-matched bundle so it cannot silently compare against an older checkpoint's
golden output. Generate a matching bundle before `--selftest`.

The standard app build now installs `run_adaptive_bench.sh`, using the current
live binary by default. It no longer needs the separately deployed historical
`tinytag_detect_live_adaptive_bench` executable. All launchers/binaries are
regenerated into the overlay rather than committed as staged copies.

### Periodic slowdown: Ethernet PHY busy waits

TinyTag alternated between approximately 105 fps and 62-67 fps while crop pixels,
candidates and the camera's 120 fps cadence were similar. During slow periods,
a kernel worker consumed 32-35% CPU, TinyTag fell to 50-54% CPU and idle reached
zero. A temporary timer-sampling module identified `phy_state_machine` in all
679 target samples out of 2999 (22.6% occupancy across 30 seconds).
Matching logs showed repeated multi-second false-link negotiations.

The callback reached `cv182xa_read_status()` in
`linux_5.10/drivers/net/phy/cvitek.c`. Its vendor workaround randomizes advertised
capabilities and polls autonegotiation using three `mdelay(10)` loops. These
burned CPU despite running in sleepable workqueue context. The patch replaces
all three active waits with `usleep_range(10000, 11000)` and explicitly includes
the delay header. Negotiation decisions, loop limits and capability restoration
are preserved. Sleep cadence can change slightly; the PHY mutex remains held
during this synchronous callback. No hardware erratum explanation is assumed
beyond what the vendor code and logs establish.

Connecting an active Ethernet cable, while retaining USB networking, removed
the observed periodic slowdown even before the kernel patch. The repeated
probe recorded 0/2999 target samples. A real partner appears to avoid the
repeating false-link condition; a cable is not a universal substitute for the
sleepable-wait fix.

After installing the patched kernel and rebooting, 20 one-second `top` samples
showed TinyTag at 67-73% CPU, system at 17-22% and idle at 14-18%. **All workers**
were at most 1%; PID 91 was 0%, but worker PIDs can change across boots.
The new log still contained repeated false-link checks. This supports the fix
removing CPU contention even while the workaround remains active. The FAT boot
file was retrieved and matched the patched FIT exactly. Cable state was not
explicitly recorded for this capture. FPS, normal Ethernet traffic and link
disconnect/reconnect behavior require separate validation.

All measurements, the diagnostic method, installation and recovery steps are
linked from the [worker diagnosis](duo-s-ethernet-phy-diagnosis.md).
The temporary probe source and build/run helpers were retired before this
progress was sealed. They are not in the commit or production image.

## Clean SD build and packaging checks

The usual `./build.sh` does not remove `buildroot/output`, and does not compile
TinyTag. An image can build successfully with old staged app binaries or none.
For this rebuild, ignored build outputs were clobbered with
`scripts/clean_keep_dl.sh -f`, including Buildroot, Linux, FSBL/FreeRTOS, U-Boot,
middleware/RTSP/TPU install outputs and app CMake directories. Download caches
and toolchains were retained. Model-conversion work, credentials and recovery
files were backed up outside the clean scope before cleaning; only credentials
were restored before building. Source edits and tracked models were retained.

The build sequence runs inside `duodocker`:

```sh
cd /home/work
export FORCE_UNSAFE_CONFIGURE=1
./build.sh milkv-duos-glibc-arm64-sd
./apps/tinytag_detect/build.sh milkv-duos-glibc-arm64-sd
./apps/aruco_nano/build.sh milkv-duos-glibc-arm64-sd
source build/envsetup_milkv.sh milkv-duos-glibc-arm64-sd
pack_rootfs
pack_sd_image
```

The first SDK build provides freshly generated dependencies for the app builds.
Rootfs and SD repacking after app installation is essential. This last repack
reuses the freshly built boot/FIP/middleware; it does not reuse pre-clobber
libraries or executables. The first SDK-only image is an intermediate artifact.

The final image must be checked directly, not just the staging directory:
extract its FAT boot files and ext4 rootfs and verify the kernel, FIP, sensor
selection/libraries, model, launchers, application binaries and required shared
libraries. Compare staged binaries after the SDK's normal stripping step.
For FAT `/boot/boot.sd`, the required file is the raw FIT
`install/.../rawimages/boot.sd`, not the vendor-header top-level `boot.sd`.

### Completed build and direct image verification

The clean SDK build, both app builds and final rootfs/SD repack succeeded.
The live binary was rebuilt again after expanding adaptive `--help`, before
the final repack. The final burnable image is:

`out/milkv-duos-glibc-arm64-sd-tinytag-v7-phy-sleep-clean.img`

Size: 941621760 bytes (approximately 898 MiB). SHA256:
`b45e2f43189ca24843c52d91757c71d7bb877b69e4ae757b3fd0f31d55d7a757`.
The `.img.sha256` sidecar records this checksum. An earlier timestamped image
from the SDK-only pass is an intermediate; burn the named final image above.

Verification parsed the final image's MBR, extracted its FAT boot/FIP files,
and extracted the ext4 filesystem with `debugfs`. Results are preserved in
[the verification record](benchmarks/duo-s-clean-image-verification.txt):

- Boot FIT and FIP match this clean SDK build. The FIT kernel payload matches
  the freshly generated compressed Image. The newly compiled PHY object has
  three calls to `usleep_range` and no active delay calls.
- The live, still, threshold benchmark, DMA benchmark, point-LDC diagnostic
  and ArUco executables match their new installed builds after stripping.
  CMake removes build RPATH on installation; live/TinyTag code and constant
  sections were also compared directly against compiler outputs.
- Every staged TinyTag file matches the extracted image. Both launcher defaults
  select the checked-in v7 model; the adaptive benchmark uses the current live
  binary, and the compiled binary contains the expanded adaptive help.
- All 67 staged SDK shared-library files and their symlink targets match the
  extracted rootfs. All 32 direct TinyTag ELF dependencies are present.
  Sensor, OpenCV, TPU runtime, media and RTSP libraries were additionally
  compared with their fresh SDK outputs after stripping. Closed vendor
  components are refreshed from the SDK releases rather than rebuilt from
  unavailable source; open components are rebuilt.
- OV9281 J2 is the selected sensor symlink; its profile matches source.
  OV5647 J2 configuration and library are also present. GC2083 settings were
  not changed.
- The selected configuration enables OD clocks and DDR3 1866. FIP's little-core
  payload matches the freshly built vendor FreeRTOS firmware. No temporary
  worker probe is in the filesystem.

Build logs are retained in `duodocker:/tmp/tinytag-clean-*-build.log` and
`/tmp/tinytag-clean-repack.log`. Research/model-conversion work and prior
boot/application recovery files were restored only after the final build and
verification; they did not supply the new app or middleware build outputs.
The pre-clean archive is also retained at
`/tmp/tinytag-clean-rebuild/before-clean-assets.tar.gz` on the host.

## After burning

Check the clock summary for A53 1000 MHz/C906L 700 MHz/TPU 700 MHz, and the
boot serial log for DDR configuration. Confirm `/mnt/data/sensor_cfg.ini`
selects OV9281 J2 and OV5647 remains selectable via the camera configuration.
Run the default live model without a model environment override:

```sh
/app/tinytag_detect/run_live.sh --no-rtsp --max 6
# Repeat the user's adaptive experiment explicitly:
/app/tinytag_detect/run_live.sh --no-rtsp --max 6 \
  --adaptive-decode 1 --adaptive-min-roi-area 3600
```

Check tag IDs/corners, latency/FPS and a 20-second `top` capture with Ethernet
unplugged, then connected. Verify Ethernet traffic and disconnect/reconnect.
Image content checks establish packaging, not runtime behavior of a newly
flashed board. Keep the existing board's boot/FIP backups until this test passes.
