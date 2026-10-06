cmake_minimum_required(VERSION 3.24)
project(bl2hdr LANGUAGES C CXX)

# BL2 is a 32-bit game: configure with an x86 toolchain (see README.md, "Building").
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "bl2hdr must be built for 32-bit x86 (use build.cmd, the x86 developer prompt or -A Win32).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no redist needed

# ---- Shaders: compiled with the Windows SDK fxc into headers embedded in the DLL ----
# fxc comes with the Windows SDK. In a VS developer prompt it is on PATH; otherwise search the
# installed SDK versions (newest first).
set(_FXC_HINTS)
file(GLOB _SDK_BINS LIST_DIRECTORIES true "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*")
list(SORT _SDK_BINS COMPARE NATURAL ORDER DESCENDING)
foreach(_bin ${_SDK_BINS})
  list(APPEND _FXC_HINTS "${_bin}/x64" "${_bin}/x86")
endforeach()
find_program(FXC_BIN fxc.exe HINTS ${_FXC_HINTS} REQUIRED)
message(STATUS "fxc: ${FXC_BIN}")
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_HEADERS)
# add_ps3(<output name> <source> [defines...])
function(add_ps3 NAME SOURCE)
  set(DEFS)
  foreach(D ${ARGN})
    list(APPEND DEFS /D ${D})
  endforeach()
  add_custom_command(
    OUTPUT ${SHADER_OUT}/${NAME}.h
    COMMAND ${FXC_BIN} /nologo /T ps_3_0 /E main /O3 ${DEFS} /Vn g_${NAME} /Fh ${SHADER_OUT}/${NAME}.h
            ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
    COMMENT "fxc ps_3_0 ${NAME}")
  set(SHADER_HEADERS ${SHADER_HEADERS} ${SHADER_OUT}/${NAME}.h PARENT_SCOPE)
endfunction()
# add_sm5(<output name> <source> <profile> <entry>) — D3D12 output pass shaders
function(add_sm5 NAME SOURCE PROFILE ENTRY)
  add_custom_command(
    OUTPUT ${SHADER_OUT}/${NAME}.h
    COMMAND ${FXC_BIN} /nologo /T ${PROFILE} /E ${ENTRY} /O3 /Vn g_${NAME} /Fh ${SHADER_OUT}/${NAME}.h
            ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
    COMMENT "fxc ${PROFILE} ${NAME}")
  set(SHADER_HEADERS ${SHADER_HEADERS} ${SHADER_OUT}/${NAME}.h PARENT_SCOPE)
endfunction()
add_ps3(tonemap_replica shaders/tonemap.ps_3_0.hlsl)
add_ps3(tonemap_tint shaders/tonemap.ps_3_0.hlsl BL2HDR_TINT=1)
add_ps3(tonemap_hdr shaders/tonemap.ps_3_0.hlsl BL2HDR_HDR=1)
add_sm5(encode_vs shaders/encode.hlsl vs_5_0 vs_main)
add_sm5(encode_ps shaders/encode.hlsl ps_5_0 ps_main)
add_custom_target(bl2hdr_shaders DEPENDS ${SHADER_HEADERS})

add_library(bl2hdr SHARED
  src/shader_swap.cpp
  src/window.cpp
  src/output_dxgi.cpp
  src/backbuffer.cpp
  src/automation.cpp
  src/ue3.cpp
  src/menu.cpp
  src/dllmain.cpp
  src/log.cpp
  src/config.cpp
  src/proxy.cpp
  src/hooks_d3d9.cpp
  src/shader_hash.cpp
  src/vtable_index.c
  src/exports.def
)
add_dependencies(bl2hdr bl2hdr_shaders)
target_include_directories(bl2hdr PRIVATE ${SHADER_OUT})
target_compile_definitions(bl2hdr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(bl2hdr PRIVATE /W4 /permissive- /utf-8)
# No link against the system d3d9.lib: only d3d9.h types are used; the real DLL is loaded dynamically.
# Ship as d3d9.dll next to Borderlands2.exe; keep the import library name distinct from the system d3d9.lib.
set_target_properties(bl2hdr PROPERTIES OUTPUT_NAME "d3d9" ARCHIVE_OUTPUT_NAME "bl2hdr")
