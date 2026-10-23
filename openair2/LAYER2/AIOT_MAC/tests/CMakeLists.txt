# SPDX-License-Identifier: LicenseRef-CSSL-1.0

foreach(t link reader device)
  add_executable(test_aiot_mac_${t} test_mac_${t}.cpp)
  target_link_libraries(test_aiot_mac_${t} PRIVATE aiot_phy GTest::gtest GTest::gtest_main)
  add_test(NAME test_aiot_mac_${t} COMMAND test_aiot_mac_${t})
  if(TARGET tests)
    add_dependencies(tests test_aiot_mac_${t})
  endif()
endforeach()
