# Cross-build for the device's native ARM64 Linux userland (Debian trixie arm64) with Clang and lld.
# The sysroot comes from tools/android/fetch_arm64_sysroot.py.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
get_filename_component(GOW3_REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT DEFINED GOW3_ARM64_SYSROOT)
  set(GOW3_ARM64_SYSROOT "${GOW3_REPO}/.local-deps/android/arm64-sysroot")
endif()
set(CMAKE_SYSROOT "${GOW3_ARM64_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)

# Apple ar silently produces empty archives for the cross-compiled ThinLTO
# objects used by FEX.  Honour the archiver selected by the Android build
# scripts and use the matching llvm-ranlib from the same installation.
if(DEFINED ENV{GOW3_AR} AND NOT "$ENV{GOW3_AR}" STREQUAL "")
  set(CMAKE_AR "$ENV{GOW3_AR}" CACHE FILEPATH "LLVM archiver" FORCE)
  get_filename_component(GOW3_LLVM_BIN "${CMAKE_AR}" DIRECTORY)
  set(GOW3_LLVM_RANLIB "${GOW3_LLVM_BIN}/llvm-ranlib")
  if(EXISTS "${GOW3_LLVM_RANLIB}")
    set(CMAKE_RANLIB "${GOW3_LLVM_RANLIB}" CACHE FILEPATH "LLVM archive indexer" FORCE)
  endif()
endif()
if(NOT CMAKE_AR)
  find_program(CMAKE_AR NAMES llvm-ar REQUIRED)
endif()
if(NOT CMAKE_RANLIB)
  find_program(CMAKE_RANLIB NAMES llvm-ranlib REQUIRED)
endif()
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_ASM_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
# Libraries and packages from the sysroot; header-only packages (GOW3_ARM64_HEADER_PREFIX) may come
# from the host. Programs (code generators, glslang) run on the host.
set(CMAKE_FIND_ROOT_PATH "${GOW3_ARM64_SYSROOT}" ${GOW3_ARM64_HEADER_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${GOW3_ARM64_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${GOW3_ARM64_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${GOW3_ARM64_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
# pkgconf (conda-forge builds of it) enables --define-prefix by default, which rewrites the
# "/usr" prefix of the Debian .pc files to a sysroot-relative path. Combined with
# PKG_CONFIG_SYSROOT_DIR that yields a double-prefixed, non-existent include path
# (<sysroot>/<sysroot>/usr/lib/include). These two switches restore freedesktop pkg-config
# behaviour, which prefixes the sysroot exactly once. Both are unknown to freedesktop
# pkg-config and therefore harmless there.
set(ENV{PKG_CONFIG_DONT_DEFINE_PREFIX} "1")
set(ENV{PKG_CONFIG_FDO_SYSROOT_RULES} "1")
# The env vars above are ignored by older pkgconf; --dont-define-prefix is the authoritative switch
# and is only understood by pkgconf, so probe for it before handing it to pkg_check_modules.
execute_process(COMMAND pkg-config --dont-define-prefix --version
                RESULT_VARIABLE GOW3_PKG_CONFIG_DONT_DEFINE_PREFIX
                OUTPUT_QUIET ERROR_QUIET)
if(GOW3_PKG_CONFIG_DONT_DEFINE_PREFIX EQUAL 0)
    set(PKG_CONFIG_ARGN "--dont-define-prefix")
endif()
