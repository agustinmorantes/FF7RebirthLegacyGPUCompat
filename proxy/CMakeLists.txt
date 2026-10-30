cmake_minimum_required(VERSION 3.20)
project(legacygpu LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# fxc/dxc from the Windows SDK. The SDK dxc ships dxil.dll, so its output is signed.
get_filename_component(SDK_BIN "$ENV{WindowsSdkVerBinPath}x64" ABSOLUTE)
find_program(FXC fxc HINTS "${SDK_BIN}" REQUIRED)
find_program(DXC dxc HINTS "${SDK_BIN}" NO_DEFAULT_PATH REQUIRED)

# The DXC release tools/ms2cs.py and tools/fix16.py use, plus dxilasm (IR to signed DXIL) next to it.
set(DXC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tools/dxc-1.9.2609)
if(NOT EXISTS ${DXC_DIR}/bin/x64/dxcompiler.dll)
  file(DOWNLOAD https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip
    ${CMAKE_CURRENT_BINARY_DIR}/dxc.zip SHOW_PROGRESS
    EXPECTED_HASH SHA256=ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1)
  file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_BINARY_DIR}/dxc.zip DESTINATION ${DXC_DIR})
endif()
add_executable(dxilasm ../tools/dxilasm/dxilasm.cpp)
target_include_directories(dxilasm PRIVATE ${DXC_DIR}/inc)
set_target_properties(dxilasm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DXC_DIR}/bin/x64)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(
  OUTPUT ${GEN}/empty_cs.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND ${FXC} /nologo /T cs_5_0 /E main /Vn g_emptyCS /Fh ${GEN}/empty_cs.h ${CMAKE_CURRENT_SOURCE_DIR}/shaders/empty_cs.hlsl
  DEPENDS shaders/empty_cs.hlsl)

add_library(d3d12proxy SHARED
  src/proxy.cpp src/hooks.cpp src/meshprof.cpp src/generate.cpp src/vtidx.c src/exports.asm src/d3d12.def
  ${GEN}/empty_cs.h)
set_target_properties(d3d12proxy PROPERTIES OUTPUT_NAME d3d12)
target_include_directories(d3d12proxy PRIVATE ${GEN})
target_compile_definitions(d3d12proxy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(d3d12proxy PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W3>)
# The proxy must not import d3d12.dll (itself); only dxguid for IIDs.
target_link_libraries(d3d12proxy PRIVATE dxguid)
# Themed common controls for the shader-conversion progress window (generate.cpp activates it).
target_link_options(d3d12proxy PRIVATE "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
add_custom_command(TARGET d3d12proxy POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/legacygpu.ini $<TARGET_FILE_DIR:d3d12proxy>)

# Test harness: links the real import lib, so the loader picks up the proxy from the exe dir.
add_custom_command(
  OUTPUT ${GEN}/test_ms.h ${GEN}/test_ps.h
  COMMAND ${DXC} -nologo -T ms_6_5 -E MSMain -Vn g_testMS -Fh ${GEN}/test_ms.h ${CMAKE_CURRENT_SOURCE_DIR}/test/test_shaders.hlsl
  COMMAND ${DXC} -nologo -T ps_6_5 -E PSMain -Vn g_testPS -Fh ${GEN}/test_ps.h ${CMAKE_CURRENT_SOURCE_DIR}/test/test_shaders.hlsl
  DEPENDS test/test_shaders.hlsl)
# 16-bit test CS, plus its fix16.py conversion where the proxy next to proxytest looks for it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
  OUTPUT ${GEN}/test_half.h ${GEN}/test_half.dxil
  COMMAND ${DXC} -nologo -T cs_6_6 -enable-16bit-types -E main -Vn g_testHalfCS -Fh ${GEN}/test_half.h -Fo ${GEN}/test_half.dxil ${CMAKE_CURRENT_SOURCE_DIR}/test/test_half.hlsl
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/fix16.py ${CMAKE_CURRENT_BINARY_DIR}/legacygpu_shaders ${GEN}/test_half.dxil
  DEPENDS test/test_half.hlsl ../tools/fix16.py dxilasm)
# Mesh-shader emulation test: the MS, its ms2cs.py pair next to proxytest, and the PS/CS.
set(EMU ${CMAKE_CURRENT_SOURCE_DIR}/test/test_emu.hlsl)
add_custom_command(
  OUTPUT ${GEN}/test_emu_ms.h ${GEN}/test_emu_ms.dxil ${GEN}/test_emu_ps.h ${GEN}/test_emu_cs.h
  COMMAND ${DXC} -nologo -T ms_6_6 -E MSMain -Vn g_emuMS -Fh ${GEN}/test_emu_ms.h -Fo ${GEN}/test_emu_ms.dxil ${EMU}
  COMMAND ${DXC} -nologo -T ps_6_6 -E PSMain -Vn g_emuPS -Fh ${GEN}/test_emu_ps.h ${EMU}
  COMMAND ${DXC} -nologo -T cs_6_0 -E CSMain -Vn g_emuCS -Fh ${GEN}/test_emu_cs.h ${EMU}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ms2cs.py ${CMAKE_CURRENT_BINARY_DIR}/legacygpu_shaders ${GEN}/test_emu_ms.dxil
  DEPENDS test/test_emu.hlsl ../tools/ms2cs.py ../tools/fix16.py dxilasm)
add_custom_command(
  OUTPUT ${GEN}/guard_cs.h
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/build_guard_fixture.py ${GEN}/test_emu_ms.dxil ${GEN}/guard_cs.h
  DEPENDS ${GEN}/test_emu_ms.dxil test/build_guard_fixture.py ../tools/ms2cs.py ../tools/fix16.py dxilasm)
# Per-primitive outputs test: the converted pair and the PS, embedded.
set(PRIM ${CMAKE_CURRENT_SOURCE_DIR}/test/test_prim.hlsl)
add_custom_command(
  OUTPUT ${GEN}/prim.h ${GEN}/prim_ps.h
  COMMAND ${DXC} -nologo -T ms_6_6 -E MSMain -Fo ${GEN}/test_prim_ms.dxil ${PRIM}
  COMMAND ${DXC} -nologo -T ps_6_6 -E PSMain -Vn g_primPS -Fh ${GEN}/prim_ps.h ${PRIM}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/build_prim_fixture.py ${GEN}/test_prim_ms.dxil ${GEN}/prim.h
  DEPENDS test/test_prim.hlsl test/build_prim_fixture.py ../tools/ms2cs.py ../tools/fix16.py dxilasm)
add_custom_command(
  OUTPUT ${GEN}/retry_fixture.h
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/build_retry_fixture.py ${CMAKE_CURRENT_SOURCE_DIR}/test/test_retry.hlsl ${GEN}/retry_fixture.h
  DEPENDS test/test_retry.hlsl test/build_retry_fixture.py ../tools/ms2cs.py ../tools/fix16.py dxilasm)
add_executable(proxytest test/test.cpp test/guard_test.cpp test/prof_test.cpp test/pass_test.cpp test/prim_test.cpp test/retry_test.cpp ${GEN}/test_ms.h ${GEN}/test_ps.h ${GEN}/test_half.h ${GEN}/test_emu_ms.h
  ${GEN}/test_emu_ps.h ${GEN}/test_emu_cs.h ${GEN}/guard_cs.h ${GEN}/prim.h ${GEN}/prim_ps.h ${GEN}/retry_fixture.h)
target_include_directories(proxytest PRIVATE ${GEN})
target_link_libraries(proxytest PRIVATE d3d12 dxgi dxguid)
add_dependencies(proxytest d3d12proxy)
