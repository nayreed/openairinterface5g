# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_executable(test_nr_aiot_r2d test_nr_aiot_r2d.cpp)
target_link_libraries(test_nr_aiot_r2d PRIVATE nr_aiot_r2d GTest::gtest LOG minimal_lib)
add_dependencies(tests test_nr_aiot_r2d)
add_test(NAME test_nr_aiot_r2d COMMAND ./test_nr_aiot_r2d)

add_executable(test_nr_aiot_d2r test_nr_aiot_d2r.cpp)
target_link_libraries(test_nr_aiot_d2r PRIVATE nr_aiot_d2r GTest::gtest LOG minimal_lib)
add_dependencies(tests test_nr_aiot_d2r)
add_test(NAME test_nr_aiot_d2r COMMAND ./test_nr_aiot_d2r)
