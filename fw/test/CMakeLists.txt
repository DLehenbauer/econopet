project("firmware-test"
    VERSION 0.1.0
    LANGUAGES C)

# Include git version information
include(${FW_DIR}/cmake/GitVersion.cmake)

# Generate version header in build directory
configure_file(
    ${FW_DIR}/cmake/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.h
    @ONLY
)

# Include external projects
set(EXTERNAL_DIR ${FW_DIR}/external)

# Temporarily disable BUILD_TESTING for libyaml
set(BUILD_TESTING OFF CACHE BOOL "Disable testing for libyaml" FORCE)
add_subdirectory(
    ${EXTERNAL_DIR}/libyaml
    ${CMAKE_BINARY_DIR}/libyaml)
# Restore BUILD_TESTING to its original value
set(BUILD_TESTING ON CACHE BOOL "Enable testing for the rest of the project" FORCE)

FIND_PATH(CHECK_INCLUDE_DIR check.h)
FIND_LIBRARY(CHECK_LIBRARIES NAMES check)

# Include source files
set(SRC_DIR ${FW_DIR}/src)
set(TEST_DIR ${FW_DIR}/test)

add_executable(${PROJECT_NAME}
    ${SRC_DIR}/config/config.c
    ${SRC_DIR}/diag/log/log.c
    ${SRC_DIR}/display/char_encoding.c
    ${SRC_DIR}/display/window.c
    ${SRC_DIR}/fatal.c
    ${SRC_DIR}/global.c
    ${SRC_DIR}/menu/menu_config.c
    ${SRC_DIR}/system_state.c
    ${SRC_DIR}/usb/keyscan.c
    ${SRC_DIR}/usb/keystate.c
    ${TEST_DIR}/char_encoding_test.c
    ${TEST_DIR}/config_parser_test.c
    ${TEST_DIR}/keyscan_test.c
    ${TEST_DIR}/keystate_test.c
    ${TEST_DIR}/log_test.c
    ${TEST_DIR}/main.c
    ${TEST_DIR}/mock.c
    ${TEST_DIR}/window_test.c
)

target_link_libraries(${PROJECT_NAME}
    ${CHECK_LIBRARIES}
    subunit
    m
    yaml)

# Include directories
include_directories(
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${EXTERNAL_DIR}/libyaml/include"
    "${SRC_DIR}")

# Enable testing
enable_testing()

# Add the test executable as a CTest test
add_test(
    NAME firmware_tests
    COMMAND ${PROJECT_NAME}
)

# Set environment variable to point to sdcard directory
set_tests_properties(firmware_tests PROPERTIES
    ENVIRONMENT "ECONOPET_TEST_SDCARD_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/../../sdcard"
)
