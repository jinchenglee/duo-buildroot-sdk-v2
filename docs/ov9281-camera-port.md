# Porting an OV9281 global-shutter mono camera to the Duo S

Porting plan and implementation notes. The Duo-specific driver is not yet
implemented, but a separate Jetson OV9281 bring-up now provides a validated
sensor register sequence, 1280x800 RAW10 timing, and useful exposure/gain
behavior to port into the SG200X sensor API. The remaining unknowns are
Duo-specific: MIPI wiring, power/clock/reset behavior, CVI sensor callbacks,
and ISP treatment of the monochrome stream.

Why OV9281: it is 1280x800 global-shutter mono, which is exactly the resolution
the TinyTag model was trained at (`528.198329.jpg` and `220-225.mp4` are both
1280x800), and `apps/tinytag_detect`'s `pre_process` already crops a 1280x720
band out of that. Feeding the network from this sensor needs no retraining and
no change to the application's preprocessing.

## How open is the platform

Mostly open. The parts a camera port touches are all source.

| layer | status |
|---|---|
| fsbl / opensbi / u-boot / linux_5.10 | full source |
| `cvi_mpi` media pipeline (VI, VPSS, MIPI RX) | source, 1103 `.c` files |
| **sensor drivers** (`cvi_mpi/component/isp/sensor/`) | **full source**, 101 for sg200x |
| TPU (`cviruntime`, `cvikernel`, `tdl_sdk`) | source |
| ISP tuning data (`isp_tuning/`) | open: `.json` source compiled to `.bin` |
| **ISP 3A algorithms** (`libisp_algo.{a,so}`) | **prebuilt blob**, no `.c` under `modules/isp/*/isp_algo` |

The closed piece is the AE/AWB/AGC internals. You drive them through the
documented callback struct in `cvi_mpi/include/cvi_sns_ctrl.h`
(`pfnExpAeCb`, `pfnExpSensorCb`, `pfnSnsProbe`, ...) but cannot modify them.
For a mono sensor feeding a grayscale network this matters much less than
usual: no demosaic, no AWB, no CCM, and no photometric tuning is required for
the network to work. Only AE/AGC behaviour is affected.

## What already exists

101 sensors are compiled-capable for sg200x under
`cvi_mpi/component/isp/sensor/sg200x/`. Global-shutter mono is well covered:

| driver | sensor | notes |
|---|---|---|
| `ov_ov7251` | OV7251, 640x480 GS mono | **the porting template** -- same OmniVision GS mono family as OV9281 |
| `ov_ov6211` | OV6211, 400x400 GS mono | |
| `sms_sc035gs`, `sms_sc035hgs` | SC035GS 640x480 GS mono | has `_1L` single-lane variants; `sensor_cfg_SC035HGS.ini` already ships for the Duo S |
| `sms_sc132gs` | SC132GS 1280x1080 GS | closest existing resolution |

Only three sensors are actually *built into* the current image --
`CONFIG_SENSOR_GCORE_GC2083`, `GCORE_GC4653`, `OV_OV5647` in
`build/boards/cv181x/sg2000_milkv_duos_glibc_arm64_sd/sg2000_milkv_duos_glibc_arm64_sd_defconfig`.
The other 98 are opt-in Kconfig entries generated from
`build/sensors/sensor_list.json` by `build/scripts/gen_sensor_config.py`.

## Why OV7251 is a good template

`ov_ov7251/` is only 1455 lines in four files:

| file | lines | contains |
|---|---|---|
| `ov7251_cmos.c` | 961 | exposure/gain math, mode table, ISP callbacks |
| `ov7251_sensor_ctl.c` | 313 | I2C access, init register sequence, chip-ID probe |
| `ov7251_cmos_param.h` | 102 | resolution, HTS/VTS, MIPI lanes, MCLK, BLC |
| `ov7251_cmos_ex.h` | 79 | enums and declarations |

Critically, it uses the **standard OmniVision register map**, which OV9281
shares:

