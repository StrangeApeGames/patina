cmake_minimum_required(VERSION 3.20)
project(patina VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PATINA_VIEWER "Build the native viewer window (patina view)" ON)
option(PATINA_USD "Build USD export (needs the third_party/LightUSD submodule)" ON)

if(MSVC)
  add_compile_options(/W3 /MP /fp:fast /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /utf-8)
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG")
else()
  add_compile_options(-Wall -Wno-unused-function -Wno-missing-field-initializers -fno-math-errno)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
endif()

# ---- third-party single-header implementations (compiled once, rarely rebuilt)
add_library(patina_thirdparty STATIC src/thirdparty_impl.c third_party/mikktspace/mikktspace.c)
target_include_directories(patina_thirdparty PUBLIC third_party third_party/mikktspace)

# ---- engine core (everything except the CLI entry point and the viewer)
add_library(patina_core STATIC
  src/util.cpp
  src/parallel.cpp
  src/json.cpp
  src/mesh.cpp
  src/bvh.cpp
  src/bake.cpp
  src/bake_normal.cpp
  src/bake_gpu.cpp
  src/noise.cpp
  src/image.cpp
  src/eval.cpp
  src/library.cpp
  src/export.cpp
  src/render.cpp
  src/envmap.cpp
  src/project.cpp
  src/commands.cpp
  src/mcp.cpp
  src/update.cpp
)
target_include_directories(patina_core PUBLIC src third_party)

# Embed files as byte arrays (cmake/embed.c, built first): docs/AGENT_GUIDE.md for `library topic=guide`,
# `patina guide` and the installed skill; the Blender bridge so an installed binary can run `blender`
# without the repo; the default studio HDRI (CC0, see third_party/hdri/README.txt) for renders.
add_executable(patina_embed cmake/embed.c)
function(patina_embed file name)
  set(out ${CMAKE_BINARY_DIR}/generated/${name}.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND patina_embed ${file} ${out}
    DEPENDS patina_embed ${file} VERBATIM)
  target_sources(patina_core PRIVATE ${out})
  set_source_files_properties(${out} PROPERTIES HEADER_FILE_ONLY TRUE)
