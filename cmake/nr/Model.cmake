set(nr_compare_source "${CMAKE_CURRENT_LIST_DIR}/../../src/shaders/nr_dedup_compare.hlsl")
set(nr_compare_header "${CMAKE_CURRENT_BINARY_DIR}/nr_dedup_compare.h")
add_custom_command(OUTPUT "${nr_compare_header}"
  COMMAND "${NR_INTEROP_FXC}" /nologo /WX /Ges /T cs_5_1 /E main /Fh "${nr_compare_header}"
          /Vn nr_dedup_compare "${nr_compare_source}" DEPENDS "${nr_compare_source}" VERBATIM)
add_custom_target(nr_dedup_shader DEPENDS "${nr_compare_header}")
add_library(nr_model STATIC "${CMAKE_CURRENT_LIST_DIR}/../../src/nr_model.cpp")
add_dependencies(nr_model nr_dedup_shader)
target_include_directories(nr_model PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# The subscription contract compiles nr_model.cpp directly to inspect the real
# pending owner. Give just that translation unit the identical generated input.
cmake_language(DEFER CALL add_dependencies model_subscription_tests nr_dedup_shader)
cmake_language(DEFER CALL target_include_directories model_subscription_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(nr_model PUBLIC cxx_std_20)
target_compile_options(nr_model PRIVATE /W4 /WX /permissive-)
target_link_libraries(nr_model PUBLIC nr_runtime nr_core nr_guides nr_color nr_interop nr_scheduler)
add_executable(model_contract_tests "${CMAKE_CURRENT_LIST_DIR}/../../tools/tests/model_contract_tests.cpp")
target_link_libraries(model_contract_tests PRIVATE nr_scheduler)
target_compile_options(model_contract_tests PRIVATE /W4 /WX /permissive-)
add_test(NAME model_worker_retirement COMMAND model_contract_tests)
set_tests_properties(model_worker_retirement PROPERTIES TIMEOUT 10)
add_executable(model_passes_tests "${CMAKE_CURRENT_LIST_DIR}/../../tools/tests/model_passes_tests.cpp")
target_include_directories(model_passes_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
target_compile_features(model_passes_tests PRIVATE cxx_std_20)
target_compile_options(model_passes_tests PRIVATE /W4 /WX /permissive-)
add_test(NAME model_contract COMMAND model_passes_tests)
add_executable(native_work_guard_tests "${CMAKE_CURRENT_LIST_DIR}/../../tools/tests/native_work_guard_tests.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/obs_native_output.cpp")
target_link_libraries(native_work_guard_tests PRIVATE nr_model nr_lifecycle OBS::libobs)
target_compile_options(native_work_guard_tests PRIVATE /W4 /WX /permissive-)
add_test(NAME model_work_guard COMMAND native_work_guard_tests)
set_tests_properties(model_work_guard PROPERTIES TIMEOUT 30
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>")

add_executable(nr_model_pipeline_tests "${CMAKE_CURRENT_LIST_DIR}/../../tools/tests/nr_model_pipeline_tests.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/obs_native_output.cpp")
target_link_libraries(nr_model_pipeline_tests PRIVATE nr_model nr_lifecycle OBS::libobs)
target_compile_options(nr_model_pipeline_tests PRIVATE /W4 /WX /permissive-)
add_dependencies(nr_model_pipeline_tests nr_color_shaders nr_obs_analysis_shader)
add_test(NAME nr_model_pipeline COMMAND nr_model_pipeline_tests)
set_tests_properties(nr_model_pipeline PROPERTIES TIMEOUT 120 RESOURCE_LOCK nr_gpu
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>")
