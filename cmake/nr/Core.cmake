# Leaf only: central owner includes this file; no OBS/plugin edits here.
get_filename_component(NR_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(nr_core STATIC "${NR_CORE_ROOT}/src/nr_core.cpp")
target_include_directories(nr_core PUBLIC "${NR_CORE_ROOT}/src")
target_compile_features(nr_core PUBLIC cxx_std_20)
target_compile_definitions(nr_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(nr_core PRIVATE /W4 /WX /permissive-)
target_link_libraries(nr_core PUBLIC d3d12 dxgi)
add_executable(nr_core_tests "${NR_CORE_ROOT}/tools/tests/nr_core_tests.cpp")
target_link_libraries(nr_core_tests PRIVATE nr_core d3d11 d3dcompiler)
target_compile_options(nr_core_tests PRIVATE /W4 /WX /permissive-)
add_test(NAME nr_core COMMAND nr_core_tests)
add_executable(nr_core_multipass_tests "${NR_CORE_ROOT}/tools/tests/nr_core_multipass_tests.cpp")
target_link_libraries(nr_core_multipass_tests PRIVATE nr_core)
target_compile_options(nr_core_multipass_tests PRIVATE /W4 /WX /permissive-)
add_test(NAME nr_core_multipass COMMAND nr_core_multipass_tests)
find_program(NR_FXC NAMES fxc.exe HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" REQUIRED)
find_program(NR_DXC NAMES dxc.exe HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" REQUIRED)
set(NR_CODEC_DXBC "${CMAKE_CURRENT_BINARY_DIR}/nr_codec.cso")
set(NR_CODEC_DXIL "${CMAKE_CURRENT_BINARY_DIR}/nr_codec.dxil")
add_custom_command(OUTPUT "${NR_CODEC_DXBC}" "${NR_CODEC_DXIL}"
  COMMAND "${NR_FXC}" /nologo /WX /Ges /Gis /T cs_5_0 /E main /Fo "${NR_CODEC_DXBC}" "${NR_CORE_ROOT}/src/shaders/nr_codec.hlsl"
  COMMAND "${NR_DXC}" -WX -Ges -Gis -T cs_6_0 -E main -Fo "${NR_CODEC_DXIL}" "${NR_CORE_ROOT}/src/shaders/nr_codec.hlsl"
  DEPENDS "${NR_CORE_ROOT}/src/shaders/nr_codec.hlsl" VERBATIM)
add_custom_target(nr_core_shaders DEPENDS "${NR_CODEC_DXBC}" "${NR_CODEC_DXIL}")
add_dependencies(nr_core nr_core_shaders)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nr_core_fake_model.dxil"
  COMMAND "${NR_DXC}" -WX -Ges -Gis -T cs_6_0 -E main
    -Fo "${CMAKE_CURRENT_BINARY_DIR}/nr_core_fake_model.dxil" "${NR_CORE_ROOT}/tools/tests/nr_core_fake_model.hlsl"
  DEPENDS "${NR_CORE_ROOT}/tools/tests/nr_core_fake_model.hlsl" VERBATIM)
add_custom_target(nr_core_fake_model_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/nr_core_fake_model.dxil")
add_executable(nr_core_multipass_gpu_tests "${NR_CORE_ROOT}/tools/tests/nr_core_multipass_gpu_tests.cpp")
target_link_libraries(nr_core_multipass_gpu_tests PRIVATE nr_core)
target_compile_options(nr_core_multipass_gpu_tests PRIVATE /W4 /WX /permissive-)
add_dependencies(nr_core_multipass_gpu_tests nr_core_fake_model_shader)
