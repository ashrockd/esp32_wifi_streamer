# This is an ESP-ADF project. Before configuring it, export both ESP-IDF and
# ESP-ADF environments so that ADF_PATH is defined.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ADF_PATH})
    message(FATAL_ERROR "ADF_PATH is not set. Install/export ESP-ADF first; see README.md")
endif()

include($ENV{ADF_PATH}/CMakeLists.txt)
set(EXTRA_COMPONENT_DIRS "$ENV{ADF_PATH}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# See ../esp32_bt_speaker (the companion project this one feeds over I2S) and
# CMakeLists.txt in the original single-chip project this pair replaced -
# same MINIMAL_BUILD rationale: only build what main/'s declared REQUIRES
# actually need, not ESP-ADF's entire component tree.
if(DEFINED ENV{MINIMAL_BUILD})
    idf_build_set_property(MINIMAL_BUILD ON)
endif()

project(esp32_wifi_streamer)
