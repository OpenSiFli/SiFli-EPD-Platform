cmake_minimum_required(VERSION 3.20)
project(epd_refresh_tests C)
option(EPD_TEST_SDK_DRIVER "Use the SDK full-frame display path" ON)
set(CMAKE_C_STANDARD 11)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LVGL "${ROOT}/SiFli-SDK/external/lvgl_v9")
file(GLOB_RECURSE LV_SOURCES CONFIGURE_DEPENDS "${LVGL}/src/*.c")
add_library(lvgl_host STATIC ${LV_SOURCES})
target_include_directories(lvgl_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${LVGL}")
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE _CRT_SECURE_NO_WARNINGS)

set(REFRESH "${ROOT}/src/ui/platform/epd_refresh.c")
add_executable(epd_refresh_tests host.c "${REFRESH}")
target_include_directories(epd_refresh_tests PRIVATE "${ROOT}/src/ui/platform"
    "${ROOT}/src" "${ROOT}/src/services" "${ROOT}/src/boards/epd_e0470a03_57x"
    "${ROOT}/tests/epd_waveform/stubs")
if(EPD_TEST_SDK_DRIVER)
    target_include_directories(epd_refresh_tests PRIVATE "${ROOT}/SiFli-SDK/customer/peripherals/display/epd_e0470a03")
    target_compile_definitions(epd_refresh_tests PRIVATE LCD_FB_USING_ONE_UNCOMPRESSED LCD_USING_EPD_E0470A03)
    set(DRIVER_CASE native_frame)
else()
    target_include_directories(epd_refresh_tests PRIVATE "${ROOT}/SiFli-SDK/customer/peripherals/display/epd_e0470a03")
    target_compile_definitions(epd_refresh_tests PRIVATE LCD_FB_USING_NONE LCD_USING_EPD_E0470A03_V10_57)
    set(DRIVER_CASE chunk_frame reader_landscape_prepared app_switch_prepared input_status_prepared retry_success retry_stop
        retry_success_prepared retry_stop_prepared retry_stop_locked
        retry_stop_locked_prepared retry_newer_dirty retry_newer_prepared
        retry_external_during_retry retry_external_during_retry_prepared
        retry_maintenance retry_maintenance_queued)
endif()
# Count scheduling requests while retaining the SDK's actual timer behavior.
set_source_files_properties("${REFRESH}" PROPERTIES COMPILE_DEFINITIONS "lv_timer_ready=host_timer_ready")
target_link_libraries(epd_refresh_tests PRIVATE lvgl_host)
if(MSVC)
    target_compile_options(lvgl_host PRIVATE /utf-8 /MP /wd4244 /wd4267)
    target_compile_options(epd_refresh_tests PRIVATE /utf-8 /W4)
else()
    target_compile_options(epd_refresh_tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(epd_refresh_tests PRIVATE m)
endif()

enable_testing()
foreach(case ${DRIVER_CASE} idle_dirty busy_release flushing_gate sdk_busy_gate maintenance maintenance_rearm
             maintenance_paused lock_deadline lock_interaction lock_hold lock_background full_locked
             slider_deadline slider_release one_shot synchronous_rearm empty_idle deferred modes app_switch
             input_regions input_status first_frame_deadline first_frame_interaction reader_landscape_fallback)
    add_test(NAME refresh_${case} COMMAND epd_refresh_tests "${case}")
    set_tests_properties(refresh_${case} PROPERTIES TIMEOUT 10)
endforeach()
