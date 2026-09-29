# Porting an OV9281 global-shutter mono camera to the Duo S

The Duo S driver and J2 configuration are implemented. The register sequence
comes from the validated Jetson OV9281 1280x800 RAW10 mode. Cross compilation
passes, but the Duo hardware path still needs a chip ID, CSI, and ISP test.

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
| **sensor drivers** (`cvi_mpi/component/isp/sensor/`) | **full source**, including OV9281 for cv181x |
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

The Duo S builds the CV181X middleware sensor tree under
`cvi_mpi/component/isp/sensor/cv181x/`. Global-shutter mono is covered:

| driver | sensor | notes |
|---|---|---|
| `ov_ov7251` | OV7251, 640x480 GS mono | **the porting template** -- same OmniVision GS mono family as OV9281 |
| `ov_ov6211` | OV6211, 400x400 GS mono | |
| `sms_sc035gs`, `sms_sc035hgs` | SC035GS 640x480 GS mono | has `_1L` single-lane variants; `sensor_cfg_SC035HGS.ini` already ships for the Duo S |
| `sms_sc132gs` | SC132GS 1280x1080 GS | closest existing resolution |

The Duo S image selects `CONFIG_SENSOR_GCORE_GC2083`, `GCORE_GC4653`,
`OV_OV5647`, and `OV_OV9281` in
`build/boards/cv181x/sg2000_milkv_duos_glibc_arm64_sd/sg2000_milkv_duos_glibc_arm64_sd_defconfig`.
The other 98 are opt-in Kconfig entries generated from
`build/sensors/sensor_list.json` by `build/scripts/gen_sensor_config.py`.

## Why OV7251 is a good template

The CV181X `ov_ov7251/` driver is the template:

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
(`ov7251_cmos.c:207`), and the CVI gain callback structure in
`cmos_gains_update()` (`ov7251_cmos.c:405`) is a useful template. The OV7251
gain register addresses do **not** transfer: it uses `0x350a/0x350b`, while
the validated OV9281 table uses `0x3508/0x3509`. Port the OV9281 gain encoding
and limits from the Jetson reference.

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

Use the Duo S **J2** camera connector for this port. J1 has a 1.8 V supply and
is excluded for the intended module. The existing J2 OV5647 configuration at
`device/generic/rootfs_overlay/duos/mnt/data/sensor_cfg_OV5647_J2.ini` provides
the connector routing: `bus_id = 2` (i2c2), `mipi_dev = 0`,
`lane_id = 5, 3, 4, -1, -1`, and `pn_swap = 0, 0, 0, 0, 0`. These are the
starting J2 settings for OV9281; verify frame delivery after probing it.

## Runtime sensor selection

The sensor is chosen at boot from `/mnt/data/sensor_cfg.ini` -- **no rebuild
needed to re-test**, as long as the driver is compiled in. Board copies live in
`device/generic/rootfs_overlay/duos/mnt/data/`:

```ini
[source]
dev_num = 1
[sensor]
name = OV_OV9281_MIPI_800P_120FPS_10BIT
bus_id = 2
sns_i2c_addr = 96
mipi_dev = 0
lane_id = 5, 3, 4, -1, -1
pn_swap = 0, 0, 0, 0, 0
mclk_en = 1
mclk = 1
```

The name matches the registered CVI sensor-mode enum. The Jetson OV9281 node uses the 7-bit I2C address `0x60`;
the Duo parser calls `atoi()`, so `sensor_cfg.ini` must spell it as decimal
`96`. The J2 lane mapping includes a clock lane and two data lanes. The Jetson
mode also specifies a 24 MHz MCLK, two-lane D-PHY, and RAW10; configure those
in the Duo sensor driver. Jetson's `lane_polarity` and reset GPIO numbers
describe its carrier routing and must not replace the J2 values above.

## Switching cameras

The board overlay points `sensor_cfg.ini` at `sensor_cfg_OV9281_J2.ini`, so
the rebuilt SD image starts with OV9281. Both sensor libraries remain in the
image. To switch a running Duo S, change the symlink and restart the camera
application:

```sh
cd /mnt/data
# To select the existing OV5647 module:
ln -sfn sensor_cfg_OV5647_J2.ini sensor_cfg.ini
# To select OV9281 again:
ln -sfn sensor_cfg_OV9281_J2.ini sensor_cfg.ini
```

The INI file selects the sensor name, I2C bus/address, MIPI receiver, lanes,
and MCLK output. A different camera needs its own driver and matching INI
profile; the OV5647 driver and profile remain available. `run_live.sh` selects
the orientation default from the profile, with `TINYTAG_LIVE_MIRROR` as an
override. The live app crops OV9281's central 1280x720 band before its 16:9
detector and preview outputs; native captures retain all 1280x800 pixels.

The first Duo S capture reached 120.6 fps without reported frame failures.
The 1280x800 NV21 frame had detailed luma but magenta false color: the
monochrome OV9281 has no color channels. The shared sample VI setup now
enables the CV181X ISP mono control for OV9281 after loading the tuning bin.
A second 1280x800 dump from the updated `sample_sensor_test` had all U and V
samples exactly 128 and retained detailed luma. See
`docs/ov9281-sample-mono-luma.png`; the first capture is preserved in
`docs/ov9281-sample-luma.png` and `docs/ov9281-sample-color.png`.

## Hardware checks still needed

- **Duo timing.** First capture was stable at 120.6 fps with no reported
  frame failures. Check longer runs and varying exposure conditions.
- **Exposure/gain semantics.** The new driver uses OV9281 registers
  `0x3508/0x3509`, a linear 1/16x gain code, and 2 to VTS minus 12 exposure
  lines. Check the resulting brightness and AE response on a live frame.
- **Mono handling.** The dedicated ISP mono control is enabled for OV9281 in
  the shared sample VI setup. A new YUV dump confirmed neutral chroma.
- **ISP tuning.** `isp_tuning/` ships `.bin`/`.json` for only 5 sensors, none
  global-shutter or mono. Probably not needed for a grayscale detector, but AE
  behaviour may need attention.
- **Hardware.** J2 chip-ID probing and CSI frame delivery succeeded. J1 remains
  excluded because its 1.8 V supply does not meet this module's connection
  plan.

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