```
ov7251_cmos.c:62    #define OV7251_EXP_ADDR    0x3500   /* +0,+1,+2, 20-bit, value << 4 */
ov7251_cmos.c:63    #define OV7251_AGAIN_ADDR  0x350a   /* +0,+1 */
ov7251_cmos.c:64    #define OV7251_VTS_ADDR    0x380E   /* +0,+1 */
ov7251_sensor_ctl.c:196  OV7251_CHIP_ID_ADDR_H 0x300A
ov7251_sensor_ctl.c:197  OV7251_CHIP_ID_ADDR_L 0x300B
ov7251_sensor_ctl.c:198  OV7251_CHIP_ID        0x7750   /* OV9281 reports 0x9281 */
```

Exposure is written as three bytes in `cmos_inttime_update()`
(`ov7251_cmos.c:207`), gain through a lookup table in `cmos_gains_update()`
(`ov7251_cmos.c:405`). Both should transfer to OV9281 with only range changes.

## New OV9281 reference implementation

The local repository
`/work/git_repo/jetson-orin-kernel-builder` contains branch `ov9281`, which
has been tested on Jetson hardware with dual OV9281 cameras. It is not a
drop-in Duo driver: its module is NVIDIA `tegracam` code and its device-tree
overlays describe Jetson CSI wiring. However, it fills the most important
sensor-specific gap.

The primary reusable file is:

```text
/work/git_repo/jetson-orin-kernel-builder/scripts/ov9281/ov9281_mode_tbls_800p.h
```

It provides the OV9281 common and 1280x800 mode register tables. The validated
RAW10 mode uses:

```text
XCLK          24 MHz
MIPI          2 lanes, 800 Mbps/lane
output        RAW10 / monochrome
active size   1280x800
HTS           728 (0x02d8)
VTS           910 (0x038e)
pixel rate    160 MHz
```

Important register values from that reference are:

```text
0x030d = 0x50       RAW10 PLL setting
0x030e = 0x02
0x3662 = 0x05       RAW10 output selection
0x4800 = 0x00
0x4509 = 0x00
```

The reference reports sustained 1280x800 RAW10 capture at approximately
120.6 fps with no CSI errors on its Jetson hardware. Its notes also record two
important pitfalls: using RAW8 PLL values for RAW10 corrupts the stream, and
the RAW10 samples may be transported in the high bits of a 16-bit word. The
latter is relevant to the TinyTag display/debug path, not to the sensor driver
itself.

The Jetson branch also contains `controls.patch` and
`fix-gain-exposure.patch`. These are useful behavioral references for the Duo
AE callbacks, but their NVIDIA control framework code must be translated into
`cmos_fps_set`, `cmos_inttime_update`, and `cmos_gains_update` rather than
copied.

## Board wiring

From `build/boards/cv181x/sg2000_milkv_duos_glibc_arm64_sd/dts_arm64/sg2000_milkv_duos_glibc_arm64_sd.dts`:

```dts
&mipi_rx {
    snsr-reset = <&porta 2 GPIO_ACTIVE_LOW>, ... ;   /* sensor reset on GPIOA2 */
};
&i2c0 { status = "disabled"; };
&i2c1 { status = "okay"; };
&i2c2 { status = "okay"; };
&i2c3 { status = "okay"; };   /* camera I2C -- what sensor_cfg.ini uses */
&i2c4 { status = "okay"; };   /* also carries the gt9xx touch controller */
```

Existing sensor configs use `bus_id = 3`, i.e. i2c3, and a 2-data-lane mapping.

## Runtime sensor selection

The sensor is chosen at boot from `/mnt/data/sensor_cfg.ini` -- **no rebuild
needed to re-test**, as long as the driver is compiled in. Board copies live in
`device/generic/rootfs_overlay/duos/mnt/data/`:

