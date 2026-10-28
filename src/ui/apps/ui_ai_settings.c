#include "app_service_internal.h"
#include "storage_internal.h"
#include "ui_internal.h"
#include "platform/epd_keyboard.h"
#include "ui_ai_settings.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "app_files.h"
#include "ai/ai_settings.h"
#include "app_service.h"
#include "epd_memory.h"
#include "storage.h"
#include "system_reset_guard.h"
#include "boards/board_service.h"
#include <rtm.h>
#include <stdlib.h>
#include <limits.h>

enum {
    A_ADD = -16000, A_NAME, A_ENABLED, A_DEFAULT, A_KEY, A_CLEAR_KEY,
    A_MODELS, A_ADD_MODEL, A_MODEL_ID, A_MODEL_DEFAULT, A_MODEL_DELETE,
    A_MAX_TOKENS, A_TOKEN_FIELD, A_ADVANCED, A_ENDPOINT, A_PROTOCOL, A_AUTH,
    A_HEADERS, A_ADD_HEADER, A_HEADER_NAME, A_HEADER_VALUE, A_HEADER_DELETE,
    A_TIMEOUTS, A_CONNECT, A_SEND_IDLE, A_FIRST_EVENT, A_READ_IDLE, A_TOTAL,
    A_SAVE, A_DISCARD, A_DELETE, A_IMPORT, A_EXPORT, A_EXPORT_PATH,
    A_TEST, A_CANCEL_TEST, A_GRANTS, A_RELOAD, A_CONFIRM, A_INPUT_SAVE,
    A_INPUT_CANCEL, A_STATUS, A_APP, A_RESTORE_DRAFT, A_DROP_DRAFT,
    A_PROFILE = -17000, A_MODEL = -17100, A_HEADER = -17200,
    A_CAP = -17300, A_FEATURE = -17400, A_TEMPLATE = -17500, A_GRANT = -17600,
    A_DRAFT_ERROR = -17700
};
typedef enum { V_LIST, V_TEMPLATE, V_EDIT, V_MODELS, V_MODEL, V_ADVANCED,
               V_HEADERS, V_HEADER, V_TIMEOUTS, V_GRANTS } ai_view_mode_t;
typedef struct {
    lv_obj_t *title, *body, *editor, *input, *keyboard, *input_save, *input_cancel;
    ui_settings_list_t *list;
    lv_timer_t *timer;
    ai_settings_snapshot_t snapshot, incoming;
    ai_settings_job_t job;
    ai_profile_t draft;
    ai_settings_draft_t checkpoint;
    ai_settings_result_t result;
    ui_settings_list_row_t rows[AI_MAX_GRANTS + 20];
    char values[EPD_AI_MAX_PROFILES][EPD_AI_MODEL_SIZE + 80];
    char numbers[8][24], path[AI_SETTINGS_PATH_SIZE], import_path[AI_SETTINGS_PATH_SIZE], app[32];
    char selected_id[EPD_AI_ID_SIZE];
    uint32_t ticket, confirmation_serial, locale, scale;
    uint32_t candidate_id, card_session, draft_revision, input_capacity;
    int32_t bottom, width;
    int confirmation, input_key;
    unsigned model, header;
    ai_view_mode_t mode;
    bool dirty, redraw, has_secret, keep_secret, selector, import_pending;
    bool test_after_save, leave_after_save, cancelling;
    bool input_numeric, input_changed, closing_editor;
    epd_ai_error_t error, service_error;
} ai_view_t;

static rt_thread_t ui_owner;
static char selecting_app[32];
static bool selecting;
static uint32_t next_identity;
static void render(ai_view_t *view);
static void editor_layout(ai_view_t *view);
static bool submit(ai_view_t *view, ai_settings_operation_t operation);
static void capture_draft(ai_view_t *view);

static ai_view_t *page(void)
{
    return current && (current->id == UI_PAGE_AI_SETTINGS || current->id == UI_PAGE_AI_SELECT) ? current->feature : NULL;
}

static void wipe(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (size--) *bytes++ = 0;
}

static const char *error_text(epd_ai_error_t error)
{
    switch (error) {
    case EPD_AI_OK: return epd_get_str(ai_saved);
    case EPD_AI_INVALID: return epd_get_str(ai_error_invalid);
    case EPD_AI_UNSUPPORTED: return epd_get_str(ai_error_unsupported);
    case EPD_AI_UNAUTHORIZED: return epd_get_str(ai_error_unauthorized);
    case EPD_AI_BUSY: return epd_get_str(ai_error_busy);
    case EPD_AI_NO_MEMORY: return epd_get_str(ai_error_memory);
    case EPD_AI_NO_NETWORK: return epd_get_str(ai_error_network);
    case EPD_AI_DNS: return epd_get_str(ai_error_dns);
    case EPD_AI_CONNECT: return epd_get_str(ai_error_connect);
    case EPD_AI_TLS: return epd_get_str(ai_error_tls);
    case EPD_AI_AUTH: return epd_get_str(ai_error_auth);
    case EPD_AI_RATE_LIMIT: return epd_get_str(ai_error_rate);
    case EPD_AI_SERVER: return epd_get_str(ai_error_server);
    case EPD_AI_PROTOCOL: return epd_get_str(ai_error_protocol);
    case EPD_AI_LIMIT: return epd_get_str(ai_error_limit);
    case EPD_AI_TIMEOUT: return epd_get_str(ai_error_timeout);
    case EPD_AI_CANCELLED: return epd_get_str(ai_error_cancelled);
    case EPD_AI_INCOMPLETE: return epd_get_str(ai_error_incomplete);
    case EPD_AI_STORAGE: return epd_get_str(ai_error_storage);
    case EPD_AI_NOT_FOUND: return epd_get_str(ai_error_not_found);
    default: return epd_get_str(ai_error_protocol);
    }
}

static void show_error(ai_view_t *view, epd_ai_error_t error)
{
    view->error = error;
    view->redraw = true;
    popup_open(error_text(error), NULL);
}

static bool search_supported(ai_view_t *view, const epd_ai_model_t *model)
{
    switch (ai_model_search_support(&view->draft.public, model)) {
    case AI_SEARCH_PROTOCOL_REQUIRED:
        popup_open_i18n(LV_EXT_STR_ID(ai_search_protocol_required), model->id); break;
    case AI_SEARCH_DEEPSEEK_RESPONSES_REQUIRED:
        popup_open_i18n(LV_EXT_STR_ID(ai_search_deepseek_responses_required), model->id); break;
    case AI_SEARCH_DEEPSEEK_PRO_REQUIRED:
        popup_open_i18n(LV_EXT_STR_ID(ai_search_deepseek_pro_required), model->id); break;
    default: return true;
    }
    view->redraw=true;
    return false;
}

static bool locked(const ai_view_t *view)
{
    return view->ticket && view->job.operation != AI_SETTINGS_TEST;
}

static bool service_allowed(void)
{
    return current && current->foreground && !storage_changing() && !storage_release_requested() &&
        !board_power_pending() && !system_reset_blocks_apps() && !app_service_stopping();
}

static bool refresh_service(ai_view_t *view)
{
    epd_ai_error_t error=service_allowed() ? ai_settings_start() : EPD_AI_BUSY;
    if (error==EPD_AI_OK) error=ai_settings_snapshot(&view->incoming);
    if (error==EPD_AI_OK) {
        if (view->snapshot.generation!=view->incoming.generation ||
            view->snapshot.initialized!=view->incoming.initialized ||
            view->snapshot.stopping!=view->incoming.stopping ||
            view->snapshot.draft_available!=view->incoming.draft_available ||
            view->snapshot.draft_ready!=view->incoming.draft_ready ||
            view->snapshot.draft_error!=view->incoming.draft_error ||
            view->snapshot.initialization_error!=view->incoming.initialization_error) view->redraw=true;
        view->snapshot=view->incoming;
    }
    if (view->service_error!=error) view->redraw=true;
    view->service_error=error;
    return error==EPD_AI_OK && view->snapshot.initialized && !view->snapshot.stopping;
}

