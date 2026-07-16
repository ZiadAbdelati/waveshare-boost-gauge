# Boost Gauge — ESP32-S3-Touch-AMOLED-1.75
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-format -Wno-error=attributes)
project(boost_gauge)
