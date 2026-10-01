# Milk-V Duo series buildroot SDK V2

```
./build.sh lunch
```

For more detailed documentation, please refer to: [https://milkv.io/docs/duo/getting-started/buildroot-sdk](https://milkv.io/docs/duo/getting-started/buildroot-sdk)

## Always build in Docker

Use the container for **every** build, including the TinyTag app build below.

```sh
# once per machine
docker run --privileged -itd --name duodocker \
    -v "$(pwd)":/home/work milkvtech/milkv-duo:latest /bin/bash

# every build
docker exec -it duodocker /bin/bash -c \
    "cd /home/work && export FORCE_UNSAFE_CONFIGURE=1 && ./build.sh milkv-duos-glibc-arm64-sd"
```

**Never mix host and container builds.** The container runs as root, so every
object file, generated source and build directory it writes is owned by root
and mode 0644. A later build as your own user then dies part-way through with
errors that look like a broken toolchain but are not, e.g.:

    flex: could not create scripts/kconfig/zconf.lex.c
    bison: scripts/kconfig/zconf.tab.c: cannot open: Permission denied

That is simply a root-owned file your user cannot overwrite. There is no
partial fix -- the trees involved hold hundreds of thousands of files
(`buildroot/output` alone is ~500k). Recovering means either going back to
Docker (root can overwrite its own files, so nothing else is needed) or taking
the whole tree over once and never using Docker again:

    sudo chown -R "$(id -u):$(id -g)" .

Docker is the recommended choice: it is what Milk-V documents, it carries the
full dependency set, and `FORCE_UNSAFE_CONFIGURE=1` exists precisely because
buildroot refuses to run as root without it.

Note a failed build still runs `clean_all` first, so it wipes
`install/soc_<project>/` -- including the TPU SDK the TinyTag app links
against. After a failed build, run a full successful one before rebuilding the
app.

## Clean rebuild (keeping downloads)

`clean_all` does not touch `buildroot/output`, so it is not a from-scratch
build. To wipe every build output while keeping `buildroot/dl` (the download
cache) and `host-tools` (the toolchains):

```sh
scripts/clean_keep_dl.sh      # dry run: list what would be removed
scripts/clean_keep_dl.sh -f   # remove it
```

It deletes exactly the git-ignored files, so tracked sources and uncommitted
edits are safe. That includes `out/`, so copy any image you want to keep
first. The build outputs are root-owned, so the script runs inside the
`duodocker` container (`DUO_CONTAINER` overrides the name). Then rebuild with
the full sequence below, starting from step 1.

## Duo S ARM64 SD power and performance profiles

Future `milkv-duos-glibc-arm64-sd` image builds default to the SDK's
**performance profile**, through `CONFIG_OD_CLK_SEL=y` in the board defconfig.
The following rates were verified from the board's Linux clock report before
and after installing the corresponding FIP:

| Component | Normal profile | Performance profile (default) |
| --- | ---: | ---: |
| A53 running Linux | 800 MHz | 1000 MHz |
| C906L running FreeRTOS | 425 MHz | 700 MHz |
| TPU | 500 MHz | 700 MHz |
| Video-codec AXI | 360 MHz | 450 MHz |
| VIP AXI | 300 MHz | 300 MHz |
| DDR3 configuration | 1866 MT/s | 1866 MT/s |
| Core-voltage request | Existing boot setting; not measured | SDK PWM setting documented as 1.00 V |

The performance profile uses the existing FSBL `sys_pll_od()` implementation.
It changes shared PLLs, voltage and some audio/video clocks together. Higher
clocks and voltage can increase power consumption and temperature; actual
power and steady-state thermal behavior have not been measured. Clock-rate
increases do not guarantee equal gains in whole-pipeline FPS or latency.
These rates describe this ARM64 board setup; other SDK targets use different
clock paths.

To select a profile for an incremental FIP build, run **inside Docker**:

```sh
cd /home/work
source build/envsetup_milkv.sh milkv-duos-glibc-arm64-sd
setconfig OD_CLK_SEL=y                 # performance; use n for normal
clean_fsbl
build_fsbl
```

Install the resulting
`install/soc_sg2000_milkv_duos_glibc_arm64_sd/fip.bin`, preserving the board's
previous FIP, and reboot. Verify `clk_a53`, `clk_c906_1` and `clk_tpu` in
`/sys/kernel/debug/clk/clk_summary`. The clock setup is in FSBL, so a
firmware-only BLCP repack or a Linux governor change does not select a profile;
the current kernel has CPU frequency scaling disabled.

`setconfig` changes the current generated config. The next board selection or
full `./build.sh milkv-duos-glibc-arm64-sd` restores the board defconfig's
performance default. For a persistent normal-profile build, remove
`CONFIG_OD_CLK_SEL=y` from that defconfig (or explicitly disable it there).
Both normal-profile options, `OD_CLK_SEL` and `VC_CLK_OVERDRIVE`, should then
be disabled. `VC_CLK_OVERDRIVE` is a separate video-oriented SDK profile,
not the normal profile.

DDR uses separate boot-time PHY initialization and training. Keep the selected
`ddr3_1866_x16` profile; 1866 MT/s corresponds to approximately a 933 MHz
memory clock. The Linux clock report above does not verify its actual rate;
check the boot log's `Data rate=...` line. The generic 2133 MT/s option is not
established as qualified for Duo S memory.

For measurements, distinguish the 800/425 MHz baseline from the new
1000/700 MHz profile, and compare actual TinyTag latency, throughput and
correctness under sustained load. See the
[clock and threshold experiment record](docs/duo-s-freertos-threshold-experiment.md)
for benchmark results and the saved clock reports.

## TinyTag detector (this fork)

This fork adds a TinyTag AprilTag detector that runs on the Duo S NPU. It is
**not** built by `./build.sh` — it has to be cross-compiled separately and
staged into the board overlay first:

```sh
D="docker exec -it duodocker /bin/bash -c"
B=milkv-duos-glibc-arm64-sd

$D "cd /home/work && export FORCE_UNSAFE_CONFIGURE=1 && ./build.sh $B"   # 1. full build (produces the TPU SDK)
$D "cd /home/work && ./apps/tinytag_detect/build.sh $B"                  # 2. build TinyTag, stage it
$D "cd /home/work && ./apps/aruco_nano/build.sh $B"                       # 3. build ArUco Nano, stage it
$D "cd /home/work && export FORCE_UNSAFE_CONFIGURE=1 && ./build.sh $B"   # 4. rebuild so the image picks both up
```

The two app-build commands cannot be folded into step 1: they cross-compile against the cvitek TPU
SDK that step 1 itself produces, so on a first-ever build there is nothing to
link against yet.

**Re-run steps 2 and 3 after every change to the app, the model, or the board
overlay layout.** Skipping step 2 is silent — the image builds successfully and
simply has no detector in it.

New to this work? Start with the [app README](apps/tinytag_detect/README.md)
and [OV9281 camera notes](docs/ov9281-camera-port.md). The detailed experiment
record is in [the live camera workplan](docs/live-camera-workplan.md).

The default live/still model is now `tinytag_v7_synthetic_area_cost.int8.cvimodel`.
The [progress and clean-image record](docs/duo-s-progress-seal.md) documents
the bare-metal comparison, Ethernet PHY sleepable-wait fix, image packaging
checks and post-flash verification. Temporary worker-debug tools were retired;
their measurements are preserved in the [PHY diagnosis](docs/duo-s-ethernet-phy-diagnosis.md).

See [apps/tinytag_detect/README.md](apps/tinytag_detect/README.md) for usage,
[tools/tinytag_cvimodel/README.md](tools/tinytag_cvimodel/README.md) for the
model conversion toolchain, and [docs/](docs/) for performance findings and the
OV9281 camera porting notes.
