/* SPDX-License-Identifier: Apache-2.0 */
#include "dfu_board.h"
#include "dfu_display.h"
#include "dfu_media.h"
#include "dfu_package.h"
#include "dfu_records.h"
#include "bf0_hal.h"
#include "ptab.h"
#include <stdio.h>
#include <string.h>

#define ITEMS_PER_PAGE 14u
#define FLASH_BATCH (64u * 1024u)
#define IO_CHUNK (16u * 1024u)
#define RECOVERY_UNKNOWN (DFU_STARTED | DFU_TARGET_MASK)

typedef struct
{
    char name[FF_LFN_BUF + 1];
    uint32_t size;
    bool directory;
} directory_item_t;

static directory_item_t items[ITEMS_PER_PAGE];
static char directory[DFU_PATH_SIZE] = "/";
static dfu_request_t request;
static dfu_package_t package;
static uint8_t buffer[IO_CHUNK] __attribute__((aligned(32)));
static uint32_t recovery_state, refreshed_at;
static bool screen_ok, card_ready;
static int queued_steps;
static const char *phase_before;
static unsigned percent_before;
static const char *const names[] = {"Main firmware", "Resources", "ACPU", "Waveform"};

static void trace(const char *phase, const char *detail)
{
    dfu_log("DFU: ");
    dfu_log(phase);
    if (detail) { dfu_log(" "); dfu_log(detail); }
    dfu_log("\r\n");
}

static void line(unsigned row, const char *text)
{
    if (!screen_ok) return;
    char label[27];
    unsigned i = 0;
    while (*text && i < sizeof(label) - 1)
    {
        unsigned char ch = *text++;
        if ((ch & 0xc0) == 0x80) continue;
        label[i++] = ch >= 32 && ch < 127 ? ch : '?';
    }
    label[i] = 0;
    dfu_display_text(28, 48 + 52 * (int)row, label);
}

static void begin_page(const char *title)
{
    if (!screen_ok) return;
    dfu_display_clear();
    line(0, title);
    dfu_display_rect(28, 94, 628, 2, true);
}

static bool show_page(void (*service)(void))
{
    if (!screen_ok) return false;
    if (dfu_display_refresh(service) == 0) return true;
    screen_ok = false;
    trace("Display failed", NULL);
    return false;
}

static void progress(const char *phase, const char *target, uint32_t done, uint32_t total)
{
    unsigned percent = total ? done * 100u / total : 0;
    uint32_t now = HAL_GetTick();
    if (phase == phase_before && (percent == percent_before ||
        (percent < 100 && (percent - percent_before < 5 || now - refreshed_at < 1200)))) return;
    phase_before = phase;
    percent_before = percent;
    refreshed_at = now;
    begin_page("Firmware Update");
    line(3, phase);
    if (target) line(4, target);
    char label[12];
    snprintf(label, sizeof(label), "%u%%", percent);
    line(7, label);
    if (screen_ok)
    {
        dfu_display_rect(28, 320, 628, 22, true);
        dfu_display_rect(30, 322, 624, 18, false);
        dfu_display_rect(30, 322, 624 * (int)percent / 100, 18, true);
    }
    show_page(NULL);
}

static void preparing(dfu_target_t target, uint32_t done, uint32_t total)
{
    progress("Preparing", names[target], done, 4 * total);
}

static const char *package_error(dfu_package_result_t error)
{
    switch (error)
    {
        case DFU_PACKAGE_IO: return "TF card access failed";
        case DFU_PACKAGE_ZIP: return "Invalid ZIP";
        case DFU_PACKAGE_MANIFEST: return "Invalid manifest";
        case DFU_PACKAGE_LAYOUT_MISMATCH: return "Wrong board or layout";
        case DFU_PACKAGE_RANGE: return "Invalid image range";
        case DFU_PACKAGE_CRC: return "Image CRC failed";
        case DFU_PACKAGE_NO_SPACE: return "TF card is full";
        default: return "Invalid update package";
    }
}

static bool mount_card(void)
{
    if (card_ready && dfu_media_mounted()) return true;
    dfu_media_unmount();
    card_ready = dfu_media_mount() == FR_OK;
    trace(card_ready ? "TF mounted" : "TF mount failed", NULL);
    return card_ready;
}

static dfu_key_t next_key(void)
{
    for (;;)
    {
        dfu_key_t key = dfu_key_poll();
        if (key != DFU_KEY_NONE) return key;
        __WFI();
    }
}

static void queue_navigation(void)
{
    dfu_key_t key = dfu_key_poll();
    /* Confirm only after the user can see the selected row. */
    if (key == DFU_KEY2) --queued_steps;
    if (key == DFU_KEY3) ++queued_steps;
}

