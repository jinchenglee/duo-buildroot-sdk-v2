# riscv64 musl cross-compilation, for the musl-riscv64 Duo variants.
# See toolchain-aarch64-linux.cmake for why these live here rather than being
# taken from the TPU SDK.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

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

set(TOOLCHAIN_ROOT "${HOST_TOOLS_PATH}/gcc/riscv64-linux-musl-x86_64")
set(TOOLCHAIN_PREFIX "${TOOLCHAIN_ROOT}/bin/riscv64-unknown-linux-musl-")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++")
set(CMAKE_AR           "${TOOLCHAIN_PREFIX}ar"      CACHE FILEPATH "ar")
set(CMAKE_RANLIB       "${TOOLCHAIN_PREFIX}ranlib"  CACHE FILEPATH "ranlib")
set(CMAKE_STRIP        "${TOOLCHAIN_PREFIX}strip"   CACHE FILEPATH "strip")

# The Duo's musl userspace is built for the C906's rv64imafdcv0p7_xthead ABI;
# these flags mirror what the SDK uses for its own musl_riscv64 targets.
set(CMAKE_C_FLAGS_INIT   "-march=rv64imafdcv0p7xthead -mabi=lp64d -mcmodel=medany")
set(CMAKE_CXX_FLAGS_INIT "-march=rv64imafdcv0p7xthead -mabi=lp64d -mcmodel=medany")

set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
