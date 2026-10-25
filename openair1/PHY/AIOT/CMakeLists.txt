# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# The Rel-19 Ambient IoT PHY (TS 38.291), and the MAC (TS 38.391: its PDUs here, its procedures and
# the Reader's protocols in openair2/LAYER2/AIOT_MAC, with the messages between nodes) as a plain C
# library.
# It builds inside OAI (openair1/PHY) or on its own:
#   cmake -S openair1/PHY/AIOT -B build-aiot -G Ninja && ninja -C build-aiot && ctest --test-dir build-aiot
# -DAIOT_BUILD_TESTS=OFF builds the library alone, without C++ or GTest.

cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(aiot_phy C)
  set(CMAKE_C_STANDARD 11)
  set(AIOT_STANDALONE ON)
  option(AIOT_BUILD_TESTS "Build the unit tests (GTest)" ON)
  if(AIOT_BUILD_TESTS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    enable_testing()
    # here, not in tests/: the MAC tests in openair2/LAYER2/AIOT_MAC/tests need GTest too
    find_package(GTest REQUIRED)
  endif()
endif()

# OAI's CRC, LTE tail-biting encoder and LTE Viterbi decoder are compiled in, so the library also
# works outside OAI. Nothing that links aiot_phy links PHY_COMMON.
set(OAI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_library(aiot_phy STATIC
  aiot_params.c
  aiot_coding.c
  aiot_r2d.c
  aiot_d2r.c
  aiot_mac_pdu.c
  ${OAI_ROOT}/openair2/LAYER2/AIOT_MAC/aiot_mac_link.c
  ${OAI_ROOT}/openair2/LAYER2/AIOT_MAC/aiot_mac_reader.c
  ${OAI_ROOT}/openair2/LAYER2/AIOT_MAC/aiot_mac_device.c
  ${OAI_ROOT}/openair2/LAYER2/AIOT_MAC/aiot_protocols.c
  ${OAI_ROOT}/openair2/LAYER2/AIOT_MAC/aiot_node_msg.c
  ${OAI_ROOT}/openair1/PHY/CODING/crc_byte.c
  ${OAI_ROOT}/openair1/PHY/CODING/ccoding_byte_lte.c
  ${OAI_ROOT}/openair1/PHY/CODING/viterbi_lte.c
)
target_include_directories(aiot_phy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${OAI_ROOT}/openair2/LAYER2/AIOT_MAC ${OAI_ROOT})
target_include_directories(aiot_phy PRIVATE ${OAI_ROOT}/openair1 ${OAI_ROOT}/common/utils ${OAI_ROOT}/common
                                            ${OAI_ROOT}/openair2/COMMON)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  # crc_byte.c calls the carry-less multiply directly. Inside OAI the rest comes from OAI's flags;
  # on its own the library targets AVX2, as OAI's Docker images do (x86-64-v3), not the build CPU.
  target_compile_options(aiot_phy PRIVATE -mpclmul)
  if(AIOT_STANDALONE)
    target_compile_options(aiot_phy PRIVATE -march=x86-64-v3 -DSIMDE_X86_PCLMUL_NATIVE)
  endif()
endif()
find_package(Threads REQUIRED)
target_link_libraries(aiot_phy PUBLIC m Threads::Threads)
set_target_properties(aiot_phy PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The same library as libaiot_phy.so, for programs that load it at run time (the Amber A-IoT
# simulator, see openair2/AMBER).
add_library(aiot_phy_shared SHARED $<TARGET_OBJECTS:aiot_phy>)
# next to amber-reader and amber-tag, which look for it there
set_target_properties(aiot_phy_shared PROPERTIES OUTPUT_NAME aiot_phy LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(aiot_phy_shared PRIVATE m Threads::Threads)

if((AIOT_STANDALONE AND AIOT_BUILD_TESTS) OR (NOT AIOT_STANDALONE AND ENABLE_TESTS))
  add_subdirectory(tests)
  add_subdirectory(${OAI_ROOT}/openair2/LAYER2/AIOT_MAC/tests ${CMAKE_CURRENT_BINARY_DIR}/mac_tests)
endif()
