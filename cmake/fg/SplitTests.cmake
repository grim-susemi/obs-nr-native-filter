# SPDX-License-Identifier: GPL-3.0-only
add_executable(split_filter_output_tests tools/tests/split_filter_output_tests.cpp)
target_compile_features(split_filter_output_tests PRIVATE cxx_std_20)
target_compile_definitions(split_filter_output_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(split_filter_output_tests PRIVATE /W4 /WX /permissive-)
target_link_libraries(split_filter_output_tests PRIVATE OBS::libobs)
add_dependencies(split_filter_output_tests obs-nr-filter)
add_test(NAME split_filter_output COMMAND split_filter_output_tests
  $<TARGET_FILE:obs-nr-filter> "${CMAKE_CURRENT_SOURCE_DIR}/data"
  "$<TARGET_FILE_DIR:OBS::libobs>/libobs-d3d11.dll" "${CMAKE_CURRENT_BINARY_DIR}/split_filter_output")
set_tests_properties(split_filter_output PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:OBS::libobs>" LABELS "gpu")

add_executable(split_presenter_tests tools/tests/split_presenter_tests.cpp src/fg/fg_controller.cpp)
target_compile_features(split_presenter_tests PRIVATE cxx_std_20)
target_compile_definitions(split_presenter_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(split_presenter_tests PRIVATE /W4 /WX /permissive-)
target_include_directories(split_presenter_tests PRIVATE src/fg)
target_link_libraries(split_presenter_tests PRIVATE fg_ipc d3d11 dxgi user32 gdi32)
add_dependencies(split_presenter_tests obs-nr-fg-presenter)
add_test(NAME split_presenter COMMAND split_presenter_tests
  $<TARGET_FILE:obs-nr-fg-presenter> "${CMAKE_CURRENT_BINARY_DIR}/split_presenter")
set_tests_properties(split_presenter PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu")
