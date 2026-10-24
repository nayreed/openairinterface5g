# SPDX-License-Identifier: LicenseRef-CSSL-1.0

foreach(t link reader device node_msg)
  add_executable(test_aiot_mac_${t} test_mac_${t}.cpp)
  target_link_libraries(test_aiot_mac_${t} PRIVATE aiot_phy GTest::gtest GTest::gtest_main)
  add_test(NAME test_aiot_mac_${t} COMMAND test_aiot_mac_${t})
  if(TARGET tests)
    add_dependencies(tests test_aiot_mac_${t})
  endif()
endforeach()

# The Reader of an NR node needs OAI's LOG, so it is not part of aiot_phy's standalone build.
if(NOT AIOT_STANDALONE)
  add_executable(test_nr_aiot_reader test_nr_aiot_reader.cpp)
  target_link_libraries(test_nr_aiot_reader PRIVATE nr_aiot_reader nr_aiot_config GTest::gtest LOG minimal_lib)
  if(TARGET tests)
    add_dependencies(tests test_nr_aiot_reader)
  endif()
  add_test(NAME test_nr_aiot_reader COMMAND ./test_nr_aiot_reader)
endif()
