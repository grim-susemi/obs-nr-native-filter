# Task8 leaf only. Shared steward includes this after reading the stable interface.
get_filename_component(NR_GUIDES_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_program(NR_GUIDES_FXC NAMES fxc.exe HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" REQUIRED)
set(NR_GUIDES_SHADER "${CMAKE_CURRENT_BINARY_DIR}/nr_guides.cso")
add_custom_command(OUTPUT "${NR_GUIDES_SHADER}"
 COMMAND "${NR_GUIDES_FXC}" /nologo /WX /Ges /Gis /T cs_5_0 /E main /Fo "${NR_GUIDES_SHADER}" "${NR_GUIDES_ROOT}/src/shaders/nr_guides.hlsl"
 DEPENDS "${NR_GUIDES_ROOT}/src/shaders/nr_guides.hlsl" VERBATIM)
add_custom_target(nr_guides_shaders DEPENDS "${NR_GUIDES_SHADER}")
# Shared NVOF export entry from the same source; the broker embeds it so the
# sanitize path cannot drift from the private Feature 18 guide shader.
set(NR_FLOW_EXPORT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/nr_flow_export.h")
add_custom_command(OUTPUT "${NR_FLOW_EXPORT_HEADER}"
 COMMAND "${NR_GUIDES_FXC}" /nologo /WX /Ges /Gis /T cs_5_0 /E sanitize /Fh "${NR_FLOW_EXPORT_HEADER}" /Vn nr_flow_export_shader "${NR_GUIDES_ROOT}/src/shaders/nr_guides.hlsl"
 DEPENDS "${NR_GUIDES_ROOT}/src/shaders/nr_guides.hlsl" VERBATIM)
add_custom_target(nr_flow_export_shader DEPENDS "${NR_FLOW_EXPORT_HEADER}")
add_library(nr_guides STATIC "${NR_GUIDES_ROOT}/src/nr_guides.cpp" "${NR_GUIDES_ROOT}/src/nr_flow_broker.cpp")
target_include_directories(nr_guides PUBLIC "${NR_GUIDES_ROOT}/src" "${NR_GUIDES_ROOT}/deps/nvofa")
target_include_directories(nr_guides PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(nr_guides PUBLIC cxx_std_20)
target_compile_definitions(nr_guides PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(nr_guides PRIVATE /W4 /WX /permissive-)
target_link_libraries(nr_guides PUBLIC d3d12 dxgi bcrypt crypt32 wintrust)
add_dependencies(nr_guides nr_guides_shaders nr_flow_export_shader)
add_library(nr_flow_scenario STATIC "${NR_GUIDES_ROOT}/tools/host/nr_flow_scenario.cpp")
target_link_libraries(nr_flow_scenario PUBLIC nr_guides)
target_include_directories(nr_flow_scenario PUBLIC "${NR_GUIDES_ROOT}/tools/host")
target_compile_options(nr_flow_scenario PRIVATE /W4 /WX /permissive-)
if(TARGET OBS::libobs)
  target_compile_definitions(nr_flow_scenario PRIVATE NR_FLOW_WITH_OBS)
  target_link_libraries(nr_flow_scenario PRIVATE OBS::libobs nr_interop)
endif()
add_executable(flow_contract_tests "${NR_GUIDES_ROOT}/tools/tests/flow_contract_tests.cpp")
target_link_libraries(flow_contract_tests PRIVATE nr_guides nr_flow_scenario)
target_compile_options(flow_contract_tests PRIVATE /W4 /WX /permissive-)
add_test(NAME flow_contract COMMAND flow_contract_tests)
# Opt-in device lane over the same host: the shared ring, its NT handles and the
# display mapping are exercised on the real adapter; a host without a hardware
# D3D12 adapter reports SKIP and still passes.
add_test(NAME flow_contract_gpu COMMAND flow_contract_tests --gpu)
set_tests_properties(flow_contract_gpu PROPERTIES TIMEOUT 300
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>")