static void row(ai_view_t *view, size_t *count, int key, ui_settings_row_kind_t kind,
                const char *title, const char *value, bool enabled, bool checked)
{
    if (*count >= sizeof(view->rows) / sizeof(view->rows[0])) return;
    view->rows[(*count)++] = (ui_settings_list_row_t){.key=key, .kind=kind,
        .title=title, .value=value, .enabled=enabled && !locked(view), .checked=checked};
}
#define LINK(k,t,v) row(view,&n,k,UI_SETTINGS_ROW_LINK,epd_get_str(t),v,true,false)
#define AI_UI_SWITCH(k,t,b) row(view,&n,k,UI_SETTINGS_ROW_SWITCH,epd_get_str(t),NULL,true,b)
#define INFO(k,t,v) row(view,&n,k,UI_SETTINGS_ROW_INFO,epd_get_str(t),v,false,false)

static const char *protocol_name(epd_ai_protocol_t protocol)
{
    switch (protocol) {
    case EPD_AI_RESPONSES: return "OpenAI Responses";
    case EPD_AI_CHAT: return "Chat Completions";
    case EPD_AI_MESSAGES: return "Anthropic Messages";
    default: return "?";
    }
}

static const char *service_name(ai_view_t *view, const char *id)
{
    for (size_t i = 0; i < view->snapshot.profile_count; ++i)
        if (!strcmp(id, view->snapshot.profiles[i].id)) return view->snapshot.profiles[i].name;
    return id;
}

static lv_ext_str_id_t title_id(const ai_view_t *view)
{
    if (view->editor) switch (view->input_key) {
    case A_NAME: return LV_EXT_STR_ID(ai_name);
    case A_KEY: return LV_EXT_STR_ID(ai_key);
    case A_MODEL_ID: case A_ADD_MODEL: return LV_EXT_STR_ID(ai_model_id);
    case A_MAX_TOKENS: return LV_EXT_STR_ID(ai_max_tokens);
    case A_ENDPOINT: return LV_EXT_STR_ID(ai_endpoint);
    case A_HEADER_VALUE: return LV_EXT_STR_ID(ai_header_value);
    case A_EXPORT_PATH: return LV_EXT_STR_ID(ai_export_path);
    default: return LV_EXT_STR_ID(ai_timeouts);
    }
    if (view->selector) return LV_EXT_STR_ID(ai_select);
    switch (view->mode) {
    case V_TEMPLATE: return LV_EXT_STR_ID(ai_template);
    case V_EDIT: return LV_EXT_STR_ID(ai_title);
    case V_MODELS: return LV_EXT_STR_ID(ai_models);
    case V_MODEL: return LV_EXT_STR_ID(ai_caps);
    case V_ADVANCED: return LV_EXT_STR_ID(ai_advanced);
    case V_HEADERS: case V_HEADER: return LV_EXT_STR_ID(ai_headers);
    case V_TIMEOUTS: return LV_EXT_STR_ID(ai_timeouts);
    case V_GRANTS: return LV_EXT_STR_ID(ai_grant);
    default: return LV_EXT_STR_ID(ai_title);
    }
}

