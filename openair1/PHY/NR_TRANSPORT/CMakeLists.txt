# SPDX-License-Identifier: LicenseRef-CSSL-1.0

# The gNB as an Ambient IoT Reader: R2D on the gNB's downlink grid, D2R from its uplink samples.
add_library(nr_aiot_r2d STATIC nr_aiot_r2d.c)
target_link_libraries(nr_aiot_r2d PUBLIC aiot_phy PRIVATE LOG utils)
add_library(nr_aiot_d2r STATIC nr_aiot_d2r.c)
target_link_libraries(nr_aiot_d2r PUBLIC aiot_phy PRIVATE LOG utils nr_aiot_r2d)

if (ENABLE_TESTS)
  add_subdirectory(tests)
endif()
