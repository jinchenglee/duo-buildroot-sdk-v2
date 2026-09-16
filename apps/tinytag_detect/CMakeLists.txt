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

install(TARGETS tinytag_detect DESTINATION usr/local/bin)
