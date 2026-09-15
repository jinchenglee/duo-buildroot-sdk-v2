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

add_executable(tinytag_detect
    main.cc
    tinytag_det.cc
    golden.cc)
target_link_libraries(tinytag_detect
    ${CVI_LIBS}
    ${OPENCV_LIBS}
    dl stdc++ pthread z m)

install(TARGETS tinytag_detect DESTINATION usr/local/bin)
