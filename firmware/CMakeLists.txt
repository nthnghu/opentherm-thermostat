cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(thermostat)

# The build is -Og (CONFIG_COMPILER_OPTIMIZATION_DEBUG), which keeps the firmware easy to debug but makes
# drawing slow: a frame of a page change took about 41 ms. The graphics library alone is built with -O2.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -O2)