static void render(ai_view_t *view)
{
    if (!view || !view->list || current->popup || !lv_refreshing_done()) return;
    uint32_t locale = epd_locale_revision(), scale = epd_ui_scale_revision();
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    int32_t width = lv_obj_get_width(current->screen);
    if (!view->redraw && view->locale == locale && view->scale == scale && view->bottom == bottom && view->width == width) return;
    int32_t top = LV_MAX(SETTINGS_CONTENT_Y, SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32));
    lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_size(view->title, LV_MAX(1,width-SETTINGS_TITLE_X-MARGIN), ui_font_settings_title()->line_height);
    lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
    text_update(view->title, LV_EXT_STR_GET(title_id(view)));
    lv_obj_set_pos(view->body, 0, top);
    lv_obj_set_size(view->body, width, LV_MAX(1,bottom-top));
    if (view->editor) editor_layout(view);
    else {
        if (!ui_settings_list_refresh_fonts(view->list, ui_font_settings_body(), ui_font_settings_value())) return;
        size_t n = 0;
        epd_ai_profile_t *profile = &view->draft.public;
        epd_ai_model_t *model = &profile->models[view->model];
        switch (view->mode) {
        case V_LIST:
            if (view->selector) INFO(A_APP,ai_app,view->app);
            if (!view->selector && view->snapshot.draft_available) {
                INFO(A_STATUS-3,ai_draft_pending,NULL);
                LINK(A_RESTORE_DRAFT,ai_draft_restore,NULL); LINK(A_DROP_DRAFT,ai_draft_discard,NULL);
            }
            for (size_t i=0; i<view->snapshot.profile_count; ++i) {
                epd_ai_profile_t *p = &view->snapshot.profiles[i];
                snprintf(view->values[i],sizeof(view->values[i]),"%s%s%s",
                    p->model_count ? p->models[0].id : epd_get_str(ai_none),
                    (!p->enabled || p->is_default) ? " · " : "",
                    !p->enabled ? epd_get_str(ai_disabled) : p->is_default ? epd_get_str(ai_default) : "");
                row(view,&n,A_PROFILE+(int)i,UI_SETTINGS_ROW_LINK,p->name,view->values[i],
                    view->selector ? (p->enabled && p->model_count) :
                    (view->snapshot.draft_ready && !view->snapshot.draft_available),false);
            }
            if (view->snapshot.initialized && !view->snapshot.profile_count) INFO(A_STATUS,ai_empty,NULL);
            if (!view->selector) {
                row(view,&n,A_ADD,UI_SETTINGS_ROW_LINK,epd_get_str(ai_add),NULL,
                    view->snapshot.initialized && view->snapshot.draft_ready && !view->snapshot.draft_available &&
                    view->snapshot.profile_count<EPD_AI_MAX_PROFILES,false);
                row(view,&n,A_IMPORT,UI_SETTINGS_ROW_LINK,epd_get_str(ai_import),NULL,
                    view->snapshot.draft_ready && !view->snapshot.draft_available,false);
                LINK(A_EXPORT,ai_export,NULL);
                LINK(A_EXPORT_PATH,ai_export_path,view->path); LINK(A_GRANTS,ai_grant,NULL);
            }
            break;
        case V_TEMPLATE:
            row(view,&n,A_TEMPLATE,UI_SETTINGS_ROW_LINK,"OpenAI",NULL,true,false);
            row(view,&n,A_TEMPLATE+1,UI_SETTINGS_ROW_LINK,"DeepSeek",NULL,true,false);
            row(view,&n,A_TEMPLATE+2,UI_SETTINGS_ROW_LINK,"Anthropic",NULL,true,false);
            LINK(A_TEMPLATE+3,ai_custom,NULL); break;
        case V_EDIT:
            LINK(A_NAME,ai_name,profile->name); AI_UI_SWITCH(A_ENABLED,ai_enabled,profile->enabled);
            AI_UI_SWITCH(A_DEFAULT,ai_default,profile->is_default);
            LINK(A_KEY,ai_key,(view->has_secret || view->draft.secret[0]) ? epd_get_str(ai_key_set) : epd_get_str(ai_key_empty));
            LINK(A_CLEAR_KEY,ai_clear_key,NULL);
            LINK(A_MODELS,ai_models,profile->model_count ? profile->models[0].id : epd_get_str(ai_none));
            LINK(A_ADVANCED,ai_advanced,NULL); LINK(A_SAVE,ai_save,NULL);
            LINK(A_TEST,ai_test,NULL); LINK(A_DELETE,ai_delete,NULL); LINK(A_DISCARD,ai_discard,NULL);
            break;
        case V_MODELS:
            for (unsigned i=0;i<profile->model_count;++i)
                row(view,&n,A_MODEL+(int)i,UI_SETTINGS_ROW_LINK,profile->models[i].id,i ? NULL : epd_get_str(ai_model_default),true,false);
            row(view,&n,A_ADD_MODEL,UI_SETTINGS_ROW_LINK,epd_get_str(ai_add_model),NULL,profile->model_count<EPD_AI_MAX_MODELS,false);
            break;
        case V_MODEL:
            LINK(A_MODEL_ID,ai_model_id,model->id); AI_UI_SWITCH(A_MODEL_DEFAULT,ai_model_default,view->model==0);
            AI_UI_SWITCH(A_CAP,ai_cap_text,model->capabilities & EPD_AI_CAP_TEXT);
            AI_UI_SWITCH(A_CAP+1,ai_cap_stream,model->capabilities & EPD_AI_CAP_STREAM);
            AI_UI_SWITCH(A_CAP+2,ai_cap_tools,model->capabilities & EPD_AI_CAP_TOOLS);
            AI_UI_SWITCH(A_CAP+3,ai_cap_json,model->capabilities & EPD_AI_CAP_JSON);
            AI_UI_SWITCH(A_CAP+4,ai_cap_schema,model->capabilities & EPD_AI_CAP_SCHEMA);
            row(view,&n,A_CAP+5,UI_SETTINGS_ROW_SWITCH,epd_get_str(ai_cap_native),NULL,
                !(model->capabilities & EPD_AI_CAP_WEB_SEARCH),model->capabilities & EPD_AI_CAP_NATIVE);
            AI_UI_SWITCH(A_CAP+6,ai_cap_web_search,model->capabilities & EPD_AI_CAP_WEB_SEARCH);
            snprintf(view->numbers[0],sizeof(view->numbers[0]),"%lu",(unsigned long)model->max_output_tokens);
            LINK(A_MAX_TOKENS,ai_max_tokens,view->numbers[0]);
            if (profile->protocol==EPD_AI_CHAT) LINK(A_TOKEN_FIELD,ai_token_field,model->token_field==EPD_AI_MAX_TOKENS ? "max_tokens" : "max_completion_tokens");
            LINK(A_MODEL_DELETE,ai_delete_model,NULL); break;
        case V_ADVANCED:
            LINK(A_ENDPOINT,ai_endpoint,view->draft.endpoint); LINK(A_PROTOCOL,ai_protocol,protocol_name(profile->protocol));
            LINK(A_AUTH,ai_auth,view->draft.auth==AI_AUTH_BEARER ? "Bearer" : "x-api-key");
            LINK(A_HEADERS,ai_headers,NULL); LINK(A_TIMEOUTS,ai_timeouts,NULL);
            AI_UI_SWITCH(A_FEATURE,ai_feature_openai,profile->features & EPD_AI_FEATURE_OPENAI);
            AI_UI_SWITCH(A_FEATURE+1,ai_feature_deepseek,profile->features & EPD_AI_FEATURE_DEEPSEEK);
            AI_UI_SWITCH(A_FEATURE+2,ai_feature_encrypted,profile->features & EPD_AI_FEATURE_ENCRYPTED_REASONING);
            break;
        case V_HEADERS:
            for (unsigned i=0;i<view->draft.header_count;++i)
                row(view,&n,A_HEADER+(int)i,UI_SETTINGS_ROW_LINK,view->draft.headers[i].name,view->draft.headers[i].value,true,false);
            row(view,&n,A_ADD_HEADER,UI_SETTINGS_ROW_LINK,epd_get_str(ai_add_header),NULL,view->draft.header_count<AI_EXTRA_HEADERS,false);
            break;
        case V_HEADER:
            LINK(A_HEADER_NAME,ai_header_name,view->draft.headers[view->header].name);
            LINK(A_HEADER_VALUE,ai_header_value,view->draft.headers[view->header].value);
            LINK(A_HEADER_DELETE,ai_delete_header,NULL); break;
        case V_TIMEOUTS: {
            uint32_t times[]={view->draft.connect_ms,view->draft.send_idle_ms,view->draft.first_event_ms,view->draft.read_idle_ms,view->draft.total_ms};
            for (unsigned i=0;i<5;++i) snprintf(view->numbers[i],sizeof(view->numbers[i]),"%lu",(unsigned long)(times[i]/1000));
            LINK(A_CONNECT,ai_connect,view->numbers[0]); LINK(A_SEND_IDLE,ai_send_idle,view->numbers[1]);
            LINK(A_FIRST_EVENT,ai_first_event,view->numbers[2]); LINK(A_READ_IDLE,ai_read_idle,view->numbers[3]);
            LINK(A_TOTAL,ai_total,view->numbers[4]); break;
        }
        case V_GRANTS:
            for (size_t i=0;i<view->snapshot.grant_count;++i)
                row(view,&n,A_GRANT+(int)i,UI_SETTINGS_ROW_LINK,view->snapshot.grants[i].app,
                    service_name(view,view->snapshot.grants[i].profile),true,false);
            if (!view->snapshot.grant_count) INFO(A_STATUS,ai_no_grants,NULL);
            break;
        }
        if (view->service_error || !view->snapshot.initialized || view->snapshot.stopping) {
            epd_ai_error_t error=view->service_error ? view->service_error : view->snapshot.initialization_error;
            INFO(A_STATUS-2,ai_status,error && error!=EPD_AI_BUSY ? error_text(error) : epd_get_str(ai_loading));
            LINK(A_RELOAD,ai_retry,NULL);
        }
        if (view->snapshot.draft_error) {
            INFO(A_DRAFT_ERROR,ai_draft_pending,error_text(view->snapshot.draft_error));
            if (view->snapshot.initialized && !view->service_error) LINK(A_RELOAD,ai_retry,NULL);
        }
        if (view->ticket) {
            INFO(A_STATUS-1,ai_status,view->job.operation==AI_SETTINGS_TEST ?
                (view->cancelling ? epd_get_str(ai_cancelling) : epd_get_str(ai_test_pending)) : epd_get_str(ai_processing));
            if (view->job.operation==AI_SETTINGS_TEST) LINK(A_CANCEL_TEST,ai_cancel,NULL);
        }
        if (!ui_settings_list_set_rows(view->list,view->rows,n)) return;
        ui_settings_list_set_group(view->list,current->group);
    }
    view->locale=locale; view->scale=scale; view->bottom=bottom; view->width=width; view->redraw=false;
}
#undef LINK
#undef AI_UI_SWITCH
#undef INFO

static void selected(int key, void *context)
{
    if (current==context && current->foreground && !current->popup && !ui_shell_modal_visible() &&
        !storage_changing() && pending_page==NAV_IDLE) pending_page=key;
}

static void input_event(lv_event_t *event)
{
    if (!page() || lv_obj_get_screen(lv_event_get_target_obj(event))!=current->screen) return;
    int key=(int)(intptr_t)lv_event_get_user_data(event);
    if (lv_event_get_code(event)==LV_EVENT_READY) key=A_INPUT_SAVE;
    selected(key,current);
}