static void selection_mark(unsigned row, bool active)
{
    if (screen_ok) dfu_display_rect(14, 48 + 52 * (int)row, 8, 34, active);
}

static unsigned select_row(unsigned first_row, unsigned count, unsigned selected)
{
    queued_steps = 0;
    selection_mark(first_row + selected, true);
    show_page(queue_navigation);
    for (;;)
    {
        int steps = queued_steps;
        queued_steps = 0;
        if (!steps)
        {
            dfu_key_t key = next_key();
            if (key == DFU_KEY1) return selected;
            steps = key == DFU_KEY2 ? -1 : 1;
        }
        unsigned next = (unsigned)(((int)selected + steps % (int)count + (int)count) % (int)count);
        if (next == selected) continue;
        uint32_t start = HAL_GetTick();
        selection_mark(first_row + selected, false);
        selection_mark(first_row + next, true);
        selected = next;
        if (show_page(queue_navigation))
        {
            char message[56];
            snprintf(message, sizeof(message), "DFU: Selection %u in %lu ms\r\n",
                     selected + 1, (unsigned long)(HAL_GetTick() - start));
            dfu_log(message);
        }
    }
}

static unsigned menu(const char *title, const char *detail, const char *const *options, unsigned count)
{
    begin_page(title);
    if (detail) line(2, detail);
    for (unsigned i = 0; i < count; ++i) line(4 + i, options[i]);
    for (unsigned i = 0, row = 10; i < DFU_PACKAGE_MAX_IMAGES; ++i)
        if (recovery_state & (1u << i)) line(row++, names[i]);
    line(21, "KEY1 OK  KEY2 UP  KEY3 DN");
    return select_row(4, count, 0);
}

static bool exit_to_platform(void)
{
    uint32_t state;
    if (!dfu_state_load(&state) || state != 0 || !dfu_leave()) return false;
    if (card_ready) (void)dfu_package_cleanup();
    trace("Exit and reboot", NULL);
    dfu_reboot();
}

static unsigned browse_page(unsigned page, bool *more)
{
    DIR iterator;
    FILINFO info;
    *more = false;
    if (f_opendir(&iterator, directory) != FR_OK) return 0;
    unsigned skipped = 0, count = 0;
    while (f_readdir(&iterator, &info) == FR_OK && info.fname[0])
    {
        char path[DFU_PATH_SIZE];
        int length = snprintf(path, sizeof(path), "%s%s%s", directory,
                              directory[1] ? "/" : "", info.fname);
        if (length < 0 || (size_t)length >= sizeof(path)) continue;
        bool is_dir = (info.fattrib & AM_DIR) != 0;
        if (is_dir ? strcmp(path, "/EPDDFU") == 0 :
            !dfu_request_path_valid(path) || !info.fsize || info.fsize > UINT32_MAX) continue;
        if (skipped < page * ITEMS_PER_PAGE) { ++skipped; continue; }
        if (count == ITEMS_PER_PAGE) { *more = true; break; }
        directory_item_t *item = &items[count++];
        strcpy(item->name, info.fname);
        item->size = is_dir ? 0 : (uint32_t)info.fsize;
        item->directory = is_dir;
    }
    f_closedir(&iterator);
    return count;
}

static bool confirm_package(void)
{
    static const char *const options[] = {"Start update", "Back to list", "Exit and reboot"};
    unsigned count = recovery_state ? 2 : 3;
    begin_page("Confirm Update");
    line(2, strrchr(request.path, '/') + 1);
    unsigned row = 4;
    for (unsigned i = 0; i < package.count; ++i)
    {
        const dfu_package_image_t *image = &package.images[i];
        char label[32];
        snprintf(label, sizeof(label), "%s %lu KiB", names[image->target],
                 (unsigned long)((image->size + 1023) / 1024));
        line(row++, label);
    }
    char total[24];
    snprintf(total, sizeof(total), "Total %lu KiB", (unsigned long)((package.total_size + 1023) / 1024));
    line(10, total);
    for (unsigned i = 0; i < count; ++i) line(16 + i, options[i]);
    line(21, "KEY1 OK  KEY2 UP  KEY3 DN");
    unsigned selected = select_row(16, count, 0);
    if (selected == 2) exit_to_platform();
    return selected == 0 && screen_ok;
}

