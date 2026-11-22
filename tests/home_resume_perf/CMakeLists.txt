cmake_minimum_required(VERSION 3.20)
project(home_resume_perf C)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO "../.." ABSOLUTE)
enable_testing()
add_subdirectory("${REPO}/tests/notes/ui" "lvgl")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${GENERATED}/home_actual.inc" "${GENERATED}/image_actual.inc"
    "${GENERATED}/decoder_actual.inc" "${GENERATED}/types_actual.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_inputs.py" "${REPO}" "${GENERATED}"
    DEPENDS generate_inputs.py "${REPO}/src/ui/apps/app_home.c"
    "${REPO}/src/ui/platform/app_image.c" "${REPO}/src/ui/platform/app_catalog.h"
    "${REPO}/src/ui/platform/app_installer.h" "${REPO}/src/ui/ui_internal.h"
    "${REPO}/SiFli-SDK/middleware/lvgl/lv_conf_sifli.h"
    "${REPO}/SiFli-SDK/middleware/lvgl/lv_drivers_v9/lvsf_img_decoder.c")
add_executable(home_resume_perf home_test.c "${REPO}/src/ui/apps/ui_home_model.c"
    "${GENERATED}/home_actual.inc" "${GENERATED}/image_actual.inc"
    "${GENERATED}/decoder_actual.inc" "${GENERATED}/types_actual.h")
target_include_directories(home_resume_perf PRIVATE "${GENERATED}" "${REPO}/src/ui/apps")
target_link_libraries(home_resume_perf PRIVATE lvgl_host)
if(MSVC)
    target_compile_options(home_resume_perf PRIVATE /utf-8 /UNDEBUG /wd4311 /wd4302)
endif()
add_test(NAME home_resume_perf COMMAND home_resume_perf)
