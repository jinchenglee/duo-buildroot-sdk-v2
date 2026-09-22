# Pure ArUco Nano live detector for the Milk-V Duo S. Reads the same OV5647
# VI -> VPSS feed as apps/tinytag_detect/live_camera.cc (channel 0, 1280x720
# YUV400, latest-value capture) but runs the stock ArUco Nano detector on the
# whole frame instead of the tinytag two-stage proposal/crop path.
#
# Unlike tinytag_detect this has *no* cvitek TPU dependency: no cviruntime /
# cvikernel / cvimodel. It only needs the MPI stack (cvi_mpi), OpenCV, and the
# vendored ArUco Nano + dictionary. OPENCV_PATH therefore defaults to the
# TPU SDK's bundled OpenCV headers, purely because those headers are already a
# build product of a full ./build.sh and are known to build aruco_dictionary.cpp
# against the board's OpenCV 3.2 runtime. See build.sh.
cmake_minimum_required(VERSION 3.10)
project(aruco_nano C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED TPU_SDK_PATH)
  message(FATAL_ERROR "Set TPU_SDK_PATH to the cvitek_tpu_sdk directory (for its bundled OpenCV headers)")
endif()
if(NOT DEFINED OPENCV_PATH)
  set(OPENCV_PATH "${TPU_SDK_PATH}/opencv")
endif()
include_directories(${OPENCV_PATH}/include)
link_directories(${OPENCV_PATH}/lib)
# The cvitek TPU SDK also ships the arm64 libz in its lib/ dir; without this
# search path the final link cannot resolve -lz (pulled in by opencv_imgcodecs
# / aruco_dictionary). This only adds a search directory -- it does not link a
# cviruntime/cvikernel library, so no TPU dependency is pulled in.
if(DEFINED TPU_SDK_PATH)
  link_directories(${TPU_SDK_PATH}/lib)
endif()

# The board already ships these three in /mnt/system/lib, so nothing extra has
# to be installed alongside the binary.
set(OPENCV_LIBS opencv_core opencv_imgcodecs opencv_imgproc)

if(NOT DEFINED MIDDLEWARE_SDK_ROOT)
  message(FATAL_ERROR "Set MIDDLEWARE_SDK_ROOT to the cvi_mpi directory")
endif()
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
# VENC + cvi_rtsp headers/libs for the live RTSP preview (--rtsp / --rtsp-luma).
if(NOT DEFINED CVI_RTSP_ROOT)
  message(FATAL_ERROR "Set CVI_RTSP_ROOT to the cvi_rtsp directory (see build.sh)")
endif()
include_directories(${CVI_RTSP_ROOT}/include/cvi_rtsp)

# ArUco Nano plus the single OpenCV 4.10 translation unit that provides
# cv::aruco::Dictionary. Compiling that TU standalone avoids pulling in the
# whole objdetect -> calib3d -> features2d chain, which the board's OpenCV 3.2
# does not have. See third_party/*/README.md.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/aruco_nano
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv_aruco_dict
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv_aruco_dict/opencv2/objdetect)

add_executable(aruco_nano
    main.cc
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

# sample_common_sensor.c gates each sensor's entry in its
# SAMPLE_COMM_SNS_GetSnsObj() switch behind #if defined(SENSOR_xxx). This app
# is intentionally OV5647-only, like tinytag_detect_live, so it uses the rebuilt
# libsns_ov5647.so and the exact OV5647 image the tinytag live path sees.
target_compile_definitions(aruco_nano PRIVATE SENSOR_OV_OV5647)

target_compile_options(aruco_nano PRIVATE -ffunction-sections -fdata-sections)
target_link_options(aruco_nano PRIVATE -Wl,--gc-sections)
target_link_libraries(aruco_nano
    ${OPENCV_LIBS}
    ${MIDDLEWARE_SDK_ROOT}/lib/libsys.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libvi.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libvpss.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libvo.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libgdc.so
    ${MIDDLEWARE_SDK_ROOT}/lib/librgn.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libisp.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libisp_algo.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libawb.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libae.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libaf.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libcvi_bin_isp.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libcvi_bin.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libvenc.so
    ${CVI_RTSP_ROOT}/install/lib/libcvi_rtsp.so
    ${MIDDLEWARE_SDK_ROOT}/lib/3rd/libini.so
    ${MIDDLEWARE_SDK_ROOT}/lib/libsns_ov5647.so
    dl stdc++ pthread z m)

install(TARGETS aruco_nano DESTINATION app/aruco_nano)