static const char *choose_package(void)
{
    unsigned page = 0, selected = 0;
    for (;;)
    {
        if (!mount_card())
        {
            const char *options[] = {"Reload card", "Exit and reboot"};
            if (menu("Firmware Update", "Insert a TF card", options, recovery_state ? 1 : 2) == 1 &&
                !exit_to_platform()) return "Cannot clear boot flag";
            continue;
        }
        bool more;
        unsigned count = browse_page(page, &more);
        bool up = directory[1] || page;
        bool exit = !recovery_state;
        unsigned length = count + up + more + exit + 1;
        if (selected >= length) selected = 0;
        begin_page("Firmware Update");
        line(2, directory);
        unsigned row = 0;
        if (up) line(3 + row++, page ? "Previous page" : "Parent directory");
        for (unsigned i = 0; i < count; ++i)
        {
            char label[sizeof(items[i].name) + 32];
            if (items[i].directory) snprintf(label, sizeof(label), "%u /%s", page * ITEMS_PER_PAGE + i + 1, items[i].name);
            else
            {
                char short_name[20];
                snprintf(short_name, sizeof(short_name), "%.19s", items[i].name);
                snprintf(label, sizeof(label), "%u %s %luK", page * ITEMS_PER_PAGE + i + 1, short_name,
                         (unsigned long)((items[i].size - 1) / 1024 + 1));
            }
            line(3 + row++, label);
        }
        if (more) line(3 + row++, "Next page");
        if (exit) line(3 + row++, "Exit and reboot");
        line(3 + row, "Reload card");
        line(21, "KEY1 OK  KEY2 UP  KEY3 DN");
        selected = select_row(3, length, selected);
        if ((selected < up + count + more) && !dfu_media_mounted())
        {
            card_ready = false;
            directory[1] = '\0';
            page = selected = 0;
            continue;
        }
        if (up && selected == 0)
        {
            if (page) --page;
            else
            {
                char *slash = strrchr(directory, '/');
                if (slash == directory) ++slash;
                *slash = 0;
            }
            continue;
        }
        unsigned index = selected - up;
        if (index < count)
        {
            const directory_item_t *item = &items[index];
            snprintf(request.path, sizeof(request.path), "%s%s%s", directory, directory[1] ? "/" : "", item->name);
            if (item->directory)
            {
                strcpy(directory, request.path);
                page = selected = 0;
                continue;
            }
            request.zip_size = item->size;
            dfu_package_result_t result = dfu_package_open(request.path, &package);
            if (result != DFU_PACKAGE_OK) return package_error(result);
            if (package.zip_size != request.zip_size) return "ZIP changed";
            if (!confirm_package()) continue;
            if (!dfu_request_store(&request)) return "Cannot save update task";
            return NULL;
        }
        if (more && index == count) { ++page; selected = 0; continue; }
        if (exit && index == count + more && !exit_to_platform()) return "Cannot clear boot flag";
        card_ready = false;
    }
}

static const char *check_staged_image(const dfu_package_image_t *image, uint32_t done)
{
    FIL input;
    if (f_open(&input, image->staging_path, FA_READ) != FR_OK) return "Cannot read staged image";
    const char *error = f_size(&input) == image->size ? NULL : "Staged image size changed";
    uint32_t crc = 0;
    for (uint32_t offset = 0; !error && offset < image->size;)
    {
        UINT got, chunk = image->size - offset;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        progress("Checking", names[image->target], package.total_size + done + offset, 4 * package.total_size);
        if (f_read(&input, buffer, chunk, &got) != FR_OK || got != chunk)
            error = "TF card read failed";
        else
        {
            crc = dfu_crc32(crc, buffer, chunk);
            offset += chunk;
        }
    }
    f_close(&input);
    if (error) return error;
    return crc == image->crc32 ? NULL : "Staged image CRC failed";
}

