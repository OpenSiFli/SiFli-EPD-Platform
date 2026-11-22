/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#define main existing_install_tests_main
#include "../app_install_perf/host_test.c"
#undef main
#undef fopen
#undef fclose
#undef fgets
#undef ferror

static unsigned descriptor_reads;
static uint16_t u16(const unsigned char *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t u32(const unsigned char *b)
{ return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static const unsigned char *section(const host_file_t *f, unsigned index)
{
    size_t at = u32(f->data + 32) + index * (size_t)u16(f->data + 46);
    assert(index < u16(f->data + 48) && at + 40 <= f->size);
    return f->data + at;
}
static const unsigned char *address(const host_file_t *f, uint32_t va, size_t n)
{
    for (unsigned i = 0; i < u16(f->data + 48); ++i) {
        const unsigned char *s = section(f, i);
        uint32_t start = u32(s + 12), size = u32(s + 20);
        if ((u32(s + 8) & 2) && u32(s + 4) != 8 && va >= start &&
            va - start <= size && n <= size - (va - start)) {
            size_t at = u32(s + 16) + va - start;
            assert(at <= f->size && n <= f->size - at); return f->data + at;
        }
    }
    assert(!"ELF address outside allocated sections"); return NULL;
}
/* The ARM loader port returns the actual artifact's exported descriptor.
 * ARM code execution is replaced; package/catalog/module policy remains production. */
static void *artifact_symbol(void *handle, const char *name)
{
    struct rt_dlmodule *m = handle; char path[128];
    snprintf(path, sizeof(path), "/flash/apps/%s/%s.so", m->name, m->name);
    const host_file_t *f = host_find(path); assert(f && f->size >= 52);
    assert(!memcmp(f->data, "\177ELF\1\1\1", 7) && u16(f->data + 18) == 40);
    for (unsigned i = 0; i < u16(f->data + 48); ++i) {
        const unsigned char *s = section(f, i);
        if (u32(s + 4) != 2 && u32(s + 4) != 11) continue;
        const unsigned char *strings = section(f, u32(s + 24));
        for (uint32_t at = 0; at < u32(s + 20); at += u32(s + 36)) {
            assert(u32(s + 36) >= 16);
            const unsigned char *sym = f->data + u32(s + 16) + at;
            const char *symbol_name = (const char *)f->data + u32(strings + 16) + u32(sym);
            if (strcmp(symbol_name, name) || !u16(sym + 14)) continue;
            if (!strcmp(name, "epd_app_info")) {
                static epd_app_info_t result;
                const unsigned char *data = address(f, u32(sym + 4), 8);
                result.abi = u32(data);
                result.id = (const char *)address(f, u32(data + 4), strlen(m->name) + 1);
                ++descriptor_reads; return &result;
            }
            return (void *)(uintptr_t)0x1234;
        }
    }
    return NULL;
}
static void seed_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb"); assert(input);
    assert(!fseek(input, 0, SEEK_END)); long n = ftell(input); assert(n >= 0);
    rewind(input); void *data = malloc((size_t)n + 1); assert(data);
    assert(fread(data, 1, (size_t)n, input) == (size_t)n); assert(!fclose(input));
    host_put(destination, data, (size_t)n, false); free(data);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    char cases_path[1024]; snprintf(cases_path, sizeof(cases_path), "%s/cases.tsv", argv[1]);
    FILE *cases = fopen(cases_path, "r"); assert(cases);
    char label[64], id[32], origin[32]; unsigned manifest_abi, module_abi, count = 0;
    while (fscanf(cases, "%63s %31s %31s %u %u", label, id, origin, &manifest_abi, &module_abi) == 5) {
        fixture(); descriptor_reads = 0; host_symbol_hook = artifact_symbol;
        char source[1024], root[128];
        snprintf(root, sizeof(root), "/flash/apps/%s", id);
        snprintf(source, sizeof(source), "%s/%s/package.sap", argv[1], label);
        seed_file(source, "/flash/input.sap");
        epd_app_entry_t entry;
        app_package_status_t metadata = app_sap_metadata("/flash/input.sap", &entry);
        assert(metadata == (manifest_abi == EPD_APP_ABI ? APP_PACKAGE_OK : APP_PACKAGE_INCOMPATIBLE));
        if (!strcmp(origin, "preinstalled")) {
            char list[1024], relative[256];
            snprintf(list, sizeof(list), "%s/%s/files.txt", argv[1], label);
            FILE *files_list = fopen(list, "r"); assert(files_list);
            host_put(root, NULL, 0, true);
            while (fscanf(files_list, "%255s", relative) == 1) {
                char destination[512]; snprintf(source, sizeof(source), "%s/%s/tree/%s", argv[1], label, relative);
                snprintf(destination, sizeof(destination), "%s/%s", root, relative);
                seed_file(source, destination);
            }
            assert(!fclose(files_list));
        } else {
            app_package_status_t installed = app_sap_install("/flash/input.sap", root, id, NULL, NULL);
            assert(installed == (manifest_abi == EPD_APP_ABI ? APP_PACKAGE_OK : APP_PACKAGE_INCOMPATIBLE));
        }
        if (manifest_abi == EPD_APP_ABI)
            assert(app_package_verify(root, id, false, NULL) == APP_PACKAGE_OK);
        volume_ready[STORAGE_FLASH] = true; result.state = APP_JOB_IDLE;
        app_entity_info info = {0};
        bool accepted = module_open_requested(id, &info, true);
        assert(accepted == (manifest_abi == EPD_APP_ABI && module_abi == EPD_APP_ABI));
        assert(descriptor_reads == (manifest_abi == EPD_APP_ABI ? 1u : 0u));
        if (accepted) {
            struct rt_dlmodule *m = dlmodule_find(id); assert(m && instances);
            epd_memory_owner_t owner = instances->owner;
            epd_memory_owner_snapshot_t state; assert(epd_memory_owner_get(owner, &state));
            assert(!strcmp(state.appid, id));
            set_policy(owner, EPD_APP_BG_STATE, 0, EPD_APP_BG_RELEASE);
            app_module_close(m); app_modules_process();
            assert(instances && !instances->closing);
            assert(module_open_requested(id, &info, true) && instances->owner == owner);
            assert(dlmodule_find(id) == m && m->nref == 1);
            set_policy(owner, EPD_APP_BG_RELEASE, 0, EPD_APP_BG_RELEASE);
            app_module_close(m);
        } else assert(!info.entry_func && !info.user_data);
        app_modules_process(); assert(!instances && !dlmodule_find(id) && !host_watch_pin_count());
        epd_memory_metadata_snapshot_t memory; epd_memory_metadata_snapshot(&memory);
        assert(!memory.protocol_errors);
        assert(app_background_forget(id));
        printf("PASS %s origin=%s manifest=%u module=%u accepted=%u refs_drained=1\n",
               label, origin, manifest_abi, module_abi, accepted ? 1u : 0u); ++count;
    }
    assert(feof(cases) && count); fclose(cases);
    printf("PACKAGE_CASES=%u\n", count); return 0;
}
