/* The background policy boundary is deterministic; task scopes and their
 * allocation ownership are supplied by the production memory core. */
static struct { epd_memory_owner_t owner, task; bool used, member; } groups[32];
static unsigned task_count, group_count, task_ends, component_wakes;
static bool accepting = true;
epd_memory_result_t app_background_group_create(epd_memory_owner_t owner, epd_app_task_group_t *out)
{
    epd_memory_owner_snapshot_t state;
    if (!accepting || !epd_memory_owner_get(owner,&state) || state.closed) return EPD_MEMORY_CLOSED;
    for (unsigned i=0;i<32;++i) if (!groups[i].used) {
        groups[i].used=true; groups[i].owner=state.account; *out=i+1; ++group_count; return EPD_MEMORY_OK;
    }
    return EPD_MEMORY_NO_METADATA;
}
epd_memory_result_t app_background_task_begin(epd_app_task_group_t group, epd_memory_owner_t *out)
{
    CHECK(group && group<=32 && groups[group-1].used && !groups[group-1].member);
    epd_memory_owner_t task=epd_memory_scope_create(groups[group-1].owner,EPD_MEMORY_SCOPE_TASK);
    if (task==EPD_MEMORY_OWNER_INVALID) return EPD_MEMORY_NO_METADATA;
    groups[group-1].task=task; groups[group-1].member=true; ++task_count; *out=task; return EPD_MEMORY_OK;
}
epd_memory_result_t app_background_group_default(epd_app_task_group_t group, epd_app_background_mode_t mode)
{ CHECK(group && groups[group-1].member && mode==EPD_APP_BG_STATE); return EPD_MEMORY_OK; }
bool app_background_group_accepting(epd_app_task_group_t group)
{ CHECK(group && group<=32 && groups[group-1].member); return accepting; }
bool app_background_task_end(epd_memory_owner_t task)
{
    for (unsigned i=0;i<32;++i) if (groups[i].used && groups[i].member && groups[i].task==task) {
        groups[i].member=false; CHECK(task_count); --task_count; ++task_ends;
        epd_memory_owner_close(task); epd_memory_owner_release(task); return true;
    }
    CHECK(false); return false;
}
epd_memory_result_t app_background_group_release(epd_app_task_group_t group)
{
    CHECK(group && group<=32 && groups[group-1].used && !groups[group-1].member);
    groups[group-1].used=false; CHECK(group_count); --group_count; return EPD_MEMORY_OK;
}
