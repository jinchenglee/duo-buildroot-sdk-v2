# Cross-compiled against the cvitek TPU SDK that build.sh drops in
# install/soc_<project>/tpu_64bit/. Configure with that SDK's own toolchain file:
# see build.sh in this directory, which works out both paths for you.
cmake_minimum_required(VERSION 3.10)

project(tinytag_detect C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED TPU_SDK_PATH)
  message(FATAL_ERROR "Set TPU_SDK_PATH to the cvitek_tpu_sdk directory")
endif()
include_directories(${TPU_SDK_PATH}/include)
link_directories(${TPU_SDK_PATH}/lib)

if(NOT DEFINED OPENCV_PATH)
  set(OPENCV_PATH ${TPU_SDK_PATH}/opencv)
endif()
include_directories(${OPENCV_PATH}/include)
link_directories(${OPENCV_PATH}/lib)

set(CVI_LIBS cviruntime cvikernel)
# Host builds link the simulator instead of talking to real TPU hardware.
if(NOT CMAKE_CROSSCOMPILING)
  list(APPEND CVI_LIBS cvicmodel)
endif()

# The board already ships these three in /mnt/system/lib, so nothing extra has
# to be installed alongside the binary.
set(OPENCV_LIBS opencv_core opencv_imgcodecs opencv_imgproc)

# ArUco Nano plus the single OpenCV 4.10 translation unit that provides
# cv::aruco::Dictionary. Compiling that TU standalone avoids pulling in the
# whole objdetect -> calib3d -> features2d chain, which the board's OpenCV 3.2
# does not have. See third_party/*/README.md.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/aruco_nano
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv_aruco_dict
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv_aruco_dict/opencv2/objdetect)

add_executable(tinytag_detect
    main.cc
    tinytag_det.cc
    golden.cc
    tag_crop_decoder.cc
    ../common/point_ldc.cc
    third_party/opencv_aruco_dict/aruco_dictionary.cpp)

# The dictionary TU defines every predefined ArUco/AprilTag table; only 36h11
# is referenced, so let the linker drop the rest (~400 KB of .rodata).
target_compile_options(tinytag_detect PRIVATE -ffunction-sections -fdata-sections)
target_link_options(tinytag_detect PRIVATE -Wl,--gc-sections)
target_link_libraries(tinytag_detect
    ${CVI_LIBS}
    ${OPENCV_LIBS}
    dl stdc++ pthread z m)

install(TARGETS tinytag_detect DESTINATION app/tinytag_detect)

# Offline check and per-ROI timing of point-level LDC on a still frame
# (see point_ldc_check.cc). Needs only OpenCV, no TPU or MPI.
add_executable(point_ldc_check
    point_ldc_check.cc
    tag_crop_decoder.cc
    ../common/point_ldc.cc
    third_party/opencv_aruco_dict/aruco_dictionary.cpp)
target_compile_options(point_ldc_check PRIVATE -ffunction-sections -fdata-sections)
target_link_options(point_ldc_check PRIVATE -Wl,--gc-sections)
target_link_libraries(point_ldc_check ${OPENCV_LIBS} stdc++ pthread z m)
install(TARGETS point_ldc_check DESTINATION app/tinytag_detect)

# --- live camera capture (VI -> VPSS, no V4L2 on this SoC) -----------------
# Separate binary: it needs cvi_mpi's MPI libraries on top of everything
# tinytag_detect already links, and keeping it separate means the
# static-image CLI path (already validated on hardware) is untouched by this
# addition. See live_camera.cc's file header for why this doesn't reuse
# tdl_sdk/sample_video's own VI/VPSS bring-up (cvi_tdl + cvi_rtsp + a
# different, incompatible OpenCV version).
if(DEFINED MIDDLEWARE_SDK_ROOT)
  include_directories(
      ${MIDDLEWARE_SDK_ROOT}/include
      ${MIDDLEWARE_SDK_ROOT}/include/linux
      ${MIDDLEWARE_SDK_ROOT}/include/isp/cv181x
      ${MIDDLEWARE_SDK_ROOT}/component/isp/common
      ${MIDDLEWARE_SDK_ROOT}/3rdparty/inih
      ${MIDDLEWARE_SDK_ROOT}/sample/common)
  if(DEFINED TDL_SDK_INCLUDE_PATH)
    include_directories(${TDL_SDK_INCLUDE_PATH})
  endif()
  if(NOT DEFINED CVI_RTSP_ROOT)
    message(FATAL_ERROR "Set CVI_RTSP_ROOT to the cvi_rtsp directory (see build.sh)")
  endif()
  include_directories(${CVI_RTSP_ROOT}/include/cvi_rtsp)
  include_directories(${CVI_RTSP_ROOT}/service/3rdparty/nlohmann_json/include)

  add_executable(tinytag_detect_live
      live_camera.cc
      ../common/ldc_config.cc
      ../common/point_ldc.cc
      ../common/sw_ldc.cc
      tinytag_det.cc
      tag_crop_decoder.cc
      third_party/opencv_aruco_dict/aruco_dictionary.cpp
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_sys.c
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_vi.c
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_isp.c
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_vpss.c
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_platform.c
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_sensor.c
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_bin.c
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_venc.c
      ${MIDDLEWARE_SDK_ROOT}/component/isp/common/sensor_list.c)

  # cvi_mpi/sample/common/sample_common_sensor.c gates each sensor's entry in
  # its SAMPLE_COMM_SNS_GetSnsObj() switch behind #if defined(SENSOR_xxx).
  # This live binary is intentionally OV5647-only: linking the standalone
  # sensor library makes the opt-in 720p60 experiment use the exact library
  # rebuilt from the working tree, instead of silently resolving OV5647 from
  # the prebuilt aggregate libsns_full.so.
  target_compile_definitions(tinytag_detect_live PRIVATE
      SENSOR_OV_OV5647)

  target_compile_options(tinytag_detect_live PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(tinytag_detect_live PRIVATE -Wl,--gc-sections)
  target_link_libraries(tinytag_detect_live
      ${CVI_LIBS}
      ${OPENCV_LIBS}
      ${MIDDLEWARE_SDK_ROOT}/lib/libsys.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libvi.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libvo.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libvpss.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libgdc.so
      ${MIDDLEWARE_SDK_ROOT}/lib/librgn.so
      ${MIDDLEWARE_SDK_ROOT}/lib/3rd/libini.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libsns_ov5647.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libisp.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libisp_algo.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libawb.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libae.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libaf.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libcvi_bin_isp.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libcvi_bin.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libvenc.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libvdec.so
      ${CVI_RTSP_ROOT}/install/lib/libcvi_rtsp.so
      dl stdc++ pthread z m)

  install(TARGETS tinytag_detect_live DESTINATION app/tinytag_detect)

  # Standalone IVE DMA characterization tool. Keep this dynamically linked and
  # separate from the live path: preview borrows VPSS luma and does not copy.
  add_executable(tinytag_ive_dma_bench ive_dma_bench.cc)
  set_target_properties(tinytag_ive_dma_bench PROPERTIES SKIP_BUILD_RPATH TRUE)
  target_link_libraries(tinytag_ive_dma_bench
      ${MIDDLEWARE_SDK_ROOT}/lib/libcvi_ive.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libsys.so
      pthread m)
  install(TARGETS tinytag_ive_dma_bench DESTINATION app/tinytag_detect)
else()
  message(STATUS "MIDDLEWARE_SDK_ROOT not set -- skipping tinytag_detect_live (camera capture)")
endif()
