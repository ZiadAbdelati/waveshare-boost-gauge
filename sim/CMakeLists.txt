cmake_minimum_required(VERSION 3.16)
project(boost_gauge_sim C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LVGL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl")
if(NOT EXISTS "${LVGL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "LVGL not found at ${LVGL_ROOT}. Build the ESP-IDF project once so managed_components are fetched.")
endif()

# Point LVGL at our host config before adding the subdirectory.
set(LV_BUILD_CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

add_subdirectory(${LVGL_ROOT} lvgl_build)

# LVGL's desktop cmake globs *.S (Helium/Neon). Drop them on x86 hosts.
get_target_property(_lvgl_sources lvgl SOURCES)
if(_lvgl_sources)
    list(FILTER _lvgl_sources EXCLUDE REGEX "\\.(S|s)$")
    set_property(TARGET lvgl PROPERTY SOURCES ${_lvgl_sources})
endif()

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

add_executable(boost_gauge_sim
    main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/boost_gauge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/boost_sim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/boost_brightness.c
)

target_include_directories(boost_gauge_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${LVGL_ROOT}
    ${LVGL_ROOT}/src
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(boost_gauge_sim PRIVATE
    LV_CONF_INCLUDE_SIMPLE
)

target_link_libraries(boost_gauge_sim PRIVATE
    lvgl
    ${SDL2_LIBRARIES}
    Threads::Threads
    m
)

if(TARGET SDL2::SDL2)
    target_link_libraries(boost_gauge_sim PRIVATE SDL2::SDL2)
endif()

add_custom_target(sim-screenshots
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/../preview/sim
    COMMAND $<TARGET_FILE:boost_gauge_sim> --screenshot ${CMAKE_SOURCE_DIR}/../preview/sim
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/raw_to_png.py ${CMAKE_SOURCE_DIR}/../preview/sim
    DEPENDS boost_gauge_sim
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Render headless LVGL snapshots and convert to PNG"
)
