# aarch64 glibc cross-compilation, using the toolchain ./build.sh fetches into
# host-tools/.
#
# The cvitek TPU SDK ships equivalent files under
# install/soc_<project>/tpu_64bit/cvitek_tpu_sdk/cmake/, but those are extracted
# mode 0640 root:root, so they are unreadable to the normal user who runs this
# build. Shipping our own copy keeps the app buildable without touching the
# ownership of SDK build products.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Derived from this file's own location (apps/tinytag_detect/cmake -> repo root)
# rather than taken from a -D cache variable: CMake runs the toolchain file again
# inside try_compile sub-projects, which do NOT inherit -D definitions, so a
# cache variable would be empty exactly when the compiler is being probed.
# HOST_TOOLS_PATH may still be set explicitly to override.
if(NOT DEFINED HOST_TOOLS_PATH)
  get_filename_component(HOST_TOOLS_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../host-tools" ABSOLUTE)
endif()
if(NOT EXISTS "${HOST_TOOLS_PATH}/gcc")
  message(FATAL_ERROR "host-tools not found at ${HOST_TOOLS_PATH}; run ./build.sh once to fetch it")
endif()

set(TOOLCHAIN_ROOT "${HOST_TOOLS_PATH}/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu")
set(TOOLCHAIN_PREFIX "${TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++")
set(CMAKE_AR           "${TOOLCHAIN_PREFIX}ar"      CACHE FILEPATH "ar")
set(CMAKE_RANLIB       "${TOOLCHAIN_PREFIX}ranlib"  CACHE FILEPATH "ranlib")
set(CMAKE_STRIP        "${TOOLCHAIN_PREFIX}strip"   CACHE FILEPATH "strip")

set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
