#include "ui_home_model.h"
#include <string.h>

static bool id_valid(const char *id)
{
    if (!id || !id[0]) return false;
    for (unsigned i = 0; i < UI_HOME_ID_CAPACITY; ++i)
    {
        unsigned char c = (unsigned char)id[i];
        if (!c) return true;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return false;
}

bool ui_home_model_valid(const ui_home_model_t *model)
{
    if (!model || model->count > UI_HOME_DOCK_CAPACITY) return false;
    for (unsigned i = 0; i < model->count; ++i)
    {
        if (!id_valid(model->ids[i])) return false;
        for (unsigned j = 0; j < i; ++j)
            if (!strcmp(model->ids[i], model->ids[j])) return false;
    }
    return true;
}

bool ui_home_model_contains(const ui_home_model_t *model, const char *id)
{
    if (!id || !ui_home_model_valid(model)) return false;
    for (unsigned i = 0; i < model->count; ++i)
        if (!strcmp(model->ids[i], id)) return true;
    return false;
}

bool ui_home_model_pin(ui_home_model_t *model, const char *id)
{
    if (!ui_home_model_valid(model) || !id_valid(id)) return false;
    if (ui_home_model_contains(model, id)) return true;
    if (model->count == UI_HOME_DOCK_CAPACITY) return false;
    memset(model->ids[model->count], 0, UI_HOME_ID_CAPACITY);
    strcpy(model->ids[model->count++], id);
    return true;
}

bool ui_home_model_unpin(ui_home_model_t *model, const char *id)
{
    if (!id || !ui_home_model_valid(model)) return false;
    for (unsigned i = 0; i < model->count; ++i)
        if (!strcmp(model->ids[i], id))
        {
            --model->count;
            memmove(model->ids[i], model->ids[i + 1],
                    (model->count - i) * UI_HOME_ID_CAPACITY);
            memset(model->ids[model->count], 0, UI_HOME_ID_CAPACITY);
            return true;
        }
    return false;
}

unsigned ui_home_page_count(unsigned count)
{
    return count / UI_HOME_GRID_CAPACITY + (count % UI_HOME_GRID_CAPACITY != 0);
}

unsigned ui_home_page_clamp(unsigned page, unsigned count)
{
    unsigned pages = ui_home_page_count(count);
    return !pages ? 0 : page < pages ? page : pages - 1;
}

int ui_home_dock_x(unsigned index, unsigned count)
{
    if (!count || count > UI_HOME_DOCK_CAPACITY || index >= count) return 0;
    int gap = (660 - (int)count * 100) / (int)(count + 1);
    return gap + (int)index * (100 + gap);
}
