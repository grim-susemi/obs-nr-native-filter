# SPDX-License-Identifier: GPL-3.0-only
add_library(fg_control STATIC src/fg/fg_controller.cpp src/fg/anr_dll_validator.cpp src/fg/xemfg_dll_validator.cpp)
target_link_libraries(fg_control PUBLIC fg_ipc PRIVATE bcrypt)
target_compile_options(fg_control PRIVATE /W4 /WX /permissive-)
target_sources(obs-nr-filter PRIVATE src/fg/fg_tap.cpp)
target_link_libraries(obs-nr-filter PRIVATE fg_control)
option(OBS_NR_VFG_QA_ACTION "Enable the isolated OBS VFG QA settings action" OFF)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(obs-nr-filter PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()

set(ANR_BACKEND_SOURCES
  tools/fg-presenter/anr_fg_backend.cpp tools/fg-presenter/anr_commands.cpp
  tools/fg-presenter/anr_guides.cpp tools/fg-presenter/shared_guides.cpp
  tools/fg-presenter/anr_runtime.cpp)
set(VFG_PRESENTER_SOURCES
  tools/fg-presenter/vfg_presenter.cpp
  tools/fg-presenter/vfg_gpu.cpp
  tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp
  tools/fg-presenter/native_vfg.cpp
  tools/fg-presenter/pushflow_backend.cpp
  tools/fg-presenter/pushflow_compute.cpp
  tools/fg-presenter/pushflow_ort.cpp
  tools/fg-presenter/pushflow_shader_bytecode.cpp
  tools/fg-presenter/xefg_runtime.cpp
  tools/fg-presenter/xefg_presenter.cpp
  tools/fg-presenter/xemfg_runtime.cpp
  tools/fg-presenter/xemfg_companion.cpp
  tools/fg-presenter/xemfg_backend.cpp
  tools/fg-presenter/xemfg_evaluate.cpp
  src/fg/anr_dll_validator.cpp
  src/fg/xemfg_dll_validator.cpp)
# One definition for the shipping presenter and the test-fixture presenter. The
# fixture additionally compiles the NR_FG_TEST_MOCK_XESS admission branch, which
# only accepts pinned hashes supplied by the test and never exists in shipping.
function(nr_fg_presenter name)
  add_executable(${name} ${VFG_PRESENTER_SOURCES})
  target_sources(${name} PRIVATE "${VFG_CONTENT_HEADER}"
    "${PUSHFLOW_PACK_HEADER}" "${PUSHFLOW_UNPACK_HEADER}" "${PUSHFLOW_RECONSTRUCT_HEADER}")
  target_sources(${name} PRIVATE ${ANR_BACKEND_SOURCES})
  add_dependencies(${name} vfg_presenter_shaders nr_guides_shaders)
  target_include_directories(${name} PRIVATE deps/ngx deps/xess-3.0.2/inc deps/mfg-xemfg)
  target_link_libraries(${name} PRIVATE nr_guides fg_ipc d3d12 dxgi user32 winmm bcrypt wintrust crypt32)
  target_compile_features(${name} PRIVATE cxx_std_20)
  target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(${name} PRIVATE /W4 /WX /permissive-)
  if(OBS_NR_VFG_QA_ACTION)
    target_compile_definitions(${name} PRIVATE OBS_NR_VFG_QA_ACTION=1)
  endif()
  target_include_directories(${name} PRIVATE src src/fg "${CMAKE_CURRENT_BINARY_DIR}")
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-1.20.1")
  target_compile_definitions(${name} PRIVATE OBS_NR_HAVE_ONNXRUNTIME=1)
endfunction()
set(VFG_SHOT_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/vfg_shot_change.hlsl")
set(VFG_SHOT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vfg_shot_change_shader.h")
foreach(PASS update compose)
  set(HUD_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/vfg_hud_lock_${PASS}.hlsl")
  set(HUD_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vfg_hud_lock_${PASS}_shader.h")
  add_custom_command(OUTPUT "${HUD_HEADER}"
    COMMAND "${NR_COLOR_DXC}" -WX -Ges -T cs_6_0 -E main -Fh "${HUD_HEADER}"
      -Vn "vfg_hud_lock_${PASS}_shader" "${HUD_SHADER}"
    DEPENDS "${HUD_SHADER}" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${VFG_SHOT_HEADER}"
  COMMAND "${NR_COLOR_DXC}" -WX -Ges -T cs_6_0 -E main -Fh "${VFG_SHOT_HEADER}"
    -Vn vfg_shot_change_shader "${VFG_SHOT_SHADER}"
  DEPENDS "${VFG_SHOT_SHADER}" VERBATIM)
set(VFG_CONTENT_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/vfg_content_compare.hlsl")
set(VFG_CONTENT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vfg_content_compare_shader.h")
set(ANR_BGRA_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/anr_rgba_to_bgra.hlsl")
foreach(STAGE vs ps)
  set(BGRA_HEADER "${CMAKE_CURRENT_BINARY_DIR}/anr_bgra_${STAGE}.h")
  add_custom_command(OUTPUT "${BGRA_HEADER}"
    COMMAND "${NR_FXC}" /nologo /WX /Ges /T "${STAGE}_5_0" /E "${STAGE}_main" /Fh "${BGRA_HEADER}"
      /Vn "anr_bgra_${STAGE}" "${ANR_BGRA_SHADER}"
    DEPENDS "${ANR_BGRA_SHADER}" VERBATIM)
  list(APPEND ANR_BGRA_HEADERS "${BGRA_HEADER}")
endforeach()
add_custom_command(OUTPUT "${VFG_CONTENT_HEADER}"
  COMMAND "${NR_FXC}" /nologo /WX /Ges /T cs_5_0 /E main /Fh "${VFG_CONTENT_HEADER}"
    /Vn vfg_content_shader "${VFG_CONTENT_SHADER}"
  DEPENDS "${VFG_CONTENT_SHADER}" VERBATIM)
set(PUSHFLOW_PACK_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/pushflow_pack.hlsl")
set(PUSHFLOW_PACK_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pushflow_pack_shader.h")
set(PUSHFLOW_UNPACK_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/pushflow_unpack.hlsl")
set(PUSHFLOW_UNPACK_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pushflow_unpack_shader.h")
set(PUSHFLOW_RECONSTRUCT_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/pushflow_reconstruct.hlsl")
set(PUSHFLOW_RECONSTRUCT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pushflow_reconstruct_shader.h")
add_custom_command(OUTPUT "${PUSHFLOW_PACK_HEADER}"
  COMMAND "${NR_COLOR_DXC}" -WX -Ges -T cs_6_0 -E main -Fh "${PUSHFLOW_PACK_HEADER}"
    -Vn pushflow_pack_shader "${PUSHFLOW_PACK_SHADER}"
  DEPENDS "${PUSHFLOW_PACK_SHADER}" VERBATIM)
add_custom_command(OUTPUT "${PUSHFLOW_UNPACK_HEADER}"
  COMMAND "${NR_COLOR_DXC}" -WX -Ges -T cs_6_0 -E main -Fh "${PUSHFLOW_UNPACK_HEADER}"
    -Vn pushflow_unpack_shader "${PUSHFLOW_UNPACK_SHADER}"
  DEPENDS "${PUSHFLOW_UNPACK_SHADER}" VERBATIM)
add_custom_command(OUTPUT "${PUSHFLOW_RECONSTRUCT_HEADER}"
  COMMAND "${NR_COLOR_DXC}" -WX -Ges -T cs_6_0 -E main -Fh "${PUSHFLOW_RECONSTRUCT_HEADER}"
    -Vn pushflow_reconstruct_shader "${PUSHFLOW_RECONSTRUCT_SHADER}"
  DEPENDS "${PUSHFLOW_RECONSTRUCT_SHADER}" VERBATIM)
set(SHARED_FLOW_SCALE_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/shared_flow_scale.hlsl")
set(SHARED_FLOW_SCALE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/shared_flow_scale_shader.h")
add_custom_command(OUTPUT "${SHARED_FLOW_SCALE_HEADER}"
  COMMAND "${NR_COLOR_DXC}" -WX -Ges -T cs_6_0 -E main -Fh "${SHARED_FLOW_SCALE_HEADER}"
    -Vn shared_flow_scale_shader "${SHARED_FLOW_SCALE_SHADER}"
  DEPENDS "${SHARED_FLOW_SCALE_SHADER}" VERBATIM)
add_custom_target(vfg_presenter_shaders DEPENDS
  ${ANR_BGRA_HEADERS}
  "${VFG_CONTENT_HEADER}" "${VFG_SHOT_HEADER}" "${PUSHFLOW_PACK_HEADER}"
  "${PUSHFLOW_UNPACK_HEADER}" "${PUSHFLOW_RECONSTRUCT_HEADER}"
  "${SHARED_FLOW_SCALE_HEADER}"
  "${CMAKE_CURRENT_BINARY_DIR}/vfg_hud_lock_update_shader.h"
  "${CMAKE_CURRENT_BINARY_DIR}/vfg_hud_lock_compose_shader.h")
nr_fg_presenter(obs-nr-fg-presenter)
nr_fg_presenter(obs-nr-fg-presenter-xefg-mock)
target_compile_definitions(obs-nr-fg-presenter-xefg-mock PRIVATE NR_FG_TEST_MOCK_XESS=1)
add_dependencies(obs-nr-filter obs-nr-fg-presenter)

# XeSS 3.0.2 ABI fixtures. The two modules are deliberately unsigned: the
# runtime test proves they are refused by default and admitted only through the
# test-only pinned-hash branch.
add_library(xefg_mock_xell MODULE tools/tests/xefg_mock_dll.cpp)
set_target_properties(xefg_mock_xell PROPERTIES PREFIX "" OUTPUT_NAME "libxell")
target_compile_features(xefg_mock_xell PRIVATE cxx_std_20)
target_compile_definitions(xefg_mock_xell PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  XEFG_MOCK_LIBRARY_XELL=1 XELL_EXPORT_API=1)
target_compile_options(xefg_mock_xell PRIVATE /W4 /WX /permissive-)
target_include_directories(xefg_mock_xell PRIVATE deps/xess-3.0.2/inc)
target_link_libraries(xefg_mock_xell PRIVATE d3d12 dxgi)
add_library(xefg_mock_fg MODULE tools/tests/xefg_mock_dll.cpp)
set_target_properties(xefg_mock_fg PROPERTIES PREFIX "" OUTPUT_NAME "libxess_fg")
target_compile_features(xefg_mock_fg PRIVATE cxx_std_20)
target_compile_definitions(xefg_mock_fg PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  XEFG_MOCK_LIBRARY_FG=1 XEFG_SWAPCHAIN_EXPORT_API=1)
target_compile_options(xefg_mock_fg PRIVATE /W4 /WX /permissive-)
target_include_directories(xefg_mock_fg PRIVATE deps/xess-3.0.2/inc)
target_link_libraries(xefg_mock_fg PRIVATE d3d12 dxgi)

add_executable(xefg_presenter_tests tools/tests/xefg_presenter_tests.cpp
  tools/fg-presenter/xefg_runtime.cpp tools/fg-presenter/xefg_presenter.cpp
  tools/fg-presenter/vfg_gpu.cpp tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp tools/fg-presenter/pushflow_shader_bytecode.cpp)
target_compile_features(xefg_presenter_tests PRIVATE cxx_std_20)
target_compile_definitions(xefg_presenter_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  NR_FG_TEST_MOCK_XESS=1)
target_compile_options(xefg_presenter_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(xefg_presenter_tests PRIVATE src src/fg tools/fg-presenter
  deps/xess-3.0.2/inc deps/ngx "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(xefg_presenter_tests PRIVATE fg_control fg_ipc nr_guides d3d11 d3d12 dxgi
  user32 winmm bcrypt wintrust crypt32)
add_dependencies(xefg_presenter_tests vfg_presenter_shaders nr_guides_shaders xefg_mock_fg xefg_mock_xell
  obs-nr-fg-presenter-xefg-mock)
add_custom_command(TARGET xefg_presenter_tests POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:xefg_presenter_tests>/xess-fg"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:xefg_mock_fg>"
    "$<TARGET_FILE_DIR:xefg_presenter_tests>/xess-fg/libxess_fg.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:xefg_mock_xell>"
    "$<TARGET_FILE_DIR:xefg_presenter_tests>/xess-fg/libxell.dll"
  VERBATIM)
add_test(NAME xefg_presenter COMMAND xefg_presenter_tests
  "$<TARGET_FILE_DIR:xefg_presenter_tests>/xess-fg"
  "$<TARGET_FILE:obs-nr-fg-presenter-xefg-mock>")
set_tests_properties(xefg_presenter PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE LABELS gpu)

add_library(xemfg_mock_dll MODULE tools/tests/xemfg_mock_dll.cpp)
set_target_properties(xemfg_mock_dll PROPERTIES PREFIX "" OUTPUT_NAME "MFGUnlocker")
target_compile_features(xemfg_mock_dll PRIVATE cxx_std_20)
target_compile_definitions(xemfg_mock_dll PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(xemfg_mock_dll PRIVATE /W4 /WX /permissive-)
target_include_directories(xemfg_mock_dll PRIVATE deps/ngx deps/mfg-xemfg)
target_link_libraries(xemfg_mock_dll PRIVATE d3d12 dxgi)

# Opt-in XeMFG admission: unsigned-but-valid provider accepted, every
# path/PE/export/companion failure rejected before LoadLibrary.
add_executable(xemfg_validator_tests tools/tests/xemfg_validator_tests.cpp)
target_compile_features(xemfg_validator_tests PRIVATE cxx_std_20)
target_compile_definitions(xemfg_validator_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(xemfg_validator_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(xemfg_validator_tests PRIVATE src src/fg)
target_link_libraries(xemfg_validator_tests PRIVATE fg_control)
add_dependencies(xemfg_validator_tests xemfg_mock_dll)
add_test(NAME xemfg_validator COMMAND xemfg_validator_tests "$<TARGET_FILE:xemfg_mock_dll>")
set_tests_properties(xemfg_validator PROPERTIES TIMEOUT 30)

# 3x/4x transport contract: real provider ABI, SEH quarantine, syntheticTex,
# HUD-lock compose and teardown, all against the mock provider above.
add_executable(xemfg_backend_tests tools/tests/xemfg_backend_tests.cpp
  tools/fg-presenter/xemfg_backend.cpp tools/fg-presenter/xemfg_evaluate.cpp
  tools/fg-presenter/xemfg_runtime.cpp tools/fg-presenter/xemfg_companion.cpp
  tools/fg-presenter/xefg_runtime.cpp
  tools/fg-presenter/anr_guides.cpp tools/fg-presenter/shared_guides.cpp
  tools/fg-presenter/anr_commands.cpp tools/fg-presenter/anr_runtime.cpp
  tools/fg-presenter/vfg_gpu.cpp tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp tools/fg-presenter/pushflow_shader_bytecode.cpp)
target_compile_features(xemfg_backend_tests PRIVATE cxx_std_20)
target_compile_definitions(xemfg_backend_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  NR_FG_TEST_MOCK_XESS=1)
target_compile_options(xemfg_backend_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(xemfg_backend_tests PRIVATE src src/fg tools/fg-presenter
  deps/xess-3.0.2/inc deps/ngx deps/mfg-xemfg "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(xemfg_backend_tests PRIVATE fg_control fg_ipc nr_guides d3d12 dxgi
  user32 winmm bcrypt wintrust crypt32)
add_dependencies(xemfg_backend_tests vfg_presenter_shaders nr_guides_shaders xefg_mock_fg xefg_mock_xell
  xemfg_mock_dll)
add_custom_command(TARGET xemfg_backend_tests POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:xemfg_backend_tests>/xess-fg"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:xefg_mock_fg>"
    "$<TARGET_FILE_DIR:xemfg_backend_tests>/xess-fg/libxess_fg.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:xefg_mock_xell>"
    "$<TARGET_FILE_DIR:xemfg_backend_tests>/xess-fg/libxell.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:xemfg_mock_dll>"
    "$<TARGET_FILE_DIR:xemfg_backend_tests>/MFGUnlocker.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${NR_GUIDES_SHADER}" "$<TARGET_FILE_DIR:xemfg_backend_tests>/nr_guides.cso"
  VERBATIM)
add_test(NAME xemfg_backend COMMAND xemfg_backend_tests "$<TARGET_FILE:xemfg_mock_dll>")
set_tests_properties(xemfg_backend PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS gpu
  ENVIRONMENT "OBS_NR_GUIDES_SHADER=${NR_GUIDES_SHADER}")

add_library(anr_mock_dll MODULE tools/tests/anr_mock_dll.cpp)
target_compile_features(anr_mock_dll PRIVATE cxx_std_20)
target_compile_options(anr_mock_dll PRIVATE /W4 /WX /permissive-)
target_compile_definitions(anr_mock_dll PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(anr_mock_dll PRIVATE tools/fg-presenter)
target_include_directories(anr_mock_dll PRIVATE deps/ngx)
add_executable(anr_mock_dll_tests tools/tests/anr_mock_dll_tests.cpp tools/tests/anr_mock_ownership.cpp ${ANR_BACKEND_SOURCES}
  tools/fg-presenter/vfg_gpu.cpp tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp tools/fg-presenter/pushflow_shader_bytecode.cpp)
target_compile_options(anr_mock_dll_tests PRIVATE /W4 /WX /permissive-)
target_compile_definitions(anr_mock_dll_tests PRIVATE OBS_NR_ANR_MOCK_TEST)
target_include_directories(anr_mock_dll_tests PRIVATE tools/fg-presenter src/fg "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(anr_mock_dll_tests PRIVATE deps/ngx)
target_link_libraries(anr_mock_dll_tests PRIVATE fg_control nr_guides user32 d3d12 dxgi)
add_dependencies(anr_mock_dll_tests anr_mock_dll vfg_presenter_shaders nr_guides_shaders)
add_custom_command(TARGET anr_mock_dll_tests POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${NR_GUIDES_SHADER}" "$<TARGET_FILE_DIR:anr_mock_dll_tests>/nr_guides.cso"
  VERBATIM)
add_test(NAME anr_mock_dll COMMAND anr_mock_dll_tests)
set_tests_properties(anr_mock_dll PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS GPU
  ENVIRONMENT "OBS_NR_GUIDES_SHADER=${NR_GUIDES_SHADER}")

# Shared NVOF consumer policy: real D3D12 device records the imported pair, the
# scale/sanitize pass and the three ownership states.
add_executable(anr_shared_flow_tests tools/tests/anr_shared_flow_tests.cpp
  tools/fg-presenter/anr_guides.cpp tools/fg-presenter/shared_guides.cpp
  tools/fg-presenter/vfg_gpu.cpp tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp tools/fg-presenter/pushflow_shader_bytecode.cpp)
target_compile_features(anr_shared_flow_tests PRIVATE cxx_std_20)
target_compile_definitions(anr_shared_flow_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(anr_shared_flow_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(anr_shared_flow_tests PRIVATE src src/fg tools/fg-presenter "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(anr_shared_flow_tests PRIVATE fg_ipc nr_guides d3d12 dxgi user32 winmm)
add_dependencies(anr_shared_flow_tests vfg_presenter_shaders nr_guides_shaders)
add_test(NAME anr_shared_flow COMMAND anr_shared_flow_tests)
set_tests_properties(anr_shared_flow PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS gpu)

add_executable(vfg_shot_change_gpu_tests
  tools/tests/vfg_shot_change_gpu_tests.cpp
  tools/fg-presenter/vfg_gpu.cpp
  tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp
  tools/fg-presenter/pushflow_shader_bytecode.cpp)
target_compile_features(vfg_shot_change_gpu_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_shot_change_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_shot_change_gpu_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_shot_change_gpu_tests PRIVATE src src/fg "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(vfg_shot_change_gpu_tests PRIVATE fg_ipc d3d12 dxgi user32 winmm)
add_dependencies(vfg_shot_change_gpu_tests vfg_presenter_shaders)
add_test(NAME vfg_shot_change_gpu COMMAND vfg_shot_change_gpu_tests)
set_tests_properties(vfg_shot_change_gpu PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS gpu)

add_executable(vfg_hud_lock_gpu_tests
  tools/tests/vfg_hud_lock_gpu_tests.cpp
  tools/fg-presenter/vfg_gpu.cpp
  tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp
  tools/fg-presenter/pushflow_shader_bytecode.cpp)
target_compile_features(vfg_hud_lock_gpu_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_hud_lock_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_hud_lock_gpu_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_hud_lock_gpu_tests PRIVATE src src/fg tools/fg-presenter "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(vfg_hud_lock_gpu_tests PRIVATE fg_ipc d3d12 dxgi user32 winmm)
add_dependencies(vfg_hud_lock_gpu_tests vfg_presenter_shaders)
add_test(NAME vfg_hud_lock_gpu COMMAND vfg_hud_lock_gpu_tests)
set_tests_properties(vfg_hud_lock_gpu PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS gpu)

# Veyra-parity band-tolerant content comparison: mode 1 visits every pixel of a
# 64x16 tile and counts RGB components further than the band, instead of stopping
# at the first difference.
set(VFG_CONTENT_BAND_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/vfg_content_compare_band.hlsl")
set(VFG_CONTENT_BAND_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vfg_content_compare_band_shader.h")
add_custom_command(OUTPUT "${VFG_CONTENT_BAND_HEADER}"
  COMMAND "${NR_FXC}" /nologo /WX /Ges /T cs_5_0 /E main /Fh "${VFG_CONTENT_BAND_HEADER}"
    /Vn vfg_content_compare_band_shader "${VFG_CONTENT_BAND_SHADER}"
  DEPENDS "${VFG_CONTENT_BAND_SHADER}" VERBATIM)
add_custom_target(vfg_content_band_shader DEPENDS "${VFG_CONTENT_BAND_HEADER}")
add_executable(vfg_content_dedup_tests tools/tests/vfg_content_dedup_tests.cpp)
target_compile_features(vfg_content_dedup_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_content_dedup_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_content_dedup_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_content_dedup_tests PRIVATE tools/fg-presenter src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(vfg_content_dedup_tests PRIVATE d3d12 dxgi)
add_dependencies(vfg_content_dedup_tests vfg_content_band_shader)
add_test(NAME vfg_content_dedup COMMAND vfg_content_dedup_tests)
set_tests_properties(vfg_content_dedup PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS gpu)

add_executable(vfg_native_smoke_tests tools/tests/vfg_native_smoke_tests.cpp)
target_compile_features(vfg_native_smoke_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_native_smoke_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(vfg_native_smoke_tests PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()
target_compile_options(vfg_native_smoke_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_native_smoke_tests PRIVATE src/fg)
target_link_libraries(vfg_native_smoke_tests PRIVATE fg_control d3d11 dxgi user32)
add_dependencies(vfg_native_smoke_tests obs-nr-fg-presenter)

add_executable(vfg_ab_smoke_runner tools/tests/vfg_ab_smoke_runner.cpp)
target_compile_features(vfg_ab_smoke_runner PRIVATE cxx_std_20)
target_compile_definitions(vfg_ab_smoke_runner PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_ab_smoke_runner PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_ab_smoke_runner PRIVATE src/fg)
target_link_libraries(vfg_ab_smoke_runner PRIVATE fg_control d3d11 dxgi user32)
add_dependencies(vfg_ab_smoke_runner obs-nr-fg-presenter)

add_executable(vfg_monitor_text_tests tools/tests/vfg_monitor_text_tests.cpp)
target_compile_features(vfg_monitor_text_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_monitor_text_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_monitor_text_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_monitor_text_tests PRIVATE src/fg)
add_test(NAME vfg_monitor_text COMMAND vfg_monitor_text_tests "${CMAKE_CURRENT_SOURCE_DIR}/data/locale")

add_executable(vfg_session_controller_tests tools/tests/vfg_session_controller_tests.cpp)
target_compile_features(vfg_session_controller_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_session_controller_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_session_controller_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_session_controller_tests PRIVATE src/fg)
target_link_libraries(vfg_session_controller_tests PRIVATE fg_control user32)
add_dependencies(vfg_session_controller_tests vfg_native_smoke_tests)
add_test(NAME vfg_session_controller COMMAND vfg_session_controller_tests
  $<TARGET_FILE:obs-nr-fg-presenter> "${CMAKE_CURRENT_BINARY_DIR}/vfg_session_controller")
set_tests_properties(vfg_session_controller PROPERTIES TIMEOUT 20)
add_test(NAME vfg_session_controller_native COMMAND vfg_native_smoke_tests
  $<TARGET_FILE:obs-nr-fg-presenter> "${CMAKE_CURRENT_BINARY_DIR}/vfg_session_controller_native" --split-telemetry)
set_tests_properties(vfg_session_controller_native PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS gpu)

add_executable(vfg_log_policy_tests tools/tests/vfg_log_policy_tests.cpp)
target_compile_features(vfg_log_policy_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_log_policy_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_log_policy_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_log_policy_tests PRIVATE src/fg tools/fg-presenter)
target_link_libraries(vfg_log_policy_tests PRIVATE fg_control user32)
add_test(NAME vfg_log_policy COMMAND vfg_log_policy_tests "${CMAKE_CURRENT_BINARY_DIR}/vfg_log_policy")
set_tests_properties(vfg_log_policy PROPERTIES TIMEOUT 20)

add_library(anr_benign_fixture MODULE tools/tests/anr_benign_fixture.cpp)
target_compile_options(anr_benign_fixture PRIVATE /W4 /WX /permissive-)
add_executable(anr_validator_tests tools/tests/anr_validator_tests.cpp tools/tests/anr_validator_disk_cases.cpp)
add_dependencies(anr_validator_tests anr_benign_fixture)
target_include_directories(anr_validator_tests PRIVATE tools/fg-presenter)
target_include_directories(anr_validator_tests PRIVATE deps/ngx)
target_compile_options(anr_validator_tests PRIVATE /W4 /WX /permissive-)
target_link_libraries(anr_validator_tests PRIVATE fg_control)
add_test(NAME anr_validator COMMAND anr_validator_tests)
set_tests_properties(anr_validator PROPERTIES TIMEOUT 20)

add_library(anr_controller_dispatch_probe OBJECT src/fg/fg_controller.cpp)
target_link_libraries(anr_controller_dispatch_probe PRIVATE fg_ipc)
target_compile_options(anr_controller_dispatch_probe PRIVATE /W4 /WX /permissive-
  "/FI${CMAKE_CURRENT_SOURCE_DIR}/tools/tests/anr_controller_dispatch_probe.h")
add_executable(anr_validator_reparse_tests
  tools/tests/anr_validator_reparse_tests.cpp tools/tests/anr_controller_junction_case.cpp
  $<TARGET_OBJECTS:anr_controller_dispatch_probe>)
add_dependencies(anr_validator_reparse_tests anr_benign_fixture)
target_compile_options(anr_validator_reparse_tests PRIVATE /W4 /WX /permissive-)
target_link_libraries(anr_validator_reparse_tests PRIVATE fg_control)
add_test(NAME anr_validator_reparse COMMAND anr_validator_reparse_tests
  $<TARGET_FILE:anr_benign_fixture> "${CMAKE_CURRENT_BINARY_DIR}/anr-reparse-test")
set_tests_properties(anr_validator_reparse PROPERTIES TIMEOUT 15)

add_executable(vfg_pacing_repair_tests tools/tests/vfg_pacing_repair_tests.cpp)
target_compile_features(vfg_pacing_repair_tests PRIVATE cxx_std_20)
target_compile_options(vfg_pacing_repair_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_pacing_repair_tests PRIVATE tools/fg-presenter)
add_test(NAME vfg_pacing_repair COMMAND vfg_pacing_repair_tests)

add_executable(vfg_capture_visibility_tests tools/tests/vfg_capture_visibility_tests.cpp)
target_compile_features(vfg_capture_visibility_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_capture_visibility_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_capture_visibility_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_capture_visibility_tests PRIVATE src/fg)
target_link_libraries(vfg_capture_visibility_tests PRIVATE fg_control d3d11 dxgi user32)
add_dependencies(vfg_capture_visibility_tests obs-nr-fg-presenter)
add_test(NAME vfg_capture_visibility COMMAND vfg_capture_visibility_tests
  $<TARGET_FILE:obs-nr-fg-presenter> "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/vfg_capture_visibility")
set_tests_properties(vfg_capture_visibility PROPERTIES TIMEOUT 60)

add_executable(vfg_adaptive_fg_tests tools/tests/vfg_adaptive_fg_tests.cpp)
target_compile_features(vfg_adaptive_fg_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_adaptive_fg_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_adaptive_fg_tests PRIVATE /W4 /WX /permissive-)
add_test(NAME vfg_adaptive_fg COMMAND vfg_adaptive_fg_tests)
set_tests_properties(vfg_adaptive_fg PROPERTIES TIMEOUT 30)

add_executable(pushflow_motion_tests tools/tests/pushflow_motion_tests.cpp)
target_compile_features(pushflow_motion_tests PRIVATE cxx_std_20)
target_compile_options(pushflow_motion_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(pushflow_motion_tests PRIVATE tools/fg-presenter)
add_test(NAME pushflow_motion COMMAND pushflow_motion_tests)
set_tests_properties(pushflow_motion PROPERTIES TIMEOUT 30)

add_library(pushflow_loader_fixture MODULE tools/tests/pushflow_loader_fixture.cpp)
target_compile_options(pushflow_loader_fixture PRIVATE /W4 /WX /permissive-)
target_link_options(pushflow_loader_fixture PRIVATE /NOENTRY)

add_executable(pushflow_backend_tests
  tools/tests/pushflow_backend_tests.cpp
  tools/tests/pushflow_gpu_fixture.cpp
  tools/fg-presenter/pushflow_backend.cpp
  tools/fg-presenter/pushflow_compute.cpp
  tools/fg-presenter/pushflow_ort.cpp
  tools/fg-presenter/pushflow_shader_bytecode.cpp
  tools/fg-presenter/vfg_shot_change.cpp
  tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_gpu.cpp)
add_dependencies(pushflow_backend_tests vfg_presenter_shaders pushflow_loader_fixture)
target_compile_features(pushflow_backend_tests PRIVATE cxx_std_20)
target_compile_definitions(pushflow_backend_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pushflow_backend_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(pushflow_backend_tests PRIVATE src src/fg "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(pushflow_backend_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-1.20.1")
target_link_libraries(pushflow_backend_tests PRIVATE fg_ipc d3d12 dxgi user32)
add_test(NAME pushflow_backend COMMAND pushflow_backend_tests)
set_tests_properties(pushflow_backend PROPERTIES TIMEOUT 90)
foreach(case IN ITEMS invalid missing)
  add_test(NAME pushflow_backend_cli_${case}
    COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:pushflow_backend_tests>" "-DCASE=${case}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/tests/check_pushflow_cli_rejection.cmake")
  set_tests_properties(pushflow_backend_cli_${case} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(vfg_filter_output_tests tools/tests/vfg_filter_output_tests.cpp tools/tests/vfg_anr_fallback_case.cpp)
target_compile_features(vfg_filter_output_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_filter_output_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_filter_output_tests PRIVATE /W4 /WX /permissive-)
target_link_libraries(vfg_filter_output_tests PRIVATE OBS::libobs)
add_dependencies(vfg_filter_output_tests obs-nr-filter anr_benign_fixture)
if(NOT OBS_NR_VFG_QA_ACTION)
  add_test(NAME vfg_filter_output COMMAND vfg_filter_output_tests
    $<TARGET_FILE:obs-nr-filter> "${CMAKE_CURRENT_SOURCE_DIR}/data" $<TARGET_FILE:obs-nr-fg-presenter>
    "$<TARGET_FILE_DIR:OBS::libobs>/libobs-d3d11.dll" "${CMAKE_CURRENT_BINARY_DIR}/vfg_filter_output")
  set_tests_properties(vfg_filter_output PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:OBS::libobs>")
  add_test(NAME vfg_filter_output_properties COMMAND vfg_filter_output_tests
    $<TARGET_FILE:obs-nr-filter> "${CMAKE_CURRENT_SOURCE_DIR}/data" $<TARGET_FILE:obs-nr-fg-presenter>
    "$<TARGET_FILE_DIR:OBS::libobs>/libobs-d3d11.dll" "${CMAKE_CURRENT_BINARY_DIR}/vfg_filter_output_properties"
    properties)
  set_tests_properties(vfg_filter_output_properties PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:OBS::libobs>")
  add_test(NAME vfg_anr_missing_companion_1x COMMAND vfg_filter_output_tests
    $<TARGET_FILE:obs-nr-filter> "${CMAKE_CURRENT_SOURCE_DIR}/data" $<TARGET_FILE:obs-nr-fg-presenter>
    "$<TARGET_FILE_DIR:OBS::libobs>/libobs-d3d11.dll" "${CMAKE_CURRENT_BINARY_DIR}/vfg_anr_missing_companion_1x"
    anr-missing-companion)
  set_tests_properties(vfg_anr_missing_companion_1x PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE LABELS gpu
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:OBS::libobs>")
  add_test(NAME vfg_anr_missing_pack_1x COMMAND vfg_filter_output_tests
    $<TARGET_FILE:obs-nr-filter> "${CMAKE_CURRENT_SOURCE_DIR}/data" $<TARGET_FILE:obs-nr-fg-presenter>
    "$<TARGET_FILE_DIR:OBS::libobs>/libobs-d3d11.dll" "${CMAKE_CURRENT_BINARY_DIR}/vfg_anr_missing_pack_1x"
    anr-missing-pack)
  set_tests_properties(vfg_anr_missing_pack_1x PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE LABELS gpu
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:OBS::libobs>")
endif()
