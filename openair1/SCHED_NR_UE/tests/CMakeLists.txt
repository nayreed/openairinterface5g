# SPDX-License-Identifier: LicenseRef-CSSL-1.0

# The UE's A-IoT R2D through the UE's own TX procedure and OFDM. nr_ue_tx.c builds the UE in C, since
# the UE's PHY headers are C only; the libraries are those of the UE's physical simulators.
add_executable(test_nr_ue_aiot_r2d test_nr_ue_aiot_r2d.cpp nr_ue_tx.c)
target_link_libraries(test_nr_ue_aiot_r2d PRIVATE
  -Wl,--start-group UTIL SIMU PHY_NR_COMMON PHY_NR PHY_NR_UE SCHED_NR_LIB SCHED_NR_UE_LIB MAC_NR_COMMON CONFIG_LIB -Wl,--end-group
  m pthread ${T_LIB} ITTI dl nr_ue_phy_meas physim_common softmodem_common minimal_lib asn1_nr_rrc_hdrs asn1_lte_rrc_hdrs GTest::gtest)
add_dependencies(test_nr_ue_aiot_r2d dfts) # the OFDM's DFT is loaded from libdfts.so
add_dependencies(tests test_nr_ue_aiot_r2d)
add_test(NAME test_nr_ue_aiot_r2d COMMAND ./test_nr_ue_aiot_r2d)
set_tests_properties(test_nr_ue_aiot_r2d PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}")
