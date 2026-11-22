/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_module.h"
#include "app_background.h"
#include "platform/epd_operation_runtime.h"
#include "platform/epd_resource_runtime.h"
#include "gui_app_context.h"
#include "gui_app_int.h"
#include <lvgl.h>
#include <rtthread.h>

#define PAGE_CONTEXT_COUNT 64

typedef struct {
    const void *page;
    epd_memory_owner_t owner;
    bool active;
    bool closing;
} page_context_t;

/* app_fwk page callbacks and destruction run on the GUI thread. */
static page_context_t pages[PAGE_CONTEXT_COUNT];

epd_memory_owner_t app_memory_foreground_page(void *context, epd_memory_owner_t account)
{
    (void)context;
    const void *page = gui_app_page_get_actived();
    for (unsigned i = 0; page && i < PAGE_CONTEXT_COUNT; ++i) {
        epd_memory_owner_snapshot_t state;
        if (pages[i].page == page && pages[i].active && !pages[i].closing &&
            epd_memory_owner_get(pages[i].owner, &state) && !state.closed && state.account == account)
            return pages[i].owner;
    }
    return EPD_MEMORY_OWNER_INVALID;
}

uint32_t gui_app_context_enter(const char *appid, const void *page)
{
    if (page)
        for (unsigned i = 0; i < PAGE_CONTEXT_COUNT; ++i)
            if (pages[i].page == page) return epd_memory_owner_enter_trusted(pages[i].owner);

    epd_memory_owner_t root = app_module_owner_acquire(appid);
    if (root == EPD_MEMORY_OWNER_INVALID)
        return app_module_loaded(appid) ? UINT32_MAX : epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);

    epd_memory_owner_t owner = root;
    if (page)
    {
        page_context_t *slot = NULL;
        for (unsigned i = 0; i < PAGE_CONTEXT_COUNT; ++i)
            if (!pages[i].page && !pages[i].closing) { slot = &pages[i]; break; }
        if (!slot) { epd_memory_owner_release_trusted(root); return UINT32_MAX; }
        owner = epd_memory_scope_create_trusted(root, EPD_MEMORY_SCOPE_PAGE);
        if (owner == EPD_MEMORY_OWNER_INVALID) { epd_memory_owner_release_trusted(root); return UINT32_MAX; }
        slot->page = page;
        slot->owner = owner;
    }
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    epd_memory_owner_release_trusted(root);
    return previous;
}

void gui_app_context_leave(uint32_t previous)
{
    epd_memory_owner_leave_trusted(previous);
}

void gui_app_context_page_active(const char *appid, const void *page, bool active)
{
    epd_memory_owner_t root = app_module_owner_acquire(appid);
    if (root == EPD_MEMORY_OWNER_INVALID) return;
    bool foreground = false;
    for (unsigned i = 0; i < PAGE_CONTEXT_COUNT; ++i)
    {
        if (pages[i].page == page)
        {
            pages[i].active = active;
            epd_resource_runtime_page_state(pages[i].owner, pages[i].owner, active);
        }
        epd_memory_owner_snapshot_t state;
        if (pages[i].page && pages[i].active &&
            epd_memory_owner_get(pages[i].owner, &state) && state.account == root)
            foreground = true;
    }
    app_background_foreground(root, foreground);
    epd_memory_owner_release_trusted(root);
}

void gui_app_context_page_destroyed(const char *appid, const void *page)
{
    gui_app_context_page_active(appid, page, false);
    for (unsigned i = 0; i < PAGE_CONTEXT_COUNT; ++i)
        if (pages[i].page == page)
        {
            epd_memory_owner_t owner = pages[i].owner;
            pages[i].page = NULL;
            pages[i].active = false;
            pages[i].closing = true;
            epd_operation_runtime_close_owner(owner);
            epd_resource_runtime_close_owner(owner);
            epd_memory_owner_close_trusted(owner);
            break;
        }
}

void app_memory_context_process(void)
{
    for (unsigned i = 0; i < PAGE_CONTEXT_COUNT; ++i)
        if (pages[i].closing && epd_operation_runtime_owner_drained(pages[i].owner) &&
            !epd_resource_runtime_owner_busy(pages[i].owner))
        {
#if LV_USE_EXECUTION_CONTEXT
            lv_execution_context_cancel(pages[i].owner);
#endif
            epd_memory_owner_release_trusted(pages[i].owner);
            pages[i].owner = EPD_MEMORY_OWNER_INVALID;
            pages[i].closing = false;
        }
}

#if LV_USE_EXECUTION_CONTEXT
uint32_t lv_execution_context_capture(void) { return epd_memory_owner_current(); }
bool lv_execution_context_matches(uint32_t owner, uint32_t root)
{
    epd_memory_owner_snapshot_t state;
    if (root == EPD_MEMORY_OWNER_INVALID || !epd_memory_owner_get(root, &state)) return false;
    for (;;)
    {
        if (!epd_memory_owner_get(owner, &state)) return false;
        if (owner == root) return true;
        if (owner == EPD_MEMORY_OWNER_SYSTEM) return false;
        owner = state.parent;
    }
}
bool lv_execution_context_is_open(uint32_t owner)
{
    while (owner != EPD_MEMORY_OWNER_SYSTEM)
    {
        epd_memory_owner_snapshot_t state;
        if (!epd_memory_owner_get(owner, &state) || state.closed) return false;
        owner = state.parent;
    }
    return true;
}
bool lv_execution_context_retain(uint32_t owner) { return epd_memory_owner_retain_trusted(owner); }
void lv_execution_context_release(uint32_t owner) { epd_memory_owner_release_trusted(owner); }
uint32_t lv_execution_context_enter(uint32_t owner) { return epd_memory_owner_enter_trusted(owner); }
void lv_execution_context_leave(uint32_t previous) { epd_memory_owner_leave_trusted(previous); }
#endif