```ini
[source]
dev_num = 1
[sensor]
name = SMS_SC035HGS_MIPI_480P_120FPS_12BIT
bus_id = 3
sns_i2c_addr = 30
mipi_dev = 0
lane_id = 2, 0, 1, -1, -1
pn_swap = 0, 0, 0, 0, 0
```

`lane_id = 2, 0, 1` is a clock lane plus two data lanes. OV9281 supports 1 or 2
data lanes; 2 is needed for 1280x800 at a useful frame rate.

## Duo implementation plan

1. Copy the OV7251 SG200X driver into a new `ov_ov9281` directory and rename
   its symbols and mode structures.
2. Port the Jetson register table into `ov9281_sensor_ctl.c`, preserving the
   Duo I2C access and sensor-control interfaces. Set the chip-ID probe to
   registers `0x300A/0x300B`, expected value `0x9281`.
3. Change `ov9281_cmos_param.h` to 1280x800, RAW10, 2-lane MIPI, and the
   validated HTS/VTS values. Recalculate the Duo AE exposure limits and gain
   table from OV9281 behavior; do not reuse OV7251 gain limits blindly.
4. Register the sensor in `cvi_mpi/component/isp/sensor/sg200x/Makefile`,
   `build/sensors/sensor_list.json`, and the Duo S defconfig.
5. Add an OV9281 `sensor_cfg.ini` selecting the correct I2C bus, sensor address,
   MIPI device, lane IDs, and P/N swaps.
6. Start with a raw capture and no assumptions about color ISP processing.
   Confirm chip ID, stable frame delivery, 1280x800 dimensions, and RAW10
   unpacking before enabling the TinyTag application.
7. Add a minimal mono ISP profile only after raw capture works. For TinyTag,
   correct luma and exposure are more important than color calibration.

The first implementation should support one fixed mode, 1280x800 at a
conservative frame rate. Adding alternate 720p/high-speed modes can wait until
the basic Duo path is stable.

## Open questions -- verify before starting

- **Duo timing validation.** The Jetson reference validates the sensor values,
  but its `pix_clk_hz` and CSI timing fields are NVIDIA-specific. The Duo
  receiver attributes must be derived and verified independently.
- **Exposure/gain semantics.** The addresses match OV7251 but the *ranges* and
  the gain table almost certainly differ. `AgainInfo` in `ov7251_cmos.c` will
  need rebuilding for OV9281.
- **Mono handling.** The Jetson reference confirms the camera produces
  monochrome `Y10`, but the OV7251 Duo driver does not explicitly flag mono; it
  declares `RAW_DATA_10BIT`. We still need to verify whether the CV181X ISP
  should bypass demosaic/AWB/CCM or expose the stream as a grayscale surface.
- **ISP tuning.** `isp_tuning/` ships `.bin`/`.json` for only 5 sensors, none
  global-shutter or mono. Probably not needed for a grayscale detector, but AE
  behaviour may need attention.
- **Hardware.** The Duo S CSI connector pinout, lane count, P/N polarity, and
  whether the module's power rails, reset GPIO, and MCLK match remain to be
  verified against the actual OV9281 board.

## Why this is worth doing

The rest of the pipeline is already measured on hardware: TPU proposals plus
ArUco Nano tag decode run in **11.25 ms (88.9 fps)** at 1280x800, of which the
NPU is only 2.11 ms -- see `docs/duo-s-performance-findings.md`. Capture is the
last missing stage, and there is a large budget left for it.

Note this is single-threaded on the one Cortex-A53 the arm64 build exposes, so
a capture path that costs CPU competes directly with the 6.5 ms crop-decode
stage. A VPSS/hardware path is preferable to anything that copies frames in
software.

Note that a camera-fed pipeline would come through VPSS rather than a file
read, and a VPSS-fed model wants `--aligned_input` at cvimodel compile time
(width-aligned frames). `apps/tinytag_detect` deliberately *rejects* such a
model at load, since it feeds a plain contiguous buffer -- so the live-camera
path needs either a second cvimodel built with `--aligned_input` or a copy into
an unaligned buffer. Decide which before building the camera application.