static const char *write_image(const dfu_package_image_t *image, uint32_t done)
{
    FIL input;
    if (f_open(&input, image->staging_path, FA_READ) != FR_OK) return "Cannot read staged image";
    const char *error = NULL;
    const char *target = names[image->target];
    uint32_t input_crc = 0;
    uint32_t bit = 1u << image->target;
    if (f_size(&input) != image->size) error = "Staged image size changed";
    if (!error && image->target == DFU_TARGET_WAVEFORM)
    {
        begin_page("Firmware Update");
        line(3, "Updating waveform...");
        if (!show_page(NULL)) error = "Waveform display failed";
    }
    if (!error && !dfu_state_append(recovery_state | DFU_STARTED | bit)) error = "Cannot save write state";
    if (!error)
    {
        recovery_state |= DFU_STARTED | bit;
        trace("Writing", target);
    }
    for (uint32_t offset = 0; !error && offset < image->size;)
    {
        uint32_t address = image->address + offset;
        uint32_t length = FLASH_BATCH - address % FLASH_BATCH;
        if (length > image->size - offset) length = image->size - offset;
        uint32_t erase_size = (length + DFU_SECTOR_SIZE - 1) & ~(DFU_SECTOR_SIZE - 1);
        if (image->target != DFU_TARGET_WAVEFORM)
            progress("Writing", target, 2 * package.total_size + 2 * done + offset, 4 * package.total_size);
        if (!dfu_flash_erase(address, erase_size)) { error = "Flash erase failed"; break; }
        for (uint32_t remaining = length; remaining;)
        {
            UINT got, chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            if (f_read(&input, buffer, chunk, &got) != FR_OK || got != chunk)
            { error = "TF card read failed"; break; }
            input_crc = dfu_crc32(input_crc, buffer, chunk);
            if (!dfu_flash_write(image->address + offset, buffer, chunk))
            { error = "Flash write failed"; break; }
            offset += chunk;
            remaining -= chunk;
        }
    }
    f_close(&input);
    if (error) return error;
    if (input_crc != image->crc32) return "Staged image CRC failed";

    trace("Verifying", target);
    uint32_t readback_crc = 0;
    for (uint32_t offset = 0; offset < image->size;)
    {
        uint32_t chunk = image->size - offset;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (image->target != DFU_TARGET_WAVEFORM)
            progress("Verifying", target, 2 * package.total_size + 2 * done + image->size + offset, 4 * package.total_size);
        if (!dfu_flash_read(image->address + offset, buffer, chunk)) return "Flash read failed";
        readback_crc = dfu_crc32(readback_crc, buffer, chunk);
        offset += chunk;
    }
    if (readback_crc != image->crc32) return "Flash CRC failed";
    if (!dfu_state_append(recovery_state & ~bit)) return "Cannot clear write state";
    recovery_state &= ~bit;
    return NULL;
}

static const char *run_update(void)
{
    if (!mount_card()) return "Cannot mount TF card";
    FILINFO info;
    if (f_stat(request.path, &info) != FR_OK || info.fsize != request.zip_size) return "ZIP missing or changed";
    trace("Preparing", NULL);
    phase_before = NULL;
    progress("Preparing", NULL, 0, 1);
    dfu_package_result_t result = dfu_package_prepare(request.path, &package, preparing);
    if (result != DFU_PACKAGE_OK) return package_error(result);
    if (package.zip_size != request.zip_size) return "ZIP changed";

    uint32_t done = 0;
    for (unsigned i = 0; i < package.count; ++i)
    {
        const dfu_package_image_t *image = &package.images[i];
        const char *error = check_staged_image(image, done);
        if (error) { trace(error, names[image->target]); return error; }
        done += image->size;
    }
    done = 0;
    for (unsigned i = 0; i < package.count; ++i)
    {
        const dfu_package_image_t *image = &package.images[i];
        const char *error = write_image(image, done);
        if (error) { trace(error, names[image->target]); return error; }
        done += image->size;
    }
    if (recovery_state & DFU_TARGET_MASK) return "Targets still need repair";
    if (!dfu_leave()) return "Cannot clear boot flag";
    (void)dfu_package_cleanup();
    dfu_media_unmount();
    trace("Update complete", NULL);
    progress("Update complete", NULL, 1, 1);
    dfu_reboot();
}

void dfu_main(void)
{
    bool flash_ok = dfu_board_init();
    screen_ok = dfu_display_init() == 0;
    if (screen_ok)
    {
        begin_page("Firmware Update");
        line(3, "Starting...");
        if (show_page(NULL)) trace("First display transfer complete", NULL);
    }
    else trace("Display init failed", NULL);
    if (!dfu_keys_init()) trace("Key init failed", NULL);
    if (!flash_ok)
    {
        begin_page("Firmware Update");
        line(3, "Flash init failed");
        show_page(NULL);
        trace("Flash init failed", NULL);
        for (;;) __WFI();
    }
    bool state_valid = dfu_state_load(&recovery_state);
    if (!state_valid) recovery_state = RECOVERY_UNKNOWN;
    dfu_request_status_t task = dfu_request_load(&request);
    bool ready = task == DFU_REQUEST_VALID && state_valid;
    trace(ready ? "Automatic update" : "Manual recovery", NULL);
    const char *error = !state_valid ? "Write state invalid" :
                        task == DFU_REQUEST_BAD ? "Update task invalid" : NULL;
    for (;;)
    {
        if (error)
        {
            trace(error, NULL);
            const char *options[] = {"Retry update", "Choose another ZIP", "Exit and reboot"};
            unsigned choice = menu("Firmware Update Error", error, options + !ready,
                                   3 - !ready - !!recovery_state) + !ready;
            if (choice == 2)
            {
                if (!exit_to_platform()) error = "Cannot clear boot flag";
                continue;
            }
            if (choice == 1)
            {
                ready = false;
                if (!dfu_request_store(NULL)) { error = "Cannot clear update task"; continue; }
            }
            card_ready = false;
        }
        if (!ready)
        {
            error = choose_package();
            if (error) continue;
            ready = true;
        }
        error = run_update();
    }
}
