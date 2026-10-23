# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_subdirectory(nr_phy_common)
add_subdirectory(TOOLS)
add_subdirectory(CODING)
add_subdirectory(MODULATION)
add_subdirectory(AIOT)
add_subdirectory(NR_TRANSPORT)
if (ENABLE_TESTS)
  add_subdirectory(INIT/tests)
endif()
