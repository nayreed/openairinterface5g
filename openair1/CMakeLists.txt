# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_subdirectory(PHY)
add_subdirectory(SIMULATION)
if (ENABLE_TESTS)
  add_subdirectory(SCHED_NR_UE/tests)
endif()
