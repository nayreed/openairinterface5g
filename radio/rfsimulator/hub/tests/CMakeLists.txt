# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_executable(test_rfsim_hub test_rfsim_hub.cpp)
target_link_libraries(test_rfsim_hub PRIVATE rfsim_hub_lib rfsimulator_static UTIL CONFIG_LIB GTest::gtest dl shlib_loader)
add_dependencies(tests test_rfsim_hub)
add_test(NAME test_rfsim_hub COMMAND ./test_rfsim_hub)
set_tests_properties(test_rfsim_hub PROPERTIES TIMEOUT 60)
