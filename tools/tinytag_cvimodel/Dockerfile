# TinyTag -> cvimodel conversion environment for Milk-V Duo S (SG2000 / cv181x).
#
# sophgo/tpuc_dev:v3.1 is the image Milk-V's own TPU documentation pins, and is
# what milkv-duo/tpu-mlir's envsetup.sh checks for (Ubuntu 22.04 + Python 3.10).
# milkv-duo/tpu-mlir is a prebuilt binary drop of tpu-mlir v1.3.228 (2023-09),
# which is the version known to emit cvimodels that the cviruntime 1.1.1 shipped
# in this SDK can load. Do not "upgrade" it casually: a newer tpu-mlir can emit a
# cvimodel version the on-board runtime cannot parse.
FROM sophgo/tpuc_dev:v3.1

ARG TPU_MLIR_REPO=https://github.com/milkv-duo/tpu-mlir.git
ARG TPU_MLIR_REF=master

RUN git clone --depth 1 -b ${TPU_MLIR_REF} ${TPU_MLIR_REPO} /opt/tpu-mlir \
 && rm -rf /opt/tpu-mlir/.git

# onnxruntime gives us the FP32 reference to validate the quantized cvimodel
# against; opencv-python does calibration-frame extraction and resizing.
# numpy is pinned <2 because tpu-mlir v1.3 predates the numpy 2 ABI break.
RUN pip3 install --no-cache-dir \
      "numpy<2" \
      "onnxruntime==1.16.3" \
      "opencv-python-headless==4.8.1.78"

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
# Explicit mode, not "chmod +x": a restrictive host umask can strip o+r, and a
# shell script the container user cannot READ fails with "Permission denied".
RUN chmod 0755 /usr/local/bin/entrypoint.sh

ENV TPUC_ROOT=/opt/tpu-mlir
WORKDIR /workspace
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
