# SPDX-License-Identifier: GPL-3.0-only
add_library(fg_control STATIC src/fg/fg_controller.cpp src/fg/anr_dll_validator.cpp)
target_link_libraries(fg_control PUBLIC fg_ipc PRIVATE bcrypt)
target_compile_options(fg_control PRIVATE /W4 /WX /permissive-)
target_sources(obs-nr-filter PRIVATE src/fg/fg_tap.cpp)
target_link_libraries(obs-nr-filter PRIVATE fg_control)
option(OBS_NR_VFG_QA_ACTION "Enable the isolated OBS VFG QA settings action" OFF)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(obs-nr-filter PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()

add_executable(obs-nr-fg-presenter
  tools/fg-presenter/vfg_presenter.cpp
  tools/fg-presenter/vfg_gpu.cpp
  tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp
  tools/fg-presenter/native_vfg.cpp
  tools/fg-presenter/pushflow_backend.cpp
  tools/fg-presenter/pushflow_compute.cpp
  tools/fg-presenter/pushflow_ort.cpp
  tools/fg-presenter/pushflow_shader_bytecode.cpp
  src/fg/anr_dll_validator.cpp)
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
add_custom_target(vfg_presenter_shaders DEPENDS
  ${ANR_BGRA_HEADERS}
  "${VFG_CONTENT_HEADER}" "${VFG_SHOT_HEADER}" "${PUSHFLOW_PACK_HEADER}"
  "${PUSHFLOW_UNPACK_HEADER}" "${PUSHFLOW_RECONSTRUCT_HEADER}"
  "${CMAKE_CURRENT_BINARY_DIR}/vfg_hud_lock_update_shader.h"
  "${CMAKE_CURRENT_BINARY_DIR}/vfg_hud_lock_compose_shader.h")
target_sources(obs-nr-fg-presenter PRIVATE "${VFG_CONTENT_HEADER}"
  "${PUSHFLOW_PACK_HEADER}" "${PUSHFLOW_UNPACK_HEADER}" "${PUSHFLOW_RECONSTRUCT_HEADER}")
add_dependencies(obs-nr-fg-presenter vfg_presenter_shaders nr_guides_shaders)
set(ANR_BACKEND_SOURCES
  tools/fg-presenter/anr_fg_backend.cpp tools/fg-presenter/anr_commands.cpp
  tools/fg-presenter/anr_guides.cpp tools/fg-presenter/anr_runtime.cpp)
target_sources(obs-nr-fg-presenter PRIVATE ${ANR_BACKEND_SOURCES})
target_include_directories(obs-nr-fg-presenter SYSTEM PRIVATE deps/ngx)
target_link_libraries(obs-nr-fg-presenter PRIVATE nr_guides)
target_compile_features(obs-nr-fg-presenter PRIVATE cxx_std_20)
target_compile_definitions(obs-nr-fg-presenter PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(obs-nr-fg-presenter PRIVATE /W4 /WX /permissive-)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(obs-nr-fg-presenter PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()
target_include_directories(obs-nr-fg-presenter PRIVATE src src/fg "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(obs-nr-fg-presenter SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-1.20.1")
target_compile_definitions(obs-nr-fg-presenter PRIVATE OBS_NR_HAVE_ONNXRUNTIME=1)
target_link_libraries(obs-nr-fg-presenter PRIVATE fg_ipc d3d12 dxgi user32 winmm bcrypt)
add_dependencies(obs-nr-filter obs-nr-fg-presenter)

add_library(anr_mock_dll MODULE tools/tests/anr_mock_dll.cpp)
target_compile_features(anr_mock_dll PRIVATE cxx_std_20)
target_compile_options(anr_mock_dll PRIVATE /W4 /WX /permissive-)
target_compile_definitions(anr_mock_dll PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(anr_mock_dll PRIVATE tools/fg-presenter)
target_include_directories(anr_mock_dll SYSTEM PRIVATE deps/ngx)
add_executable(anr_mock_dll_tests tools/tests/anr_mock_dll_tests.cpp tools/tests/anr_mock_ownership.cpp ${ANR_BACKEND_SOURCES}
  tools/fg-presenter/vfg_gpu.cpp tools/fg-presenter/vfg_hud_lock.cpp
  tools/fg-presenter/vfg_shot_change.cpp tools/fg-presenter/pushflow_shader_bytecode.cpp)
target_compile_options(anr_mock_dll_tests PRIVATE /W4 /WX /permissive-)
target_compile_definitions(anr_mock_dll_tests PRIVATE OBS_NR_ANR_MOCK_TEST)
target_include_directories(anr_mock_dll_tests PRIVATE tools/fg-presenter src/fg "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(anr_mock_dll_tests SYSTEM PRIVATE deps/ngx)
target_link_libraries(anr_mock_dll_tests PRIVATE fg_control nr_guides user32 d3d12 dxgi)
add_dependencies(anr_mock_dll_tests anr_mock_dll vfg_presenter_shaders nr_guides_shaders)
add_custom_command(TARGET anr_mock_dll_tests POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${NR_GUIDES_SHADER}" "$<TARGET_FILE_DIR:anr_mock_dll_tests>/nr_guides.cso"
  VERBATIM)
add_test(NAME anr_mock_dll COMMAND anr_mock_dll_tests)
set_tests_properties(anr_mock_dll PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS GPU
  ENVIRONMENT "OBS_NR_GUIDES_SHADER=${NR_GUIDES_SHADER}")

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

add_executable(vfg_monitor_text_tests tools/tests/vfg_monitor_text_tests.cpp)
target_compile_features(vfg_monitor_text_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_monitor_text_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vfg_monitor_text_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(vfg_monitor_text_tests PRIVATE src/fg)
add_test(NAME vfg_monitor_text COMMAND vfg_monitor_text_tests)

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

add_library(anr_benign_fixture MODULE tools/tests/anr_benign_fixture.cpp)
target_compile_options(anr_benign_fixture PRIVATE /W4 /WX /permissive-)
add_executable(anr_validator_tests tools/tests/anr_validator_tests.cpp tools/tests/anr_validator_disk_cases.cpp)
add_dependencies(anr_validator_tests anr_benign_fixture)
target_include_directories(anr_validator_tests PRIVATE tools/fg-presenter)
target_include_directories(anr_validator_tests SYSTEM PRIVATE deps/ngx)
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
target_include_directories(pushflow_backend_tests SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps/onnxruntime-1.20.1")
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
