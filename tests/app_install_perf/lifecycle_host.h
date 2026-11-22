#ifndef INSTALL_LIFECYCLE_HOST_H
#define INSTALL_LIFECYCLE_HOST_H
#include "host.h"
#include "platform/epd_memory_manager.h"
#include "platform/epd_operation_runtime.h"
#include "platform/epd_resource_runtime.h"
#include "platform/epd_cold_runtime.h"
#include "app_background.h"
epd_memory_result_t app_service_quiesce(const char *id);
size_t app_service_module_pins(const char *id);
void lv_execution_context_cancel(uintptr_t owner);
#endif
