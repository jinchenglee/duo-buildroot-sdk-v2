# Porting an OV9281 global-shutter mono camera to the Duo S

Investigation notes, not an implementation. Everything below was read out of
this SDK tree; the "Open questions" section lists what was *not* verified and
needs a datasheet or the actual hardware.

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

## Sketch of the work

1. `cp -r cvi_mpi/component/isp/sensor/sg200x/ov_ov7251 .../ov_ov9281`, rename
   files/symbols `ov7251` -> `ov9281`, `OV7251` -> `OV9281`.
2. `ov9281_sensor_ctl.c`: replace the init register array with OV9281's
   1280x800 sequence, set `OV9281_CHIP_ID` to `0x9281`.
3. `ov9281_cmos_param.h`: mode table -> 1280x800, real HTS/VTS, exposure and
   again min/max/default from the datasheet. Update `combo_dev_attr_s` for
   2 data lanes if the module is wired that way.
4. `ov9281_cmos_ex.h`: rename the mode enum (`OV9281_MODE_1280X800P120`).
5. Add `"OV_OV9281"` to `build/sensors/sensor_list.json`; the Kconfig entry is
   generated from it.
6. Enable `CONFIG_SENSOR_OV_OV9281=y` in the board defconfig.
7. Add `device/generic/rootfs_overlay/duos/mnt/data/sensor_cfg_OV9281.ini` and
   point `sensor_cfg.ini` at it.
8. Bring-up order: chip-ID probe over i2c3 first (proves power, clock, reset,
   address), then a raw frame dump, then the ISP pipeline.

## Open questions -- verify before starting

- **Datasheet.** OV9281 register details are under OmniVision NDA. The mainline
  Linux `ov9281`/`ov9282` driver
  (`drivers/media/i2c/ov9282.c`) is a usable cross-reference for the init
  sequence and mode timings, and is GPL.
- **Exposure/gain semantics.** The addresses match OV7251 but the *ranges* and
  the gain table almost certainly differ. `AgainInfo` in `ov7251_cmos.c` will
  need rebuilding for OV9281.
- **Mono handling.** Nothing in the OV7251 driver explicitly flags "mono" --
  it declares `RAW_DATA_10BIT` and the ISP appears to treat it as Bayer with
  BLC only (`g_stIspBlcCalibratio`, all channels 64). How a mono stream reaches
  the application as single-channel Y was **not** traced. Worth understanding
  before writing code, since the network wants grayscale.
- **ISP tuning.** `isp_tuning/` ships `.bin`/`.json` for only 5 sensors, none
  global-shutter or mono. Probably not needed for a grayscale detector, but AE
  behaviour may need attention.
- **Hardware.** The Duo S CSI connector pinout, lane count, and whether the
  module's power rails and MCLK match were not verified against a real OV9281
  board.

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
