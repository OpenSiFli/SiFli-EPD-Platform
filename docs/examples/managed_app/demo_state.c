/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "demo_state.h"
#include "services/app_background.h"

static bool capture(void *user, void *bytes, size_t capacity, size_t *size)
{
    demo_state_t *state = user;
    if (capacity < 4) return false;
    unsigned char *out = bytes;
    for (unsigned i = 0; i < 4; ++i) out[i] = (unsigned char)(state->count >> (i * 8));
    *size = 4;
    return true;
}
static bool restore_edits(void *user, const void *bytes, size_t size, uint32_t pending_edits)
{
    if (size != 4) return false;
    const unsigned char *in = bytes;
    uint32_t count = (uint32_t)in[0] | (uint32_t)in[1] << 8 |
                     (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
    if (pending_edits > UINT32_MAX - count) return false;
    ((demo_state_t *)user)->count = count + pending_edits;
    return true;
}
static bool restore(void *user, const void *bytes, size_t size)
{
    return restore_edits(user, bytes, size, 0);
}
static void detach(void *user)
{
    demo_state_t *state = user;
    state->checkpoint = 0;
}
bool demo_state_open(demo_state_t *state)
{
    if (!state) return false;
    if (state->checkpoint) {
        epd_cold_runtime_snapshot_t snapshot;
        if (!epd_checkpoint_get(state->checkpoint, &snapshot) || snapshot.closing) return false;
        if (snapshot.failed) (void)epd_checkpoint_retry(state->checkpoint);
        return true;
    }
    epd_checkpoint_spec_t spec = {
        .key = "counter", .format_version = 1, .source_version = 1,
        .maximum_bytes = 4, .optional = false, .user = state,
        .capture = capture, .restore = restore, .detach = detach
    };
    if (!epd_checkpoint_register_edits(&spec, restore_edits, &state->checkpoint)) return false;
    return true;
}
bool demo_state_increment(demo_state_t *state)
{
    if (!state || !state->checkpoint || state->count == UINT32_MAX) return false;
    epd_cold_runtime_snapshot_t snapshot;
    if (!epd_checkpoint_get(state->checkpoint, &snapshot)) return false;
    if (snapshot.failed) {
        (void)epd_checkpoint_retry(state->checkpoint);
        return false;
    }
    /* The unchanged template uses RELEASE. Editing the counter explicitly
     * chooses to retain this recoverable business state when leaving the page. */
    epd_app_background_snapshot_t background;
    if (epd_app_background_get(&background) != EPD_MEMORY_OK) return false;
    if (background.effective != EPD_APP_BG_STATE) {
        epd_app_background_policy_t policy = {EPD_APP_BG_STATE, 0, EPD_APP_BG_RELEASE};
        if (epd_app_background_set(&policy, NULL) != EPD_MEMORY_OK) return false;
    }
    if (!epd_checkpoint_changed(state->checkpoint)) return false;
    ++state->count;
    return true;
}
