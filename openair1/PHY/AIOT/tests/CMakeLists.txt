# SPDX-License-Identifier: LicenseRef-CSSL-1.0

if(AIOT_STANDALONE)
  find_package(GTest REQUIRED)
endif()

foreach(t coding r2d d2r mac_pdu)
  add_executable(test_aiot_${t} test_${t}.cpp)
  target_link_libraries(test_aiot_${t} PRIVATE aiot_phy GTest::gtest GTest::gtest_main)
  add_test(NAME test_aiot_${t} COMMAND test_aiot_${t})
  if(TARGET tests)
    add_dependencies(tests test_aiot_${t})
  endif()
endforeach()
