# Boost Gauge — ESP32-S3-Touch-AMOLED-1.75
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-format -Wno-error=attributes)
project(boost_gauge)

# The LVGL GIF widget and its bundled AnimatedGIF decoder are vendored into
# main/gif/ (project-owned, so the optimisations and the framebuffer zero-init
# survive a dependency refresh; managed_components/ is git-ignored and is
# rewritten by `idf.py reconfigure`). Drop the upstream copies from the LVGL
# component so ours are the ones that build - they define the same symbols, and
# a mismatch here fails loudly at link time with duplicate definitions rather
# than silently reverting the optimisations.
idf_component_get_property(_lvgl_lib lvgl__lvgl COMPONENT_LIB)
get_target_property(_lvgl_srcs ${_lvgl_lib} SOURCES)
list(FILTER _lvgl_srcs EXCLUDE REGEX "/src/libs/gif/")
set_target_properties(${_lvgl_lib} PROPERTIES SOURCES "${_lvgl_srcs}")
