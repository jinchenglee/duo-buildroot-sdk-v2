# aruco_nano

Pure ArUco Nano live detector for the Milk-V Duo S. It reads the **same** OV5647
camera feed as `apps/tinytag_detect`'s live path and runs the **stock** ArUco
Nano detector over the whole frame, so the two can be compared under identical
acquisition conditions.

Where `tinytag_detect` is the two-stage detector (neural TPU proposals + per-ROI
crop decode via `TagCropDecoder`), this app is stage-two only: it skips the
network entirely and scans the full 1280x720 YUV400 plane for AprilTag 36h11
markers.

## The identical feed

The capture path mirrors `apps/tinytag_detect/live_camera.cc` exactly (the
verified, hardware-tuned setup):

- OV5647 at 1920x1080@30 (opt-in 1280x720@60 with
  `ARUCO_NANO_LIVE_OV5647_720P60=1`) -> VI -> ISP -> VPSS group 0.
- VPSS **channel 0 = 1280x720 YUV400** — the same full-resolution frame
  tinytag uses for crop decode (and for exact-luma preview).
- `VI_OFFLINE_VPSS_ONLINE` (default), VPSS device 1, group 0.
- Dedicated capture thread writes a **latest-value** slot
  (`take_latest_frame`), so the detector always sees the freshest frame (at most
  one capture period old), never a backlog.
- The plane is mapped zero-copy with `CVI_SYS_MmapCache` and a persistent
  mapping cache keyed on the VPSS physical address (no mmap/munmap per frame).
- VI **mirror correction defaults to 1**: the OV5647 module on this board
  delivers a horizontally mirrored frame and AprilTag markers are chiral, so a
  mirrored frame decodes zero tags.
- Same live ISP control (stdin commands: `gain`, `exptime`, `ae auto`, `awb`,
  `max-exposure-us` cap via `--max-exposure-us N`).

The only difference from `live_camera.cc` is what runs on the mapped frame:
`decoder->detect(gray)` on the whole 1280x720 Mat, versus tinytag's
`post_process` over a small set of neural-proposed ROI crops.

## Build and install

**Always build in the Docker container** (see the top-level README). The app has
no cvitek TPU dependency, but it links OpenCV from the TPU SDK build product, so
a full `./build.sh` must have run once:

```sh
docker exec -it duodocker /bin/bash -c \
    "cd /home/work && ./apps/aruco_nano/build.sh milkv-duos-glibc-arm64-sd"
```

Installs into `device/<board>/overlay/app/aruco_nano/`, then:

```sh
./build.sh <board>
```

bakes it into the image.

## Usage on the board

```sh
run_aruco_nano.sh                                   # strict, mirror=1, camera feed
run_aruco_nano.sh --mode tolerant                   # accept marginal tags
run_aruco_nano.sh --max-exposure-us 10000           # cap AE shutter for fixed fps
run_aruco_nano.sh --ldc-calibration /root/ldc-calibration.json  # hardware lens correction
ARUCO_NANO_LIVE_OV5647_720P60=1 run_aruco_nano.sh   # opt-in 720p60
```

Or call the binary directly:

```
aruco_nano [--mode strict|tolerant] [--mirror 0|1] [--flip 0|1]
           [--max-exposure-us N] [--save-frame frame.png]
           [--ldc-calibration FILE.json]
           [--tag-output 0|1] [--rtsp|--rtsp-luma|--no-rtsp]
           [--quiet] [--debug n]
```

| variable | default | meaning |
|---|---|---|
| `ARUCO_NANO_MODE` | `strict` | `strict` or `tolerant` |
| `ARUCO_NANO_MIRROR` | `1` | correct the horizontally-mirrored OV5647 |
| `ARUCO_NANO_FLIP` | `0` | correct a vertically-mirrored sensor |
| `ARUCO_NANO_DEBUG` | `1` | verbosity |
| `ARUCO_NANO_TAG_OUTPUT` | `0` | print a per-second batch of decoded tags on stdout |
| `ARUCO_NANO_LIVE_OV5647_720P60` | unset | set `1` for the opt-in 720p60 mode |
| `ARUCO_NANO_LIVE_VI_ONLINE` | unset | set `1` for `VI_ONLINE_VPSS_ONLINE` |
| `ARUCO_NANO_LIVE_PREVIEW_NICE` | `10` | worker niceness for the RTSP preview thread |

