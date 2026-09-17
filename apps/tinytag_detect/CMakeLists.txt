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

  add_executable(tinytag_detect_live
      live_camera.cc
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
      ${MIDDLEWARE_SDK_ROOT}/sample/common/sample_common_venc.c)

  # cvi_mpi/sample/common/sample_common_sensor.c gates each sensor's entry in
  # its SAMPLE_COMM_SNS_GetSnsObj() switch behind #if defined(SENSOR_xxx).
  # The prebuilt tdl_sdk demo binaries (sample_vi_fd et al.) were compiled
  # with every sensor this board supports enabled; compiling this file
  # ourselves without these defines silently compiles out every case, so
  # GetSnsObj() returns NULL for a sensor that is definitely present and
  # working (confirmed against real hardware: SAMPLE_COMM_ISP_PatchSnsObj
  # failed with GetSnsObj returning NULL for OV5647, while the prebuilt
  # sample_vi_fd worked fine against the same board/ini).
  #
  # Only OV5647 and GC2083 here, not the SC035HGS/SC200AI this board's other
  # sensor_cfg_*.ini files reference: libsns_full.so's prebuilt .so does not
  # actually export stSnsSC200AI_Obj/stSnsSC035HGS_Obj (confirmed -- defining
  # those two macros is an undefined-reference link error), so this library
  # build doesn't support them regardless of what we define here. Add them
  # back if a build with those objects present is ever available.
  target_compile_definitions(tinytag_detect_live PRIVATE
      SENSOR_OV_OV5647
      SENSOR_GCORE_GC2083)

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
      ${MIDDLEWARE_SDK_ROOT}/lib/libsns_full.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libisp.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libisp_algo.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libawb.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libae.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libaf.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libcvi_bin_isp.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libcvi_bin.so
      ${MIDDLEWARE_SDK_ROOT}/lib/libvenc.so
      ${CVI_RTSP_ROOT}/install/lib/libcvi_rtsp.so
      dl stdc++ pthread z m)

  install(TARGETS tinytag_detect_live DESTINATION app/tinytag_detect)
else()
  message(STATUS "MIDDLEWARE_SDK_ROOT not set -- skipping tinytag_detect_live (camera capture)")
endif()
