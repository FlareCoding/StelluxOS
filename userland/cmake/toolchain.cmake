# The body of the CMake toolchain files for programs that run on Stellux, toolchain-x86_64.cmake and
# toolchain-aarch64.cmake, which set STELLUX_ARCH. Nested projects, such as CMake's own checks, load
# the file again without the cache, so the architecture is the file's rather than a variable's.
#
# It compiles the way userland/mk/toolchain.mk does and links the way userland/mk/cxxapp.mk does,
# against the sysroot and the SDK's Qt kit. Qt programs also link STELLUX_QT_PLATFORM_LIBRARIES,
# which registers the stellux platform plugin.

if(NOT STELLUX_ARCH MATCHES "^(x86_64|aarch64)$")
    message(FATAL_ERROR "Use the toolchain file of an architecture, toolchain-x86_64.cmake or toolchain-aarch64.cmake")
endif()

get_filename_component(STELLUX_USERLAND "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(STELLUX_SYSROOT "${STELLUX_USERLAND}/sysroot/${STELLUX_ARCH}")
set(STELLUX_QT_DIR "${STELLUX_USERLAND}/toolchain/sdk/${STELLUX_ARCH}/qt")
set(STELLUX_BUILTINS "${STELLUX_SYSROOT}/lib/libclang_rt.builtins-${STELLUX_ARCH}.a")

if(NOT EXISTS "${STELLUX_SYSROOT}/lib/libc++.a" OR NOT EXISTS "${STELLUX_BUILTINS}")
    message(FATAL_ERROR "The ${STELLUX_ARCH} sysroot is incomplete, run make musl libcxx compiler-rt first")
endif()

# Apple's clang has no lld, so on macOS the Homebrew LLVM keg compiles, as in scripts/host.mk
if(CMAKE_HOST_APPLE)
    set(STELLUX_LLVM_HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
endif()

find_program(STELLUX_CLANG clang HINTS ${STELLUX_LLVM_HINTS} REQUIRED)
find_program(STELLUX_CLANGXX clang++ HINTS ${STELLUX_LLVM_HINTS} REQUIRED)
find_program(STELLUX_LLVM_AR llvm-ar HINTS ${STELLUX_LLVM_HINTS} REQUIRED)
find_program(STELLUX_LLVM_RANLIB llvm-ranlib HINTS ${STELLUX_LLVM_HINTS} REQUIRED)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ${STELLUX_ARCH})
set(CMAKE_SYSROOT ${STELLUX_SYSROOT})
set(CMAKE_C_COMPILER ${STELLUX_CLANG})
set(CMAKE_CXX_COMPILER ${STELLUX_CLANGXX})
set(CMAKE_C_COMPILER_TARGET ${STELLUX_ARCH}-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET ${STELLUX_ARCH}-linux-musl)
set(CMAKE_AR ${STELLUX_LLVM_AR})
set(CMAKE_RANLIB ${STELLUX_LLVM_RANLIB})

set(CMAKE_C_FLAGS_INIT "-nostdlibinc -isystem ${STELLUX_SYSROOT}/include")
set(CMAKE_CXX_FLAGS_INIT "-nostdlibinc -isystem ${STELLUX_SYSROOT}/include/c++/v1 -isystem ${STELLUX_SYSROOT}/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-nostdlib -fuse-ld=lld -static ${STELLUX_SYSROOT}/lib/crt1.o ${STELLUX_SYSROOT}/lib/crti.o -L${STELLUX_SYSROOT}/lib")
set(CMAKE_C_STANDARD_LIBRARIES
    "-Wl,--start-group -lc -lm ${STELLUX_BUILTINS} -Wl,--end-group ${STELLUX_SYSROOT}/lib/crtn.o")
set(CMAKE_CXX_STANDARD_LIBRARIES
    "-Wl,--start-group -lc++ -lc++abi -lunwind -lc -lm ${STELLUX_BUILTINS} -Wl,--end-group ${STELLUX_SYSROOT}/lib/crtn.o")

# Roots a project passes in CMAKE_FIND_ROOT_PATH, such as its own dependencies, are searched too.
# The Qt kit lies outside the sysroot, and its code generators come from the host Qt make qt builds.
list(APPEND CMAKE_FIND_ROOT_PATH ${STELLUX_SYSROOT} ${STELLUX_QT_DIR})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(QT_HOST_PATH "${STELLUX_USERLAND}/toolchain/qt-host" CACHE PATH "Host Qt for the kit's code generators")

# libstlxqpa is linked whole so its plugins register, as userland/mk/cxxapp.mk links it
set(STELLUX_QT_PLATFORM_LIBRARIES
    -Wl,--whole-archive ${STELLUX_SYSROOT}/lib/libstlxqpa.a -Wl,--no-whole-archive
    ${STELLUX_SYSROOT}/lib/libstlxwin.a ${STELLUX_SYSROOT}/lib/libstlxconf.a ${STELLUX_SYSROOT}/lib/libstlx.a)
