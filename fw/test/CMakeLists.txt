project("firmware-test")

# Calculate the root '/fw' directory relative to this file
set(FW_DIR ${CMAKE_SOURCE_DIR}/fw)

FIND_PATH(CHECK_INCLUDE_DIR check.h)
FIND_LIBRARY(CHECK_LIBRARIES NAMES check)

# Include source files
set(SRC_DIR ${FW_DIR}/src)
set(TEST_DIR ${FW_DIR}/test)

add_executable(${PROJECT_NAME}
    ${TEST_DIR}/keystate_test.c
    ${TEST_DIR}/main.c
    ${SRC_DIR}/usb/keystate.c
)

target_link_libraries(${PROJECT_NAME} ${CHECK_LIBRARIES} subunit m)
