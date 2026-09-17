# Sample frames

Installed to `/app/tinytag_detect/samples/` in the image by `build.sh`, so a
freshly flashed board has something to run the detector against without any
copying.

| file | what it exercises |
|---|---|
| `arena-1280x800.jpg` | the **real deployment path**: larger than the 1280x720 band, so `pre_process` crops the bottom-left band and then resizes. This is the timing figure that matters (~1.7 ms of pre_process on a Duo S). |
| `frame-640x360.png` | the network input size exactly, so `pre_process` is a plain memcpy with no resize (~0.19 ms). Useful for isolating TPU time from resize cost. |

`frame-640x360.png` is one of the held-out validation frames, and is also
embedded in the golden bundle used by `--selftest` -- so its expected output is
known. Both are raw, un-annotated frames.

## Reference output

To check the board against FP32 for either of these:

    tools/tinytag_cvimodel/tpu_docker.sh run \
      tools/tinytag_cvimodel/reference_proposals.py \
        --onnx /assets/.../tinytag-v40c-unfrozen-moderate30ep.static.onnx \
        --image apps/tinytag_detect/samples/arena-1280x800.jpg

Measured agreement on a Duo S: proposals match FP32 within ~1.4 px, with
occasional low-confidence peaks shifting by one grid cell or dropping out --
an expected INT8 effect, since the heatmap logit quantizes to ~0.045 steps.
