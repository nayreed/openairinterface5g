# SPDX-License-Identifier: LicenseRef-CSSL-1.0

# The gNB as an Ambient IoT Reader: R2D on the gNB's downlink grid.
add_library(nr_aiot_r2d STATIC nr_aiot_r2d.c)
target_link_libraries(nr_aiot_r2d PUBLIC aiot_phy PRIVATE LOG)

if (ENABLE_TESTS)
  add_subdirectory(tests)
endif()
