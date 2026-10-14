# SPDX-License-Identifier: LicenseRef-CSSL-1.0

find_package(Python3 REQUIRED COMPONENTS Development.Embed)

add_library(amber_host STATIC amber_host.c)
target_link_libraries(amber_host PUBLIC Python3::Python CONFIG_LIB UTIL shlib_loader m)
# librfsimulator.so, loaded at run time, takes these from the executable; nothing else in the
# Amber programs uses them, so make the linker keep them.
target_link_options(amber_host INTERFACE
                    "LINKER:--undefined=cpu_meas_enabled"
                    "LINKER:--undefined=insert_in_time_stats_sorted_list"
                    "LINKER:--undefined=reset_time_stats_sorted_list")

