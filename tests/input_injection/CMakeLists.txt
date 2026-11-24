# SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(input_injection_tests C)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO "../.." ABSOLUTE)
add_subdirectory("${REPO}/tests/notes/ui" notes)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${GENERATED}/actual_handlers.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py" "${REPO}" "${GENERATED}"
    DEPENDS generate.py "${REPO}/src/ui/ui_shell.c" "${REPO}/src/ui/apps/app_lock.c")
add_executable(input_injection_tests host_test.c "${GENERATED}/actual_handlers.inc"
    "${REPO}/src/platform/epd_input_test.c" "${REPO}/src/ui/ui_nav_gesture.c"
    "${REPO}/src/ui/ui_shell_model.c")
target_include_directories(input_injection_tests PRIVATE host "${GENERATED}" "${REPO}/src/platform" "${REPO}/src/ui")
target_include_directories(input_injection_tests SYSTEM PRIVATE "${REPO}/SiFli-SDK/external/lvgl_v9")
target_compile_definitions(input_injection_tests PRIVATE EPD_INPUT_TEST_ENABLED=1 RT_USING_FINSH)
target_link_libraries(input_injection_tests PRIVATE lvgl_host)
add_executable(input_injection_disabled disabled_test.c "${REPO}/src/platform/epd_input_test.c")
target_include_directories(input_injection_disabled PRIVATE "${REPO}/src/platform")
if(MSVC)
    target_compile_options(input_injection_tests PRIVATE /utf-8 /W4 /WX /UNDEBUG)
    target_compile_options(input_injection_disabled PRIVATE /utf-8 /W4 /WX /UNDEBUG)
endif()
enable_testing()
add_test(NAME input_injection COMMAND input_injection_tests)
add_test(NAME input_injection_default_off COMMAND input_injection_disabled)
