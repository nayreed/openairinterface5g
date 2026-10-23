# SPDX-License-Identifier: LicenseRef-CSSL-1.0

# The A-IoT Reader of an NR node (gNB or UE): the 38.391 Reader MAC on the node's R2D and D2R. The
# MAC itself (aiot_mac_*.c, aiot_protocols.c) is built into aiot_phy (openair1/PHY/AIOT).
add_library(nr_aiot_reader STATIC nr_aiot_reader.c)
target_link_libraries(nr_aiot_reader PUBLIC aiot_phy nr_aiot_r2d nr_aiot_d2r PRIVATE LOG)
target_include_directories(nr_aiot_reader PUBLIC ${OPENAIR1_DIR})
# its "aiot" configuration section
add_library(nr_aiot_reader_config STATIC nr_aiot_reader_config.c)
target_link_libraries(nr_aiot_reader_config PUBLIC nr_aiot_reader PRIVATE CONFIG_LIB LOG)
