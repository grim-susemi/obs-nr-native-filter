# SPDX-License-Identifier: GPL-3.0-only
target_sources(obs-nr-filter PRIVATE src/fg/fg_tap.cpp src/fg/fg_controller.cpp)
target_link_libraries(obs-nr-filter PRIVATE fg_ipc)
option(OBS_NR_VFG_QA_ACTION "Enable the isolated OBS VFG QA settings action" OFF)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(obs-nr-filter PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()

add_executable(obs-nr-fg-presenter
  tools/fg-presenter/vfg_presenter.cpp
  tools/fg-presenter/vfg_gpu.cpp
  tools/fg-presenter/vfg_shot_change.cpp
  tools/fg-presenter/native_vfg.cpp
  tools/fg-presenter/pushflow_shader_bytecode.cpp)
set(VFG_SHOT_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/vfg_shot_change.hlsl")
set(VFG_SHOT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vfg_shot_change_shader.h")
add_custom_command(OUTPUT "${VFG_SHOT_HEADER}"
  COMMAND "${NR_COLOR_DXC}" -WX -Ges -T cs_6_0 -E main -Fh "${VFG_SHOT_HEADER}"
    -Vn vfg_shot_change_shader "${VFG_SHOT_SHADER}"
  DEPENDS "${VFG_SHOT_SHADER}" VERBATIM)
set(VFG_CONTENT_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/vfg_content_compare.hlsl")
set(VFG_CONTENT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vfg_content_compare_shader.h")
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
add_custom_target(vfg_presenter_shaders DEPENDS
  "${VFG_CONTENT_HEADER}" "${VFG_SHOT_HEADER}" "${PUSHFLOW_PACK_HEADER}"
  "${PUSHFLOW_UNPACK_HEADER}" "${PUSHFLOW_RECONSTRUCT_HEADER}")
target_sources(obs-nr-fg-presenter PRIVATE "${VFG_CONTENT_HEADER}"
  "${PUSHFLOW_PACK_HEADER}" "${PUSHFLOW_UNPACK_HEADER}" "${PUSHFLOW_RECONSTRUCT_HEADER}")
add_dependencies(obs-nr-fg-presenter vfg_presenter_shaders)
target_compile_features(obs-nr-fg-presenter PRIVATE cxx_std_20)
target_compile_definitions(obs-nr-fg-presenter PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(obs-nr-fg-presenter PRIVATE /W4 /WX /permissive-)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(obs-nr-fg-presenter PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()
target_include_directories(obs-nr-fg-presenter PRIVATE src src/fg "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(obs-nr-fg-presenter SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-1.20.1")
target_compile_definitions(obs-nr-fg-presenter PRIVATE OBS_NR_HAVE_ONNXRUNTIME=1)
target_link_libraries(obs-nr-fg-presenter PRIVATE fg_ipc d3d12 dxgi user32 winmm)
add_dependencies(obs-nr-filter obs-nr-fg-presenter)

add_executable(vfg_shot_change_gpu_tests
  tools/tests/vfg_shot_change_gpu_tests.cpp
  tools/fg-presenter/vfg_gpu.cpp
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

add_executable(vfg_native_smoke_tests
  tools/tests/vfg_native_smoke_tests.cpp
  src/fg/fg_controller.cpp)
target_compile_features(vfg_native_smoke_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_native_smoke_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(vfg_native_smoke_tests PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()
target_compile_options(vfg_native_smoke_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_native_smoke_tests PRIVATE src/fg)
target_link_libraries(vfg_native_smoke_tests PRIVATE fg_ipc d3d11 dxgi user32)
add_dependencies(vfg_native_smoke_tests obs-nr-fg-presenter)

add_executable(vfg_monitor_text_tests tools/tests/vfg_monitor_text_tests.cpp)
target_compile_features(vfg_monitor_text_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_monitor_text_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_monitor_text_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_monitor_text_tests PRIVATE src/fg)
add_test(NAME vfg_monitor_text COMMAND vfg_monitor_text_tests)

add_executable(vfg_session_controller_tests
  tools/tests/vfg_session_controller_tests.cpp
  src/fg/fg_controller.cpp)
target_compile_features(vfg_session_controller_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_session_controller_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_session_controller_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_session_controller_tests PRIVATE src/fg)
target_link_libraries(vfg_session_controller_tests PRIVATE fg_ipc user32)
add_dependencies(vfg_session_controller_tests vfg_native_smoke_tests)
add_test(NAME vfg_session_controller COMMAND vfg_session_controller_tests
  $<TARGET_FILE:obs-nr-fg-presenter> "${CMAKE_CURRENT_BINARY_DIR}/vfg_session_controller")
set_tests_properties(vfg_session_controller PROPERTIES TIMEOUT 20)
add_test(NAME vfg_session_controller_native COMMAND vfg_native_smoke_tests
  $<TARGET_FILE:obs-nr-fg-presenter> "${CMAKE_CURRENT_BINARY_DIR}/vfg_session_controller_native" --split-telemetry)
set_tests_properties(vfg_session_controller_native PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS gpu)

add_executable(vfg_pacing_repair_tests tools/tests/vfg_pacing_repair_tests.cpp)
target_compile_features(vfg_pacing_repair_tests PRIVATE cxx_std_20)
target_compile_options(vfg_pacing_repair_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_pacing_repair_tests PRIVATE tools/fg-presenter)
add_test(NAME vfg_pacing_repair COMMAND vfg_pacing_repair_tests)

add_executable(vfg_capture_visibility_tests
  tools/tests/vfg_capture_visibility_tests.cpp
  src/fg/fg_controller.cpp)
target_compile_features(vfg_capture_visibility_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_capture_visibility_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_capture_visibility_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_capture_visibility_tests PRIVATE src/fg)
target_link_libraries(vfg_capture_visibility_tests PRIVATE fg_ipc d3d11 dxgi user32)
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

add_executable(vfg_filter_output_tests tools/tests/vfg_filter_output_tests.cpp)
target_compile_features(vfg_filter_output_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_filter_output_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_filter_output_tests PRIVATE /W4 /WX /permissive-)
target_link_libraries(vfg_filter_output_tests PRIVATE OBS::libobs)
add_dependencies(vfg_filter_output_tests obs-nr-filter)
if(NOT OBS_NR_VFG_QA_ACTION)
  add_test(NAME vfg_filter_output COMMAND vfg_filter_output_tests
    $<TARGET_FILE:obs-nr-filter> "${CMAKE_CURRENT_SOURCE_DIR}/data" $<TARGET_FILE:obs-nr-fg-presenter>
    "$<TARGET_FILE_DIR:OBS::libobs>/libobs-d3d11.dll" "${CMAKE_CURRENT_BINARY_DIR}/vfg_filter_output")
  set_tests_properties(vfg_filter_output PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:OBS::libobs>")
endif()
