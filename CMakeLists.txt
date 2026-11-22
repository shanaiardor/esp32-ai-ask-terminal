cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai_ask_terminal)

esptool_py_flash_to_partition(flash "font_dat" "${CMAKE_SOURCE_DIR}/fonts_bin/font_cn_16.bin")
