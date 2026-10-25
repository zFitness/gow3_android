# libgportcpu.so: gport_guest_cpu.h on FEXCore, linked statically against FEX's libraries.
set(GPORTCPU_DIR "${CMAKE_CURRENT_LIST_DIR}")
add_library(gportcpu SHARED "${GPORTCPU_DIR}/guest_cpu_fex.cpp" "${GPORTCPU_DIR}/host_call_arm64.S")
target_include_directories(gportcpu PRIVATE "${GPORTCPU_DIR}/../src" "${CMAKE_SOURCE_DIR}/Source" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(gportcpu PRIVATE FEXCore Common JemallocLibs fmt::fmt)
target_link_options(gportcpu PRIVATE -Wl,--no-undefined)
target_compile_options(gportcpu PRIVATE ${FEX_TUNE_COMPILE_FLAGS})
set_target_properties(gportcpu PROPERTIES
  OUTPUT_NAME gportcpu
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)
