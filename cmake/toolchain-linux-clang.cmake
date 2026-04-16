# Native Linux x86_64 toolchain — clang + libc++
# Same stdlib as macOS and Android NDK.
#
# Prerequisites (Ubuntu 22.04+):
#   sudo apt install clang lld libc++-dev libc++abi-dev
# Prerequisites (Ubuntu 20.04):
#   Install clang-14 from apt.llvm.org, then symlink or use versioned names.
#
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-clang.cmake

# Use versioned clang if available (apt.llvm.org on older distros), else bare name
find_program (_clang   NAMES clang-14 clang)
find_program (_clangpp NAMES clang++-14 clang++)
find_program (_lld     NAMES lld-14 lld ld.lld-14 ld.lld)

set (CMAKE_C_COMPILER   "${_clang}")
set (CMAKE_CXX_COMPILER "${_clangpp}")

set (CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set (CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
set (CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