`strict` matches the K230/tinytag production setting
(`errorCorrectionRate` and `maxErroneousBitsInBorderRate` both 0.0). `tolerant`
raises both to 1.0: more marginal tags, more false positives, slower.

LDC uses a 1280x768 aligned VPSS surface for a visible 1280x720 image.
Detection and `--rtsp-luma` use only the valid 720 rows; the bottom 48 rows
are hardware padding. The VB pool includes the full surface and space for
GDC's temporary rotation buffer.

The first LDC start stores a GDC mesh beside the calibration JSON as
`.sg2000-ldc-*.mesh`; later starts load it in milliseconds. The cache is shared
with TinyTag when the output size and LDC parameters match. Changing those
parameters selects a new cache file. Remove mesh files after an SDK/firmware
change to regenerate them. Mesh reuse reduces startup time, not the per-frame
cost of hardware correction.

## Comparison with tinytag

Run both on the same board, same scene, same `--max-exposure-us` and mirror
settings, and compare these counts:

- `[detector] ... fps` and per-frame `wait / map / detect / release / output`,
  plus `cpu` (detector thread) and `proc-cpu` (whole process) — the single-core
  CPU cost story.
- `[tails] ... service / acquire-age / result-age` percentiles — latency, not
  throughput.
- `[aruco-profile] ... threshold / contour / quad / marker / refine / pixels /
  contours / candidates / attempts / markers` — why the decode cost moves.

The tinytag equivalent of `detect` is its `crop` stage (the per-ROI decode),
and tinytag's `decode` is the cheaper `decode_proposals`. This app's `detect`
is a whole-frame ArUco Nano run, so it is not directly comparable in ms to a
single tinytag crop; the comparable quantities are fps and per-frame CPU under
the same acquisition.

## Live preview (RTSP)

`--rtsp-luma` and `--rtsp` turn on a VENC + cvi_rtsp server so the live feed can
be watched from the host:

```
aruco_nano --rtsp-luma            # exact-luma: borrows the detector's Y plane (zero copy) + neutral chroma
aruco_nano --rtsp                 # colour: a second VPSS channel (channel 1) delivers NV21
```

The overlay (detected-tag quads + ids + a status line) is drawn into the preview,
then encoded H.264 1280x720 and served at `rtsp://<board>:554/h264`. `[preview]`
stats on stderr report `published / dequeued / encoded / fail` and the VENC/RTSP
timing; `encoded` counts frames a client could have streamed.

View on the host (prefer TCP transport over UDP):

```sh
ffprobe  -rtsp_transport tcp -i rtsp://192.168.42.1:554/h264 \
          -show_entries stream=codec_name,width,height   # quick check
ffplay   -rtsp_transport tcp rtsp://192.168.42.1:554/h264
```

Note on the bind address: the cvi_rtsp server binds `0.0.0.0:554`, so it is
reachable on whichever interface the host uses. The URL the library prints at
startup picks one interface (it showed `rtsp://192.168.10.170/h264` on a board
whose `eth0` was 192.168.10.170 but whose USB-gadget link to the host is
192.168.42.1) — connect to the IP you actually reach the board at
(`192.168.42.1` in that example), not the printed one.

## Runtime dependencies

None beyond the image: `libopencv_core/imgcodecs/imgproc.so.3.2` and the MPI
libs are all already in `/mnt/system/lib`, which `/etc/profile` puts on
`LD_LIBRARY_PATH`. `run_aruco_nano.sh` sets that path itself for non-login
shells.
