project("firmware-test")

# Calculate the root '/fw' directory relative to this file
set(FW_DIR ${CMAKE_SOURCE_DIR}/fw)

# Include external projects
set(EXTERNAL_DIR ${FW_DIR}/external)
add_subdirectory(
    ${EXTERNAL_DIR}/libyaml
    ${CMAKE_BINARY_DIR}/libyaml)

FIND_PATH(CHECK_INCLUDE_DIR check.h)
FIND_LIBRARY(CHECK_LIBRARIES NAMES check)

# Include source files
set(SRC_DIR ${FW_DIR}/src)
set(TEST_DIR ${FW_DIR}/test)

add_executable(${PROJECT_NAME}
    ${TEST_DIR}/keystate_test.c
    ${TEST_DIR}/config_test.c
    ${TEST_DIR}/window_test.c
    #${TEST_DIR}/menu_test.c
    ${TEST_DIR}/main.c
    ${SRC_DIR}/config/config.c
    ${SRC_DIR}/menu/window.c
    ${SRC_DIR}/menu/menu_config.c
    ${SRC_DIR}/term.c
    ${SRC_DIR}/usb/keystate.c
    #${SRC_DIR}/term.c
)

target_link_libraries(${PROJECT_NAME}
    ${CHECK_LIBRARIES}
    subunit
    m
    yaml)

# Include directories
include_directories(
    "${EXTERNAL_DIR}/libyaml/include")

add_custom_command(
    TARGET ${PROJECT_NAME} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${TEST_DIR}/data
            ${CMAKE_CURRENT_BINARY_DIR}/data
)