static lv_obj_t *input_button(lv_obj_t *parent,const char *text,int key)
{
    lv_obj_t *button=lv_button_create(parent);
    epd_obj_init(button);
    lv_obj_set_style_border_width(button,2,0);
    lv_obj_set_style_border_color(button,lv_color_black(),0);
    lv_obj_set_style_bg_color(button,lv_color_white(),0);
    lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
    lv_obj_set_style_radius(button,8,0);
    lv_obj_set_style_outline_width(button,2,LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button,-3,LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(button,input_event,LV_EVENT_CLICKED,(void *)(intptr_t)key);
    lv_obj_t *label=label_create(button,text,0,0,1,ui_font_settings_value());
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_group_add_obj(current->group,button);
    return button;
}

static void editor_layout(ai_view_t *view)
{
    int32_t width=lv_obj_get_width(current->screen),top=lv_obj_get_y(view->body);
    int32_t height=LV_MAX(1,ui_shell_content_bottom()-top);
    int32_t field=LV_MAX(epd_ui_touch(72),ui_font_settings_body()->line_height+epd_ui_px(24));
    int32_t gap=epd_ui_px(16),button_width=(width-2*MARGIN-gap)/2;
    lv_obj_set_pos(view->editor,0,top); lv_obj_set_size(view->editor,width,height);
    lv_obj_set_pos(view->input,MARGIN,0); lv_obj_set_size(view->input,width-2*MARGIN,field);
    lv_obj_set_style_text_font(view->input,ui_font_settings_body(),0);
    lv_obj_t *buttons[]={view->input_cancel,view->input_save};
    for (unsigned i=0;i<2;++i) {
        lv_obj_set_pos(buttons[i],MARGIN+(int32_t)i*(button_width+gap),field+gap);
        lv_obj_set_size(buttons[i],button_width,field);
        lv_obj_t *label=lv_obj_get_child(buttons[i],0);
        text_update(label,i ? epd_get_str(ai_save) : epd_get_str(platform_cancel));
        lv_obj_set_style_text_font(label,ui_font_settings_value(),0);
        lv_obj_set_size(label,button_width-epd_ui_px(24),ui_font_settings_value()->line_height); lv_obj_center(label);
    }
    epd_keyboard_refresh(view->keyboard);
    int32_t available=LV_MAX(1,height-2*field-3*gap);
    int32_t keyboard_height=LV_MIN(epd_keyboard_preferred_height(view->keyboard),available);
    lv_obj_set_size(view->keyboard,width,keyboard_height);
    lv_obj_align(view->keyboard,LV_ALIGN_BOTTOM_MID,0,-epd_ui_px(16));
}

static void capture_draft(ai_view_t *view)
{
    if (!view || view->selector || view->closing_editor || !view->draft.public.id[0] ||
        (!view->dirty && !view->input_changed) ||
        (locked(view) && (view->job.operation==AI_SETTINGS_SAVE_PROFILE ||
                          view->job.operation==AI_SETTINGS_REMOVE))) return;
    ai_settings_draft_t *d=&view->checkpoint;
    memset(d,0,sizeof(*d));
    d->present=1; d->profile=view->draft; d->model=view->model; d->header=view->header;
    d->mode=view->mode; d->keep_secret=view->keep_secret; d->has_secret=view->has_secret; d->dirty=view->dirty;
    memcpy(d->selected_id,view->selected_id,sizeof(d->selected_id));
    if (view->editor && view->input_changed && view->input_key!=A_EXPORT_PATH) {
        d->input_key=view->input_key; d->input_capacity=view->input_capacity; d->input_numeric=view->input_numeric;
        const char *text=lv_textarea_get_text(view->input);
        if (strlen(text)>=sizeof(d->input)) { view->error=EPD_AI_LIMIT; wipe(d,sizeof(*d)); return; }
        memcpy(d->input,text,strlen(text)+1);
    }
    epd_ai_error_t error=ai_settings_draft_set(d,&view->draft_revision);
    if (error) { view->error=error; view->redraw=true; }
    wipe(d,sizeof(*d));
}

static void draft_input_changed(lv_event_t *event)
{
    ai_view_t *view=lv_event_get_user_data(event);
    if (view->closing_editor || view->input_key==A_EXPORT_PATH) return;
    view->input_changed=true; capture_draft(view);
}

static void edit_text(ai_view_t *view,int key,const char *text,size_t capacity,bool numeric)
{
    view->input_key=key;
    view->input_capacity=(uint32_t)capacity; view->input_numeric=numeric; view->input_changed=false;
    ui_settings_list_set_group(view->list,NULL); object_visible(view->body,false);
    view->editor=lv_obj_create(current->screen); epd_obj_init(view->editor);
    view->input=lv_textarea_create(view->editor);
    lv_textarea_set_one_line(view->input,true);
    lv_textarea_set_max_length(view->input,(uint32_t)(capacity-1));
    if (numeric) lv_textarea_set_accepted_chars(view->input,"0123456789");
    if (key==A_KEY) { lv_textarea_set_password_mode(view->input,true); lv_textarea_set_password_show_time(view->input,0); }
    lv_textarea_set_text(view->input,text);
    lv_obj_add_event_cb(view->input,draft_input_changed,LV_EVENT_VALUE_CHANGED,view);
    lv_obj_set_style_border_width(view->input,2,0); lv_obj_set_style_border_color(view->input,lv_color_black(),0);
    lv_obj_set_style_shadow_width(view->input,0,LV_STATE_FOCUSED);
    lv_group_add_obj(current->group,view->input);
    view->input_cancel=input_button(view->editor,epd_get_str(platform_cancel),A_INPUT_CANCEL);
    view->input_save=input_button(view->editor,epd_get_str(ai_save),A_INPUT_SAVE);
    const epd_keyboard_config_t config={
        .mode=key==A_KEY ? EPD_KEYBOARD_PASSWORD : numeric ? EPD_KEYBOARD_NUMBER :
            key==A_NAME || key==A_EXPORT_PATH ? EPD_KEYBOARD_TEXT : EPD_KEYBOARD_ASCII,
        .chinese=key==A_NAME || key==A_EXPORT_PATH,
        .voice=key==A_NAME
    };
    view->keyboard=epd_keyboard_create(view->editor,view->input,&config);
    lv_obj_add_event_cb(view->keyboard,input_event,LV_EVENT_READY,NULL);
    editor_layout(view); lv_group_focus_obj(view->input);
}

static void editor_close(ai_view_t *view)
{
    if (!view->editor) return;
    view->closing_editor=true;
    /* Clear the textarea's owned credential before releasing the editor. */
    if (view->input_key==A_KEY) {
        const char *text=lv_textarea_get_text(view->input);
        wipe((void *)text,strlen(text));
        lv_textarea_set_text(view->input,"");
    }
    lv_obj_delete(view->editor);
    view->editor=view->input=view->keyboard=view->input_save=view->input_cancel=NULL;
    view->closing_editor=false; view->input_changed=false;
    object_visible(view->body,true); view->redraw=true;
    ui_settings_list_set_group(view->list,current->group);
    ui_settings_list_focus_key(view->list,view->input_key);
}

static bool copy_input(char *out,size_t capacity,const char *text,bool empty)
{
    size_t n=strlen(text);
    if (n>=capacity || (!empty && !n)) return false;
    for (size_t i=0;i<n;++i) if ((unsigned char)text[i]<32 || text[i]==127) return false;
    memcpy(out,text,n+1); return true;
}

static bool tf_path(const char *path)
{
    size_t root=strlen(STORAGE_SD_ROOT), n=strlen(path);
    return n>root+6 && !strncmp(path,STORAGE_SD_ROOT,root) && path[root]=='/' &&
        !strstr(path,"/../") && !strstr(path,"/./") && !strchr(path,'\\') &&
        !strcmp(path+n-5,".json");
}

static bool input_apply(ai_view_t *view)
{
    const char *text=lv_textarea_get_text(view->input);
    epd_ai_model_t *model=&view->draft.public.models[view->model];
    bool ok=false;
    switch (view->input_key) {
    case A_NAME: ok=copy_input(view->draft.public.name,sizeof(view->draft.public.name),text,false); break;
    case A_KEY:
        ok=copy_input(view->draft.secret,sizeof(view->draft.secret),text,false);
        if (ok) { view->keep_secret=false; view->has_secret=true; } break;
    case A_ENDPOINT: ok=copy_input(view->draft.endpoint,sizeof(view->draft.endpoint),text,false); break;
    case A_MODEL_ID: case A_ADD_MODEL: {
        unsigned index=view->input_key==A_ADD_MODEL ? view->draft.public.model_count : view->model;
        if (index>=EPD_AI_MAX_MODELS) break;
        for (unsigned i=0;i<view->draft.public.model_count;++i)
            if (i!=index && !strcmp(view->draft.public.models[i].id,text)) return false;
        ok=copy_input(view->draft.public.models[index].id,EPD_AI_MODEL_SIZE,text,false);
        if (ok && view->input_key==A_ADD_MODEL) {
            model=&view->draft.public.models[index]; model->capabilities=EPD_AI_CAP_TEXT;
            model->max_output_tokens=1024; model->token_field=EPD_AI_MAX_TOKENS;
            ++view->draft.public.model_count; view->model=index; view->mode=V_MODEL;
        }
        break;
    }
    case A_HEADER_VALUE: ok=copy_input(view->draft.headers[view->header].value,sizeof(view->draft.headers[0].value),text,false); break;
    case A_EXPORT_PATH:
        ok=tf_path(text) && copy_input(view->path,sizeof(view->path),text,false);
        if (ok) { editor_close(view); return true; } break;
    default: {
        if (!text[0]) break;
        uint32_t number=0;
        for (const char *p=text;*p;++p) {
            if (*p<'0' || *p>'9' || number>(UINT32_MAX-(unsigned)(*p-'0'))/10) return false;
            number=number*10+(unsigned)(*p-'0');
        }
        if (!number || number>(view->input_key==A_MAX_TOKENS ? 1048576u : 900u)) break;
        switch (view->input_key) {
        case A_MAX_TOKENS: model->max_output_tokens=number; break;
        case A_CONNECT: view->draft.connect_ms=number*1000; break;
        case A_SEND_IDLE: view->draft.send_idle_ms=number*1000; break;
        case A_FIRST_EVENT: view->draft.first_event_ms=number*1000; break;
        case A_READ_IDLE: view->draft.read_idle_ms=number*1000; break;
        case A_TOTAL: view->draft.total_ms=number*1000; break;
        default: return false;
        }
        ok=true; break;
    }
    }
    if (ok) { view->dirty=true; editor_close(view); }
    return ok;
}

static void confirm(ai_view_t *view,int action,lv_ext_str_id_t text,const char *detail)
{
    view->confirmation=action;
    popup_confirm_i18n(text,detail,A_CONFIRM);
    view->confirmation_serial=current->popup_serial;
}

static bool discard_draft(ai_view_t *view)
{
    epd_ai_error_t error=ai_settings_draft_discard(view->draft_revision);
    if (error) { show_error(view,error); return false; }
    view->draft_revision=0; view->dirty=false;
    wipe(&view->draft,sizeof(view->draft));
    view->mode=V_LIST; refresh_service(view);
    return true;
}

static void restore_draft(ai_view_t *view)
{
    ai_settings_draft_t *d=&view->checkpoint;
    epd_ai_error_t error=ai_settings_draft_get(d);
    if (error) { show_error(view,error); return; }
    size_t capacity=0; bool numeric=false;
    switch (d->input_key) {
    case 0: break;
    case A_NAME: capacity=sizeof(d->profile.public.name); break;
    case A_KEY: capacity=sizeof(d->profile.secret); break;
    case A_MODEL_ID: case A_ADD_MODEL: capacity=EPD_AI_MODEL_SIZE; break;
    case A_ENDPOINT: capacity=sizeof(d->profile.endpoint); break;
    case A_HEADER_VALUE: capacity=sizeof(d->profile.headers[0].value); break;
    case A_MAX_TOKENS: case A_CONNECT: case A_SEND_IDLE: case A_FIRST_EVENT: case A_READ_IDLE: case A_TOTAL:
        capacity=16; numeric=true; break;
    default: error=EPD_AI_INVALID; break;
    }
    if (d->mode<V_EDIT || d->mode>V_TIMEOUTS || d->model>=EPD_AI_MAX_MODELS ||
        d->header>=AI_EXTRA_HEADERS || (d->input_key && (capacity!=d->input_capacity || numeric!=!!d->input_numeric)) ||
        ((d->mode==V_MODEL || d->input_key==A_MODEL_ID || d->input_key==A_MAX_TOKENS) && d->model>=d->profile.public.model_count) ||
        ((d->mode==V_HEADER || d->input_key==A_HEADER_VALUE) && d->header>=d->profile.header_count)) error=EPD_AI_INVALID;
    if (error) { wipe(d,sizeof(*d)); show_error(view,error); return; }
    view->draft=d->profile; view->model=d->model; view->header=d->header;
    view->mode=(ai_view_mode_t)d->mode; view->keep_secret=d->keep_secret!=0;
    view->has_secret=d->has_secret!=0; view->dirty=d->dirty!=0; view->draft_revision=d->revision;
    memcpy(view->selected_id,d->selected_id,sizeof(view->selected_id));
    if (d->input_key) { edit_text(view,d->input_key,d->input,capacity,numeric); view->input_changed=true; }
    wipe(d,sizeof(*d));
}

static void discard_import(ai_view_t *view)
{
    if (view->candidate_id) ai_settings_import_discard(view->candidate_id);
    if (view->ticket && view->job.operation==AI_SETTINGS_IMPORT_PREPARE) {
        ai_settings_import_discard(view->ticket); view->ticket=0;
    }
    view->candidate_id=0;
}

static bool submit(ai_view_t *view,ai_settings_operation_t operation)
{
    if (locked(view)) return false;
    if (operation==AI_SETTINGS_SAVE_PROFILE) {
        for (unsigned i=0;i<view->draft.public.model_count;++i) {
            const epd_ai_model_t *model=&view->draft.public.models[i];
            if ((model->capabilities & EPD_AI_CAP_WEB_SEARCH) && !search_supported(view,model)) {
                view->test_after_save=view->leave_after_save=false;
                return false;
            }
        }
    }
    if (!refresh_service(view) && !((operation==AI_SETTINGS_RELOAD || operation==AI_SETTINGS_IMPORT_PREPARE ||
        operation==AI_SETTINGS_IMPORT) &&
        view->service_error==EPD_AI_OK && !view->snapshot.stopping)) {
        if (view->service_error && view->service_error!=EPD_AI_BUSY) show_error(view,view->service_error);
        return false;
    }
    if (view->ticket) ai_settings_cancel(view->ticket);
    memset(&view->job,0,sizeof(view->job));
    view->job.operation=operation;
    view->job.card_session=view->card_session; view->job.candidate_id=view->candidate_id;
    view->job.draft_revision=view->draft_revision;
    snprintf(view->job.profile_id,sizeof(view->job.profile_id),"%s",view->selected_id);
    if (operation==AI_SETTINGS_SAVE_PROFILE) {
        view->job.profile=view->draft; view->job.keep_secret=view->keep_secret;
    }
    if (operation==AI_SETTINGS_TEST) {
        snprintf(view->job.profile_id,sizeof(view->job.profile_id),"%s",view->draft.public.id);
        snprintf(view->job.model_id,sizeof(view->job.model_id),"%s",view->draft.public.models[view->model].id);
    }
    snprintf(view->job.app_id,sizeof(view->job.app_id),"%s",view->app);
    snprintf(view->job.path,sizeof(view->job.path),"%s",operation==AI_SETTINGS_IMPORT_PREPARE ? view->import_path : view->path);
    epd_ai_error_t error=ai_settings_submit(&view->job,&view->ticket);
    wipe(view->job.profile.secret,sizeof(view->job.profile.secret));
    view->cancelling=false; view->redraw=true;
    if (error!=EPD_AI_OK) { view->ticket=0; show_error(view,error); return false; }
    if (operation==AI_SETTINGS_IMPORT) view->candidate_id=0;
    return true;
}

static void new_profile(ai_view_t *view,unsigned preset)
{
    wipe(&view->draft,sizeof(view->draft));
    epd_ai_profile_t *p=&view->draft.public;
    p->struct_size=sizeof(*p); p->version=EPD_AI_VERSION; p->enabled=true;
    bool unique;
    do {
        snprintf(p->id,sizeof(p->id),"ai-%08lx-%08lx",(unsigned long)lv_tick_get(),(unsigned long)++next_identity);
        unique=true;
        for (size_t i=0;i<view->snapshot.profile_count;++i) if (!strcmp(view->snapshot.profiles[i].id,p->id)) unique=false;
    } while (!unique);
    static const char *const names[]={"OpenAI","DeepSeek","Anthropic","AI"};
    static const char *const endpoints[]={"https://api.openai.com/v1/responses","https://api.deepseek.com/responses","https://api.anthropic.com/v1/messages","https://"};
    snprintf(p->name,sizeof(p->name),"%s",names[preset]);
    snprintf(view->draft.endpoint,sizeof(view->draft.endpoint),"%s",endpoints[preset]);
    p->protocol=preset==2 ? EPD_AI_MESSAGES : EPD_AI_RESPONSES;
    p->features=preset==0 ? EPD_AI_FEATURE_OPENAI : preset==1 ? EPD_AI_FEATURE_DEEPSEEK : 0;
    view->draft.auth=preset==2 ? AI_AUTH_API_KEY : AI_AUTH_BEARER;
    view->draft.connect_ms=view->draft.send_idle_ms=30000;
    view->draft.first_event_ms=90000; view->draft.read_idle_ms=60000; view->draft.total_ms=300000;
    if (preset==2) {
        view->draft.header_count=1;
        strcpy(view->draft.headers[0].name,"anthropic-version"); strcpy(view->draft.headers[0].value,"2023-06-01");
    }
    view->has_secret=view->keep_secret=false; view->dirty=view->redraw=true; view->model=0; view->mode=V_EDIT;
    view->selected_id[0]=0;
}

static bool imported(const char *path,void *context)
{
    ai_view_t *view=context;
    if (!tf_path(path) || !copy_input(view->import_path,sizeof(view->import_path),path,false)) {
        popup_open_i18n(LV_EXT_STR_ID(ai_error_tf),NULL); return false;
    }
    view->card_session=storage_card_session();
    view->import_pending=true; return true;
}

static void process(ai_view_t *view)
{
    if (!view || !current->foreground) return;
    refresh_service(view);
    if (current->popup || !service_allowed()) return;
    if (view->candidate_id && view->confirmation==A_IMPORT) {
        discard_import(view); view->confirmation=0;
    }
    epd_ai_error_t error;
    if (view->import_pending) {
        view->import_pending=false;
        submit(view,AI_SETTINGS_IMPORT_PREPARE); return;
    }
    if (view->ticket) {
        error=ai_settings_poll(view->ticket,&view->result);
        if (error!=EPD_AI_BUSY) {
            ai_settings_operation_t operation=view->job.operation;
            view->ticket=0; view->redraw=true;
            if (error==EPD_AI_OK) error=view->result.error;
            if (operation==AI_SETTINGS_TEST && error==EPD_AI_OK && !view->result.complete) error=EPD_AI_INCOMPLETE;
            if (error!=EPD_AI_OK) {
                if (operation==AI_SETTINGS_IMPORT_PREPARE || operation==AI_SETTINGS_IMPORT) discard_import(view);
                view->test_after_save=view->leave_after_save=false;
                view->error=error;
                const char *detail=NULL;
                if (view->result.http_status) {
                    snprintf(view->numbers[0],sizeof(view->numbers[0]),"HTTP %d",view->result.http_status);
                    detail=view->numbers[0];
                }
                popup_open(error_text(error),detail); return;
            }
            if (operation==AI_SETTINGS_IMPORT_PREPARE) {
                view->candidate_id=view->result.candidate_id;
                confirm(view,A_IMPORT,LV_EXT_STR_ID(ai_import_confirm),view->import_path); return;
            } else if (operation==AI_SETTINGS_SAVE_PROFILE) {
                view->dirty=false; view->keep_secret=true;
                view->draft_revision=0;
                wipe(view->draft.secret,sizeof(view->draft.secret));
                snprintf(view->selected_id,sizeof(view->selected_id),"%s",view->draft.public.id);
                if (view->test_after_save) { view->test_after_save=false; submit(view,AI_SETTINGS_TEST); }
                else if (view->leave_after_save) { view->leave_after_save=false; view->mode=V_LIST; }
                else popup_open_i18n(LV_EXT_STR_ID(ai_saved),NULL);
            } else if (operation==AI_SETTINGS_TEST) popup_open_i18n(LV_EXT_STR_ID(ai_test_success),NULL);
            else if (operation==AI_SETTINGS_GRANT && view->selector) {
                if (!ui_navigation_back()) popup_open_i18n(LV_EXT_STR_ID(platform_back_failed),NULL);
                return;
            } else if (operation==AI_SETTINGS_REMOVE || operation==AI_SETTINGS_IMPORT) {
                view->mode=V_LIST; view->dirty=false; wipe(&view->draft,sizeof(view->draft));
            } else if (operation==AI_SETTINGS_EXPORT) popup_open_i18n(LV_EXT_STR_ID(ai_exported),NULL);
        }
    }
    render(view);
}

static void poll(lv_timer_t *timer)
{
    if (current==lv_timer_get_user_data(timer)) process(page());
}

void ui_ai_settings_init(void) { ui_owner=rt_thread_self(); }

bool epd_ai_select_service(epd_service_t *owner)
{
    if (rt_interrupt_get_nest() || !ui_owner || rt_thread_self()!=ui_owner || selecting ||
        storage_changing() || ui_shell_modal_visible() || (current && current->popup)) return false;
    char app[sizeof(selecting_app)]={0};
    if (!epd_service_identity(owner,app,sizeof(app)) || !app[0]) return false;
    memcpy(selecting_app,app,sizeof(app)); selecting=true;
    if (!ui_navigation_open(UI_PAGE_AI_SELECT)) { selecting=false; wipe(selecting_app,sizeof(selecting_app)); return false; }
    return true;
}
RTM_EXPORT(epd_ai_select_service);

epd_ai_error_t epd_ai_authorize_service(epd_service_t *owner, uint32_t *ticket)
{
    if (!ticket) return EPD_AI_INVALID;
    *ticket=0;
    if (rt_interrupt_get_nest() || !ui_owner || rt_thread_self()!=ui_owner || storage_changing()) return EPD_AI_BUSY;
    char app[32]={0};
    if (!epd_service_identity(owner,app,sizeof(app)) || !app[0]) return EPD_AI_INVALID;
    epd_ai_error_t e=ai_settings_start();
    if(e) return e;
    ai_settings_snapshot_t *snapshot=epd_app_alloc(sizeof(*snapshot),EPD_APP_PSRAM);
    ai_settings_job_t *job=epd_app_alloc(sizeof(*job),EPD_APP_PSRAM);
    if(!snapshot || !job){epd_app_free(snapshot);epd_app_free(job);return EPD_AI_NO_MEMORY;}
    e=ai_settings_snapshot(snapshot); int selected=-1;
    if(!e && !snapshot->initialized)e=snapshot->initialization_error?snapshot->initialization_error:EPD_AI_BUSY;
    if(!e)for(size_t i=0;i<snapshot->profile_count;++i){
        epd_ai_profile_t *p=&snapshot->profiles[i];if(!p->enabled)continue;
        for(size_t j=0;j<p->model_count;++j)if(p->models[j].capabilities&EPD_AI_CAP_TEXT){
            if(selected<0 || !strcmp(p->id,snapshot->default_id))selected=(int)i;break;
        }
    }
    if(!e && selected<0)e=EPD_AI_NOT_FOUND;
    if(!e){
        memset(job,0,sizeof(*job));job->operation=AI_SETTINGS_GRANT;
        strcpy(job->app_id,app);strcpy(job->profile_id,snapshot->profiles[selected].id);
        e=ai_settings_submit(job,ticket);
    }
    epd_app_free(job);epd_app_free(snapshot);return e;
}
RTM_EXPORT(epd_ai_authorize_service);

epd_ai_error_t epd_ai_authorization_poll(uint32_t ticket)
{
    if(rt_interrupt_get_nest() || !ui_owner || rt_thread_self()!=ui_owner)return EPD_AI_INVALID;
    ai_settings_result_t result={0};epd_ai_error_t e=ai_settings_poll(ticket,&result);
    if(e)return e;
    return result.operation==AI_SETTINGS_GRANT?result.error:EPD_AI_INVALID;
}
RTM_EXPORT(epd_ai_authorization_poll);

void ui_ai_settings_create(lv_obj_t *screen)
{
    ai_view_t *view=epd_app_alloc(sizeof(*view),EPD_APP_PSRAM);
    if (!view) { centered_label_i18n(screen,LV_EXT_STR_ID(platform_no_memory),MARGIN,300,CONTENT_WIDTH,ui_font_settings_body()); return; }
    memset(view,0,sizeof(*view)); current->feature=view;
    view->selector=current->id==UI_PAGE_AI_SELECT; view->redraw=true;
    if (view->selector) memcpy(view->app,selecting_app,sizeof(view->app));
    snprintf(view->path,sizeof(view->path),"%s/ai-services.json",STORAGE_SD_ROOT);
    view->title=page_title_create(screen,title_id(view),ui_font_settings_title());
    view->body=lv_obj_create(screen); epd_obj_init(view->body);
    view->list=ui_settings_list_create(view->body,ui_font_settings_body(),ui_font_settings_value(),selected,current);
    view->timer=lv_timer_create(poll,250,current);
    process(view);
}

void ui_ai_settings_restore_focus(void)
{
    ai_view_t *view=page();
    if (!view || current->popup) return;
    if (view->editor) {
        lv_group_add_obj(current->group,view->input); lv_group_add_obj(current->group,view->input_cancel); lv_group_add_obj(current->group,view->input_save);
    } else ui_settings_list_set_group(view->list,current->group);
}

void ui_ai_settings_resume(void)
{
    ai_view_t *view=page(); if (!view) return;
    view->redraw=true; if (view->timer) lv_timer_resume(view->timer); process(view);
}

void ui_ai_settings_pause(void)
{
    ai_view_t *view=page(); if (!view) return;
    capture_draft(view); discard_import(view);
    if (view->confirmation==A_IMPORT || view->confirmation==A_EXPORT) view->card_session=0;
    if (view->ticket && view->job.operation==AI_SETTINGS_TEST) ai_settings_cancel(view->ticket);
    if (view->timer) lv_timer_pause(view->timer);
}

void ui_ai_settings_stop(void)
{
    ai_view_t *view=page();
    if (current && current->id==UI_PAGE_AI_SELECT) { selecting=false; wipe(selecting_app,sizeof(selecting_app)); }
    if (!view) return;
    capture_draft(view); discard_import(view);
    files_picker_cancel(current->screen);
    if (view->ticket && view->job.operation==AI_SETTINGS_TEST) ai_settings_cancel(view->ticket);
    if (view->timer) lv_timer_delete(view->timer);
    editor_close(view);
    if (view->list) ui_settings_list_destroy(view->list);
    wipe(view,sizeof(*view)); epd_app_free(view); current->feature=NULL;
}

static bool confirmed(ai_view_t *view)
{
    if (!current->popup || current->popup_serial!=view->confirmation_serial) return true;
    int action=view->confirmation; view->confirmation=0; popup_close();
    switch (action) {
    case A_DISCARD: case A_DROP_DRAFT: discard_draft(view); break;
    case A_DELETE: submit(view,AI_SETTINGS_REMOVE); break;
    case A_IMPORT:
        if (!view->candidate_id || !storage_session_valid(STORAGE_SD,view->card_session)) {
            discard_import(view); show_error(view,EPD_AI_CANCELLED);
        } else submit(view,AI_SETTINGS_IMPORT);
        break;
    case A_EXPORT:
        if (!storage_session_valid(STORAGE_SD,view->card_session)) show_error(view,EPD_AI_CANCELLED);
        else submit(view,AI_SETTINGS_EXPORT);
        break;
    case A_GRANTS: submit(view,AI_SETTINGS_REVOKE); break;
    case A_PROFILE: submit(view,AI_SETTINGS_GRANT); break;
    case A_TEST:
        if (view->dirty) { view->test_after_save=true; if (!submit(view,AI_SETTINGS_SAVE_PROFILE)) view->test_after_save=false; }
        else submit(view,AI_SETTINGS_TEST);
        break;
    default: break;
    }
    view->redraw=true; render(view); return true;
}

bool ui_ai_settings_action(int *target)
{
    ai_view_t *view=page(); if (!view || !target) return false;
    int key=*target;
    if (key==A_CONFIRM) return confirmed(view);
    if (current->popup) return false;
    if (view->editor) {
        if (key==NAV_BACK || key==A_INPUT_CANCEL) {
            bool changed=view->input_changed;
            if (changed && !view->dirty && view->draft_revision) {
                epd_ai_error_t error=ai_settings_draft_discard(view->draft_revision);
                if (error) { show_error(view,error); return true; }
                view->draft_revision=0;
            }
            editor_close(view);
        }
        else if (key==A_INPUT_SAVE && !input_apply(view)) popup_open_i18n(LV_EXT_STR_ID(ai_error_input),NULL);
        capture_draft(view);
        render(view); return true;
    }
    if (key==NAV_BACK) {
        if (view->mode==V_LIST) { ui_ai_settings_pause(); return false; }
        if (locked(view)) return true;
        if (view->mode==V_EDIT && view->dirty) { confirm(view,A_DISCARD,LV_EXT_STR_ID(ai_discard_confirm),NULL); return true; }
        switch (view->mode) {
        case V_MODEL: view->mode=V_MODELS; break;
        case V_HEADER: view->mode=V_HEADERS; break;
        case V_TIMEOUTS: case V_HEADERS: view->mode=V_ADVANCED; break;
        case V_MODELS: case V_ADVANCED: view->mode=V_EDIT; break;
        default: view->mode=V_LIST; break;
        }
        capture_draft(view); view->redraw=true; render(view); return true;
    }
    if (!service_allowed() || locked(view)) return true;
    if (!view->selector && view->mode==V_LIST &&
        (!view->snapshot.draft_ready || view->snapshot.draft_available) &&
        (key==A_ADD || key==A_IMPORT || (key>=A_PROFILE && key<A_PROFILE+(int)view->snapshot.profile_count))) return true;
    epd_ai_profile_t *p=&view->draft.public;
    epd_ai_model_t *m=&p->models[view->model];
    if (key>=A_PROFILE && key<A_PROFILE+(int)view->snapshot.profile_count && view->mode==V_LIST) {
        epd_ai_profile_t *chosen=&view->snapshot.profiles[key-A_PROFILE];
        snprintf(view->selected_id,sizeof(view->selected_id),"%s",chosen->id);
        if (view->selector) {
            if (chosen->enabled && chosen->model_count) confirm(view,A_PROFILE,LV_EXT_STR_ID(ai_grant_confirm),chosen->name);
        } else {
            epd_ai_error_t error=ai_settings_profile_get(chosen->id,&view->draft,&view->has_secret);
            if (error) show_error(view,error);
            else { view->keep_secret=true; view->dirty=false; view->mode=V_EDIT; view->model=0; }
        }
    } else if (key>=A_TEMPLATE && key<A_TEMPLATE+4 && view->mode==V_TEMPLATE) new_profile(view,(unsigned)(key-A_TEMPLATE));
    else if (key>=A_MODEL && key<A_MODEL+p->model_count && view->mode==V_MODELS) { view->model=(unsigned)(key-A_MODEL); view->mode=V_MODEL; }
    else if (key>=A_HEADER && key<A_HEADER+view->draft.header_count && view->mode==V_HEADERS) { view->header=(unsigned)(key-A_HEADER); view->mode=V_HEADER; }
    else if (key>=A_CAP && key<A_CAP+7 && view->mode==V_MODEL) {
        uint32_t capability=1u<<(key-A_CAP);
        if (capability==EPD_AI_CAP_WEB_SEARCH && !(m->capabilities & capability) &&
            !search_supported(view,m)) return true;
        m->capabilities=ai_capabilities_with_dependencies(m->capabilities ^ capability);
        view->dirty=true;
    }
    else if (key>=A_FEATURE && key<A_FEATURE+3 && view->mode==V_ADVANCED) { p->features^=1u<<(key-A_FEATURE); view->dirty=true; }
    else if (key>=A_GRANT && key<A_GRANT+(int)view->snapshot.grant_count && view->mode==V_GRANTS) {
        ai_grant_t *grant=&view->snapshot.grants[key-A_GRANT];
        snprintf(view->app,sizeof(view->app),"%s",grant->app); snprintf(view->selected_id,sizeof(view->selected_id),"%s",grant->profile);
        confirm(view,A_GRANTS,LV_EXT_STR_ID(ai_revoke_confirm),grant->app);
    } else switch (key) {
    case A_RESTORE_DRAFT: restore_draft(view); break;
    case A_DROP_DRAFT: {
        epd_ai_error_t error=ai_settings_draft_get(&view->checkpoint);
        if (error) show_error(view,error);
        else { view->draft_revision=view->checkpoint.revision; confirm(view,A_DROP_DRAFT,LV_EXT_STR_ID(ai_discard_confirm),NULL); }
        wipe(&view->checkpoint,sizeof(view->checkpoint)); break;
    }
    case A_ADD: if (!view->selector && view->snapshot.profile_count<EPD_AI_MAX_PROFILES) view->mode=V_TEMPLATE; break;
    case A_NAME: edit_text(view,key,p->name,sizeof(p->name),false); break;
    case A_KEY: edit_text(view,key,"",sizeof(view->draft.secret),false); break;
    case A_CLEAR_KEY: wipe(view->draft.secret,sizeof(view->draft.secret)); view->has_secret=view->keep_secret=false; view->dirty=true; break;
    case A_ENABLED: p->enabled=!p->enabled; view->dirty=true; break;
    case A_DEFAULT: p->is_default=!p->is_default; view->dirty=true; break;
    case A_MODELS: view->mode=V_MODELS; break;
    case A_ADD_MODEL: if (p->model_count<EPD_AI_MAX_MODELS) edit_text(view,key,"",EPD_AI_MODEL_SIZE,false); break;
    case A_MODEL_ID: edit_text(view,key,m->id,sizeof(m->id),false); break;
    case A_MODEL_DEFAULT: {
        epd_ai_model_t selected=*m;
        memmove(p->models+1,p->models,view->model*sizeof(*m)); p->models[0]=selected;
        view->model=0; view->dirty=true; break;
    }
    case A_MODEL_DELETE:
        memmove(m,m+1,(p->model_count-view->model-1)*sizeof(*m)); --p->model_count;
        memset(&p->models[p->model_count],0,sizeof(*m)); view->model=0; view->mode=V_MODELS; view->dirty=true; break;
    case A_TOKEN_FIELD: m->token_field=m->token_field==EPD_AI_MAX_TOKENS ? EPD_AI_MAX_COMPLETION_TOKENS : EPD_AI_MAX_TOKENS; view->dirty=true; break;
    case A_MAX_TOKENS: snprintf(view->numbers[0],sizeof(view->numbers[0]),"%lu",(unsigned long)m->max_output_tokens); edit_text(view,key,view->numbers[0],16,true); break;
    case A_ADVANCED: view->mode=V_ADVANCED; break;
    case A_ENDPOINT: edit_text(view,key,view->draft.endpoint,sizeof(view->draft.endpoint),false); break;
    case A_PROTOCOL: p->protocol=(epd_ai_protocol_t)((p->protocol+1)%3); view->dirty=true; break;
    case A_AUTH: view->draft.auth=view->draft.auth==AI_AUTH_BEARER ? AI_AUTH_API_KEY : AI_AUTH_BEARER; view->dirty=true; break;
    case A_HEADERS: view->mode=V_HEADERS; break;
    case A_ADD_HEADER:
        if (view->draft.header_count<AI_EXTRA_HEADERS) {
            view->header=view->draft.header_count++; memset(&view->draft.headers[view->header],0,sizeof(ai_header_t));
            strcpy(view->draft.headers[view->header].name,"anthropic-version"); view->mode=V_HEADER; view->dirty=true;
        } break;
    case A_HEADER_NAME: {
        static const char *const names[]={"anthropic-version","anthropic-beta","OpenAI-Organization","OpenAI-Project","OpenAI-Beta"};
        unsigned count=sizeof(names)/sizeof(names[0]),i=0;
        while (i<count && strcmp(view->draft.headers[view->header].name,names[i])) ++i;
        snprintf(view->draft.headers[view->header].name,sizeof(view->draft.headers[0].name),"%s",names[(i+1)%count]); view->dirty=true; break;
    }
    case A_HEADER_VALUE: edit_text(view,key,view->draft.headers[view->header].value,sizeof(view->draft.headers[0].value),false); break;
    case A_HEADER_DELETE:
        memmove(&view->draft.headers[view->header],&view->draft.headers[view->header+1],(view->draft.header_count-view->header-1)*sizeof(ai_header_t));
        --view->draft.header_count; memset(&view->draft.headers[view->draft.header_count],0,sizeof(ai_header_t)); view->mode=V_HEADERS; view->dirty=true; break;
    case A_TIMEOUTS: view->mode=V_TIMEOUTS; break;
    case A_CONNECT: case A_SEND_IDLE: case A_FIRST_EVENT: case A_READ_IDLE: case A_TOTAL:
        edit_text(view,key,view->numbers[key-A_CONNECT],16,true); break;
    case A_SAVE:
        if (!p->model_count) popup_open_i18n(LV_EXT_STR_ID(ai_error_model),NULL);
        else submit(view,AI_SETTINGS_SAVE_PROFILE); break;
    case A_DISCARD: confirm(view,A_DISCARD,LV_EXT_STR_ID(ai_discard_confirm),NULL); break;
    case A_DELETE:
        if (!view->selected_id[0]) discard_draft(view);
        else confirm(view,A_DELETE,LV_EXT_STR_ID(ai_delete_confirm),p->name); break;
    case A_IMPORT: if (!files_picker_open_json(imported,view)) show_error(view,EPD_AI_BUSY); return true;
    case A_EXPORT_PATH: edit_text(view,key,view->path,sizeof(view->path),false); break;
    case A_EXPORT:
        if (!tf_path(view->path)) popup_open_i18n(LV_EXT_STR_ID(ai_error_tf),NULL);
        else { view->card_session=storage_card_session(); confirm(view,A_EXPORT,LV_EXT_STR_ID(ai_export_confirm),view->path); } break;
    case A_TEST:
        if (!p->model_count) popup_open_i18n(LV_EXT_STR_ID(ai_error_model),NULL);
        else if (!view->ticket) confirm(view,A_TEST,LV_EXT_STR_ID(ai_test_confirm),p->models[view->model].id); break;
    case A_CANCEL_TEST: if (view->ticket && view->job.operation==AI_SETTINGS_TEST) { ai_settings_cancel(view->ticket); view->cancelling=true; } break;
    case A_GRANTS: view->mode=V_GRANTS; break;
    case A_RELOAD: submit(view,AI_SETTINGS_RELOAD); break;
    default: return false;
    }
    capture_draft(view); view->redraw=true; render(view); return true;
}
