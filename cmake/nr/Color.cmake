# Owned leaf; shared steward includes this after interface freeze.
get_filename_component(NR_COLOR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(nr_color STATIC "${NR_COLOR_ROOT}/src/nr_color.cpp")
target_include_directories(nr_color PUBLIC "${NR_COLOR_ROOT}/src")
target_compile_features(nr_color PUBLIC cxx_std_20)
target_compile_definitions(nr_color PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(nr_color PRIVATE /W4 /WX /permissive-)
target_link_libraries(nr_color PUBLIC d3d12 dxgi)
add_executable(color_contract_tests "${NR_COLOR_ROOT}/tools/tests/color_contract_tests.cpp")
target_link_libraries(color_contract_tests PRIVATE nr_color)
target_compile_options(color_contract_tests PRIVATE /W4 /WX /permissive-)
add_test(NAME color_contract COMMAND color_contract_tests)
add_library(nr_color_scenario STATIC "${NR_COLOR_ROOT}/tools/host/nr_color_scenario.cpp")
target_include_directories(nr_color_scenario PUBLIC "${NR_COLOR_ROOT}/tools/host")
target_link_libraries(nr_color_scenario PUBLIC nr_color)
target_compile_options(nr_color_scenario PRIVATE /W4 /WX /permissive-)
if(TARGET OBS::libobs)
  target_compile_definitions(nr_color_scenario PRIVATE NR_COLOR_WITH_OBS)
  target_link_libraries(nr_color_scenario PRIVATE OBS::libobs nr_interop)
endif()
add_executable(color_software_tests "${NR_COLOR_ROOT}/tools/tests/color_software_tests.cpp"
  "${NR_COLOR_ROOT}/tools/host/nr_color_scenario.cpp")
target_link_libraries(color_software_tests PRIVATE nr_color)
target_include_directories(color_software_tests PRIVATE "${NR_COLOR_ROOT}/tools/host")
target_compile_options(color_software_tests PRIVATE /W4 /WX /permissive-)
find_program(NR_COLOR_FXC NAMES fxc.exe HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" REQUIRED)
find_program(NR_COLOR_DXC NAMES dxc.exe HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" REQUIRED)
set(NR_COLOR_DXBC "${CMAKE_CURRENT_BINARY_DIR}/nr_color.cso")
set(NR_COLOR_DXIL "${CMAKE_CURRENT_BINARY_DIR}/nr_color.dxil")
add_custom_command(OUTPUT "${NR_COLOR_DXBC}" "${NR_COLOR_DXIL}"
  COMMAND "${NR_COLOR_FXC}" /nologo /WX /Ges /Gis /T cs_5_0 /E main /Fo "${NR_COLOR_DXBC}" "${NR_COLOR_ROOT}/src/shaders/nr_color.hlsl"
  COMMAND "${NR_COLOR_DXC}" -WX -Ges -Gis -T cs_6_0 -E main -Fo "${NR_COLOR_DXIL}" "${NR_COLOR_ROOT}/src/shaders/nr_color.hlsl"
  DEPENDS "${NR_COLOR_ROOT}/src/shaders/nr_color.hlsl" VERBATIM)
add_custom_target(nr_color_shaders DEPENDS "${NR_COLOR_DXBC}" "${NR_COLOR_DXIL}")
add_dependencies(color_software_tests nr_color_shaders)
# DXIL is the actual D3D12 production shader. FXC bytecode is a strict compile
# audit only: retained WARP receipts show its double lowering is not admitted.
add_test(NAME color_software COMMAND color_software_tests "${NR_COLOR_DXIL}")
