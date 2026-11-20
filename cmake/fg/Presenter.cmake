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
target_compile_features(obs-nr-fg-presenter PRIVATE cxx_std_20)
target_compile_definitions(obs-nr-fg-presenter PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(obs-nr-fg-presenter PRIVATE /W4 /WX /permissive-)
if(OBS_NR_VFG_QA_ACTION)
  target_compile_definitions(obs-nr-fg-presenter PRIVATE OBS_NR_VFG_QA_ACTION=1)
endif()
target_include_directories(obs-nr-fg-presenter PRIVATE src src/fg)
target_link_libraries(obs-nr-fg-presenter PRIVATE fg_ipc d3d12 dxgi user32 winmm)
add_dependencies(obs-nr-filter obs-nr-fg-presenter)

add_executable(vfg_native_smoke_tests
  tools/tests/vfg_native_smoke_tests.cpp
  src/fg/fg_controller.cpp)
target_compile_features(vfg_native_smoke_tests PRIVATE cxx_std_20)
target_compile_definitions(vfg_native_smoke_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
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
