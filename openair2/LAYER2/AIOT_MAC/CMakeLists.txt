# SPDX-License-Identifier: LicenseRef-CSSL-1.0

# The A-IoT Reader of an NR node (gNB or UE): the 38.391 Reader MAC on the node's R2D and D2R. The
# MAC itself (aiot_mac_*.c, aiot_protocols.c) is built into aiot_phy (openair1/PHY/AIOT).
# nr_aiot_peer.c: a UE's end of a split Reader, whose MAC runs in the gNB
add_library(nr_aiot_reader STATIC nr_aiot_reader.c nr_aiot_peer.c nr_aiot_udp.c)
target_link_libraries(nr_aiot_reader PUBLIC aiot_phy nr_aiot_r2d nr_aiot_d2r PRIVATE LOG utils)
target_include_directories(nr_aiot_reader PUBLIC ${OPENAIR1_DIR})
# the node's "aiot" configuration section: its R2D, D2R and Reader
add_library(nr_aiot_config STATIC nr_aiot_config.c)
target_link_libraries(nr_aiot_config PUBLIC nr_aiot_reader PRIVATE CONFIG_LIB LOG utils)
