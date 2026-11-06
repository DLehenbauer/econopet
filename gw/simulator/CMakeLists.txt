# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

cmake_minimum_required(VERSION 3.21)
project(EconoPET_Simulator LANGUAGES NONE)

get_filename_component(GW_MODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../EconoPET" ABSOLUTE)
add_custom_target(gateware_models ALL)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/VerilatorModels.cmake")

file(GLOB TB_FILES CONFIGURE_DEPENDS "${GW_MODEL_DIR}/sim/*_tb.sv")
foreach(TB_FILE IN LISTS TB_FILES)
    get_filename_component(TB_NAME "${TB_FILE}" NAME_WE)
    add_gateware_model(${TB_NAME})
endforeach()
