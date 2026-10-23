# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_library(rfsim_hub_lib STATIC hub_room.c hub_server.c)
target_link_libraries(rfsim_hub_lib PUBLIC UTIL m)

add_executable(rfsim_hub rfsim_hub.c)
target_link_libraries(rfsim_hub PRIVATE rfsim_hub_lib CONFIG_LIB UTIL)
set_target_properties(rfsim_hub PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if (ENABLE_TESTS)
  add_subdirectory(tests)
endif()
