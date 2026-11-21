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
  tools/fg-presenter/native_vfg.cpp)
set(VFG_CONTENT_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/tools/fg-presenter/vfg_content_compare.hlsl")
set(VFG_CONTENT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vfg_content_compare_shader.h")
add_custom_command(OUTPUT "${VFG_CONTENT_HEADER}"
  COMMAND "${NR_FXC}" /nologo /WX /Ges /T cs_5_0 /E main /Fh "${VFG_CONTENT_HEADER}"
    /Vn vfg_content_shader "${VFG_CONTENT_SHADER}"
  DEPENDS "${VFG_CONTENT_SHADER}" VERBATIM)
target_sources(obs-nr-fg-presenter PRIVATE "${VFG_CONTENT_HEADER}")
target_compile_features(obs-nr-fg-presenter PRIVATE cxx_std_20)
target_compile_definitions(obs-nr-fg-presenter PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(obs-nr-fg-presenter PRIVATE /W4 /WX /permissive-)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(obs-nr-fg-presenter PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()
target_include_directories(obs-nr-fg-presenter PRIVATE src src/fg "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(obs-nr-fg-presenter PRIVATE fg_ipc d3d12 dxgi user32 winmm)
add_dependencies(obs-nr-filter obs-nr-fg-presenter)

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
  set_tests_properties(vfg_filter_output PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:OBS::libobs>")
endif()