endfunction()
patina_embed(${CMAKE_SOURCE_DIR}/docs/AGENT_GUIDE.md patina_guide)
patina_embed(${CMAKE_SOURCE_DIR}/tools/blender/patina_blender.py patina_blender)
patina_embed(${CMAKE_SOURCE_DIR}/third_party/hdri/studio_small_09_1k.hdr patina_studio_hdri)
target_include_directories(patina_core PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(patina_core PUBLIC PATINA_VERSION="${PROJECT_VERSION}")

# GPU baker (src/gpu.h). Windows: Direct3D 11 compute kernels, compiled here by the Windows SDK's fxc (it sits
# next to rc.exe) into one bytecode header each, so nothing is compiled at run time. Other platforms, or a
# Windows toolchain without fxc, build the CPU baker only.
if(WIN32)
  get_filename_component(PATINA_SDK_BIN "${CMAKE_RC_COMPILER}" DIRECTORY)
  find_program(PATINA_FXC fxc HINTS "${PATINA_SDK_BIN}" "$ENV{WindowsSdkVerBinPath}/x64")
endif()
if(WIN32 AND PATINA_FXC)
  target_sources(patina_core PRIVATE src/gpu_d3d11.cpp)
  foreach(kernel surface project bevel)
    string(TOUPPER ${kernel} KERNEL)
    set(out ${CMAKE_BINARY_DIR}/generated/bake_${kernel}.h)
    add_custom_command(OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
      COMMAND ${PATINA_FXC} /nologo /T cs_5_0 /E main /O3 /D KERNEL_${KERNEL}=1 /Vn g_bake_${kernel} /Fh ${out}
              ${CMAKE_SOURCE_DIR}/src/shaders/bake.hlsl
      DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/bake.hlsl VERBATIM)
    target_sources(patina_core PRIVATE ${out})
    set_source_files_properties(${out} PROPERTIES HEADER_FILE_ONLY TRUE)
  endforeach()
else()
  if(WIN32)
    message(WARNING "fxc.exe (Windows SDK) not found: building without the GPU baker")
  endif()
  target_sources(patina_core PRIVATE src/gpu_none.cpp)
endif()

# USD export (src/usd.h) through LightUSD, the library that was tinyusdz. third_party/LightUSD is a submodule of our
# fork, on the same branch and commit as the engine's cooker uses. Only what export needs is compiled: the text and
# crate writers, and the readers that check what was written. That is under half of the library, with no Tydra.
# The list is every member the linker loads (link /VERBOSE) when src/usd.cpp is linked against the whole library.
# After moving the submodule, a missing file shows up as an unresolved symbol at link time.
set(LIGHTUSD ${CMAKE_SOURCE_DIR}/third_party/LightUSD/src)
if(PATINA_USD AND NOT EXISTS ${LIGHTUSD}/tinyusdz.hh)
  message(WARNING "third_party/LightUSD is not checked out (git submodule update --init): building without USD export")
  set(PATINA_USD OFF)
endif()
if(PATINA_USD)
  set(LIGHTUSD_SOURCES
    ascii-parser-basetype.cc ascii-parser-entry.cc ascii-parser-props.cc ascii-parser-timesamples-array.cc
    ascii-parser-timesamples.cc ascii-parser.cc asset-resolution.cc core/instance-key.cc crate-format.cc
    crate-path-utils/path_sort.cc crate-reader-arrays.cc crate-reader-paths.cc crate-reader-timesamples.cc
    crate-reader-values.cc crate-reader.cc crate-writer.cc enum-handlers.cc hash-util.cc integerCoding.cpp io-util.cc
    layer.cc linear-algebra.cc lz4-compression.cc lz4/lz4.c parser-timing.cc path-util.cc pprint-ar.cc pprint-enum.cc
    pprint-geom.cc pprint-light.cc pprint-media.cc pprint-meta.cc pprint-physics.cc pprint-shader.cc pprint-skel.cc
    pprinter.cc prim-reconstruct-ar.cc prim-reconstruct-media.cc prim-reconstruct-physics.cc
    prim-reconstruct-shader.cc prim-reconstruct.cc prim-types.cc primvar.cc sconv-ar.cc sconv-geom.cc sconv-light.cc
    sconv-media.cc sconv-physics.cc sconv-shader.cc sconv-skel.cc stage-converter.cc stage.cc str-util.cc
    timesamples-pprint.cc timesamples.cc tiny-format.cc tiny-string.cc tinyusdz.cc usdGeom.cc usdLux.cc
    usda-reader.cc usdc-reader-prim.cc usdc-reader-property.cc usdc-reader-reconstruct.cc usdc-reader.cc
    usdc-writer.cc value-eval-util.cc value-pprint.cc value-types.cc xform.cc)
  list(TRANSFORM LIGHTUSD_SOURCES PREPEND ${LIGHTUSD}/)
  add_library(patina_lightusd STATIC ${LIGHTUSD_SOURCES})
  target_include_directories(patina_lightusd PRIVATE ${LIGHTUSD} ${LIGHTUSD}/external)
  # It is someone else's code: its warnings are not ours, and /fp:fast stays out of how it prints and parses numbers.
  get_target_property(LIGHTUSD_OPTIONS patina_lightusd COMPILE_OPTIONS)
  list(REMOVE_ITEM LIGHTUSD_OPTIONS /W3 /fp:fast -Wall)
  set_target_properties(patina_lightusd PROPERTIES COMPILE_OPTIONS "${LIGHTUSD_OPTIONS}")
  if(MSVC)
    target_compile_options(patina_lightusd PRIVATE /w /bigobj)
    target_compile_definitions(patina_lightusd PRIVATE WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(patina_lightusd PRIVATE -w)
  endif()

  # usd.cpp is the only file of ours that can include the library. LightUSD's source folder has its own copies of
  # headers we also vendor (stb_image.h), so its folders are on the include path of this one file and no other.
  target_sources(patina_core PRIVATE src/usd.cpp)
  if(MSVC)
    set_source_files_properties(src/usd.cpp PROPERTIES COMPILE_OPTIONS
      "/external:I${LIGHTUSD};/external:I${LIGHTUSD}/external;/external:W0;/bigobj")
  else()
    set_source_files_properties(src/usd.cpp PROPERTIES COMPILE_OPTIONS "-isystem;${LIGHTUSD};-isystem;${LIGHTUSD}/external")
  endif()
  target_link_libraries(patina_core PRIVATE patina_lightusd)
else()
  target_sources(patina_core PRIVATE src/usd_none.cpp)
endif()
target_link_libraries(patina_core PUBLIC patina_thirdparty)
find_package(Threads REQUIRED)
target_link_libraries(patina_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(patina_core PUBLIC advapi32 user32)  # install: user PATH in the registry
endif()

add_executable(patina src/main.cpp)
target_link_libraries(patina PRIVATE patina_core)
if(WIN32)
  add_custom_command(TARGET patina PRE_LINK COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:patina> -P ${CMAKE_SOURCE_DIR}/cmake/move_aside.cmake)
endif()

if(PATINA_VIEWER)
  add_library(patina_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
  )
  target_include_directories(patina_imgui PUBLIC third_party/imgui)

  if(APPLE)
    set(SOKOL_BACKEND SOKOL_METAL)
    set(SOKOL_IMPL_SRC src/sokol_impl.m)
    set_source_files_properties(src/sokol_impl.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  elseif(WIN32)
    set(SOKOL_BACKEND SOKOL_D3D11)
    set(SOKOL_IMPL_SRC src/sokol_impl.c)
  else()
    set(SOKOL_BACKEND SOKOL_GLCORE)
    set(SOKOL_IMPL_SRC src/sokol_impl.c)
  endif()

  target_sources(patina PRIVATE src/viewer.cpp ${SOKOL_IMPL_SRC})
  target_compile_definitions(patina PRIVATE PATINA_HAS_VIEWER=1 SOKOL_NO_ENTRY ${SOKOL_BACKEND})
  target_link_libraries(patina PRIVATE patina_imgui)
  if(APPLE)
    target_link_libraries(patina PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework MetalKit")
  elseif(WIN32)
    target_link_libraries(patina PRIVATE d3d11 dxgi)
    # keep a console subsystem so the same binary works as CLI + MCP server
  else()
    target_link_libraries(patina PRIVATE X11 Xi Xcursor GL dl m)
  endif()
endif()
