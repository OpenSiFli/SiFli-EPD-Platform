#include "app_service_internal.h"
#include "platform/epd_system_internal.h"
#include "epd_system.h"
#include "epd_sound.h"
#include "reminder_internal.h"
#include "app_service.h"
#include "sleep_guard.h"
#include <rthw.h>
#include <rtm.h>
#include <string.h>

typedef struct notification
{
    struct notification *next;
    epd_service_t *owner;
    char app_id[EPD_SYSTEM_APP_ID_SIZE], id[EPD_NOTIFICATION_ID_SIZE];
    /* NULL is a dismissal tombstone, not an empty notification. */
    epd_notification_t *value;
} notification_t;

typedef struct media_command
{
    struct media_command *next;
    epd_media_command_t value;
    epd_service_t *owner;
    uint64_t item, intent;
    bool automatic;
} media_command_t;

static struct rt_mutex mutex;
static bool initialized;
static notification_t *notifications;
static uint32_t notification_revision, media_revision;
static epd_service_t *media_owner;
static epd_media_t media;
static uint64_t media_item, media_intent;
static bool media_quiet;
static epd_service_t *media_overlay;
static epd_service_t *intent_pending_owner;
static char media_navigation[EPD_SYSTEM_APP_ID_SIZE];
static struct
{
    bool active, invalid, quiet, was_playing, pause_consumed;
    epd_service_t *owner;
    epd_media_t value;
    uint64_t item, intent;
} lease;
static media_command_t *commands, *commands_tail;
/* Queued commands own a separate hold; discarding them must not release playback. */
static sleep_guard_owner_t commands_hold = {.id = "media-commands"};

static bool lock(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!initialized)
        initialized = rt_mutex_init(&mutex, "sysstate", RT_IPC_FLAG_PRIO) == RT_EOK;
    bool ready = initialized;
    rt_hw_interrupt_enable(level);
    return ready && rt_mutex_take(&mutex, RT_WAITING_FOREVER) == RT_EOK;
}

static void unlock(void) { rt_mutex_release(&mutex); }

static notification_t *find(const char *app_id, const char *id)
{
    for (notification_t *item = notifications; item; item = item->next)
        if (!strcmp(item->app_id, app_id) && !strcmp(item->id, id)) return item;
    return NULL;
}

static bool same_notification(const epd_notification_t *a, const epd_notification_t *b)
{
    return !strcmp(a->title, b->title) && !strcmp(a->text, b->text) &&
           !strcmp(a->page, b->page) && a->ongoing == b->ongoing;
}

static bool notification_publish(epd_service_t *service, const epd_notification_t *value, bool resident)
{
    if (!value || !value->id[0] || !memchr(value->id, 0, sizeof(value->id)) ||
        !memchr(value->title, 0, sizeof(value->title)) ||
        !memchr(value->text, 0, sizeof(value->text)) ||
        !memchr(value->page, 0, sizeof(value->page)) || !lock()) return false;
    char id[EPD_SYSTEM_APP_ID_SIZE];
    bool ok = resident ? value->app_id[0] && memchr(value->app_id, 0, sizeof(value->app_id)) != NULL : epd_service_identity(service, id, sizeof(id));
    if (resident && ok) strcpy(id, value->app_id);
    if (!ok) { unlock(); return false; }
    notification_t **link = &notifications;
    while (*link && strcmp((*link)->app_id, id)) link = &(*link)->next;
    notification_t *item = *link;
    if (item && item->owner == service && !strcmp(item->id, value->id))
    {
        bool completed = false;
        if (item->value)
        {
            item->owner = service;
            if (!same_notification(item->value, value))
            {
                completed = item->value->ongoing && !value->ongoing;
                *item->value = *value;
                strcpy(item->value->app_id, id);
                ++notification_revision;
            }
        }
        unlock();
        if (completed && !resident) epd_sound_notify();
        return true;
    }
    epd_notification_t *copy = rt_malloc(sizeof(*copy));
    notification_t *created = item ? NULL : rt_calloc(1, sizeof(*created));
    if (!copy || (!item && !created))
    {
        rt_free(copy);
        rt_free(created);
        unlock();
        return false;
    }
    if (item)
    {
        *link = item->next;
        rt_free(item->value);
    }
    else item = created;
    item->value = copy;
    item->owner = service;
    strcpy(item->app_id, id);
    strcpy(item->id, value->id);
    *item->value = *value;
    strcpy(item->value->app_id, id);
    item->next = notifications;
    notifications = item;
    ++notification_revision;
    unlock();
    if (!value->ongoing && !resident) epd_sound_notify();
    return true;
}
bool epd_notification_publish(epd_service_t *service, const epd_notification_t *value)
{ return notification_publish(service, value, false); }
bool epd_notification_publish_resident(const epd_notification_t *value)
{ return notification_publish(NULL, value, true); }
RTM_EXPORT(epd_notification_publish);

uint32_t epd_notification_revision(void)
{
    if (!lock()) return 0;
    uint32_t revision = notification_revision;
    unlock();
    return revision;
}
RTM_EXPORT(epd_notification_revision);

size_t epd_notification_read(size_t offset, epd_notification_t *out, size_t capacity,
                             size_t *total, uint32_t *revision)
{
    size_t count = 0, copied = 0;
    if (total) *total = 0;
    if (revision) *revision = 0;
    if ((!out && capacity) || !lock()) return 0;
    for (notification_t *item = notifications; item; item = item->next)
    {
        if (!item->value) continue;
        if (count >= offset && copied < capacity) out[copied++] = *item->value;
        ++count;
    }
    if (total) *total = count;
    if (revision) *revision = notification_revision;
    unlock();
    return copied;
}
RTM_EXPORT(epd_notification_read);

bool epd_notification_find(const char *app_id, const char *id, epd_notification_t *out)
{
    if (!app_id || !id || !out || !lock()) return false;
    notification_t *item = find(app_id, id);
    bool found = item && item->value;
    if (found) *out = *item->value;
    unlock();
    return found;
}
RTM_EXPORT(epd_notification_find);

bool epd_notification_dismiss(const char *app_id, const char *id)
{
    if (!app_id || !id || !lock()) return false;
    notification_t *item = find(app_id, id);
    if (item && item->value)
    {
        if (!reminder_dismiss(app_id, id)) { unlock(); return false; }
        rt_free(item->value);
        item->value = NULL;
        ++notification_revision;
    }
    bool found = item != NULL;
    unlock();
    return found;
}
RTM_EXPORT(epd_notification_dismiss);

size_t epd_notification_app_ids(char (*ids)[EPD_SYSTEM_APP_ID_SIZE], size_t capacity)
{
    if (!ids || !capacity || !lock()) return 0;
    size_t count = 0;
    for (notification_t *item = notifications; item && count < capacity; item = item->next)
    {
        if (!item->value) continue;
        size_t index;
        for (index = 0; index < count; ++index)
            if (!strcmp(ids[index], item->app_id)) break;
        if (index == count) strcpy(ids[count++], item->app_id);
    }
    unlock();
    return count;
}
RTM_EXPORT(epd_notification_app_ids);

static void clear_commands(void)
{
    while (commands)
    {
        media_command_t *next = commands->next;
        rt_free(commands);
        commands = next;
    }
    commands_tail = NULL;
    sleep_guard_release(&commands_hold);
}

static void clear_media(void)
{
    clear_commands();
    if (media_owner) ++media_revision;
    media_owner = NULL;
    media_item = 0;
    media_quiet = false;
    media_navigation[0] = 0;
    memset(&media, 0, sizeof(media));
}

static bool publish_media(epd_service_t *service, const epd_media_t *value,
                           uint64_t item, bool quiet, bool overlay, epd_service_t *requester)
{
    if (value && (!memchr(value->artist, 0, sizeof(value->artist)) ||
                  !memchr(value->title, 0, sizeof(value->title)))) return false;
    if (!lock()) return false;
    char id[EPD_SYSTEM_APP_ID_SIZE];
    if (!epd_service_identity(service, id, sizeof(id))) { unlock(); return false; }
    char navigation[EPD_SYSTEM_APP_ID_SIZE];
    strcpy(navigation, id);
    if (overlay && value)
    {
        if (!lease.active || lease.invalid || (media_overlay && media_overlay != service))
        { unlock(); return false; }
        if (!requester || !epd_service_identity(requester, navigation, sizeof(navigation)))
        { unlock(); return false; }
        media_overlay = service;
    }
    if (lease.active && service == lease.owner)
    {
        if (!value || lease.item != item || strcmp(lease.value.title, value->title))
            lease.invalid = true;
        if (value)
        {
            lease.value = *value;
            strcpy(lease.value.app_id, id);
            lease.quiet = !value->playing && quiet;
        }
        else lease.owner = NULL;
        if (media_owner != service && !lease.invalid) { unlock(); return true; }
    }
    else if (lease.active && value && service != media_overlay)
        lease.invalid = true;
    if (!value)
    {
        if (intent_pending_owner == service) intent_pending_owner = NULL;
        if (media_owner == service) clear_media();
        if (media_overlay == service) media_overlay = NULL;
    }
    else if (media_owner != service || strcmp(media.artist, value->artist) ||
             strcmp(media.title, value->title) || media.playing != value->playing ||
             media.audio_type != value->audio_type ||
             strcmp(media_navigation, navigation) ||
             media.capabilities != value->capabilities)
    {
        if (media_owner != service) clear_commands();
        media_owner = service;
        media = *value;
        strcpy(media.app_id, id);
        strcpy(media_navigation, navigation);
        ++media_revision;
    }
    if (value && media_owner == service)
    {
        media_item = item;
        media_quiet = !value->playing && quiet;
    }
    unlock();
    return true;
}

bool epd_media_publish(epd_service_t *service, const epd_media_t *value)
{ return publish_media(service, value, 0, false, false, NULL); }
RTM_EXPORT(epd_media_publish);

bool epd_media_publish_state(epd_service_t *service, const epd_media_t *value,
                             uint64_t item, bool quiet)
{ return publish_media(service, value, item, quiet, false, NULL); }
RTM_EXPORT(epd_media_publish_state);

bool epd_media_publish_overlay(epd_service_t *service, epd_service_t *requester,
                               const epd_media_t *value, bool quiet)
{ return publish_media(service, value, 0, quiet, true, requester); }

static void note_intent(epd_service_t *service)
{
    if (service == media_overlay) return;
    ++media_intent;
    intent_pending_owner = service;
    if (service == media_owner) media_quiet = false;
    if (lease.active) lease.invalid = true;
}

void epd_media_note_intent(epd_service_t *service)
{
    if (!service || !lock()) return;
    char id[EPD_SYSTEM_APP_ID_SIZE];
    if (epd_service_identity(service, id, sizeof(id))) note_intent(service);
    unlock();
}
RTM_EXPORT(epd_media_note_intent);

void epd_media_intent_settled(epd_service_t *service)
{
    if (!service || !lock()) return;
    if (intent_pending_owner == service && !commands) intent_pending_owner = NULL;
    unlock();
}
RTM_EXPORT(epd_media_intent_settled);

bool epd_media_read_display(epd_media_t *out, uint32_t *revision,
                            char *navigation_id, size_t capacity)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (revision) *revision = 0;
    if (navigation_id && capacity) navigation_id[0] = 0;
    if (!lock()) return false;
    if (navigation_id && capacity <= strlen(media_navigation)) { unlock(); return false; }
    *out = media;
    if (navigation_id) strcpy(navigation_id, media_navigation);
    if (revision) *revision = media_revision;
    bool available = media_owner != NULL;
    unlock();
    return available;
}
bool epd_media_read(epd_media_t *out, uint32_t *revision)
{ return epd_media_read_display(out, revision, NULL, 0); }
RTM_EXPORT(epd_media_read);

static bool queue_command(epd_service_t *owner, epd_media_command_t command,
                           bool automatic, uint64_t item_id, uint64_t intent)
{
    media_command_t *item = rt_malloc(sizeof(*item));
    if (!item) return false;
    if (!sleep_guard_acquire(&commands_hold))
    {
        rt_free(item);
        return false;
    }
    item->value = command;
    item->owner = owner;
    item->automatic = automatic;
    item->item = item_id;
    item->intent = intent;
    item->next = NULL;
    if (commands_tail) commands_tail->next = item;
    else commands = item;
    commands_tail = item;
    epd_service_wake(owner);
    return true;
}

bool epd_media_send(const char *app_id, epd_media_command_t command)
{
    if (!app_id || command < EPD_MEDIA_PREVIOUS || command > EPD_MEDIA_STOP || !lock()) return false;
    char id[EPD_SYSTEM_APP_ID_SIZE];
    if (!media_owner || strcmp(media.app_id, app_id) ||
        !(media.capabilities & (1u << command)) ||
        !epd_service_identity(media_owner, id, sizeof(id))) { unlock(); return false; }
    bool ok = queue_command(media_owner, command, false, 0, 0);
    if (ok) note_intent(media_owner);
    unlock();
    return ok;
}
RTM_EXPORT(epd_media_send);

bool epd_media_poll(epd_service_t *service, epd_media_command_t *command)
{
    if (!command || !lock()) return false;
    char id[EPD_SYSTEM_APP_ID_SIZE];
    if (!commands || service != media_owner || !epd_service_identity(service, id, sizeof(id)))
    { unlock(); return false; }
    /* A queued automatic restore can be superseded before the worker runs. */
    while (commands && commands->automatic &&
           (commands->owner != service || commands->item != media_item ||
            commands->intent != media_intent ||
            (commands->value == EPD_MEDIA_PAUSE && (!lease.active || lease.invalid))))
    {
        media_command_t *stale = commands;
        commands = stale->next;
        rt_free(stale);
    }
    if (!commands)
    {
        commands_tail = NULL;
        sleep_guard_release(&commands_hold);
        unlock(); return false;
    }
    if (commands->automatic && commands->value == EPD_MEDIA_PLAY && !media_quiet)
    { unlock(); return false; }
    /* Transfer responsibility before removing the command. An empty poll never
     * acquires a service hold; the consumer releases after execution/cleanup. */
    if (!epd_service_sleep_acquire(service)) { unlock(); return false; }
    media_command_t *item = commands;
    commands = item->next;
    if (!commands) commands_tail = NULL;
    *command = item->value;
    if (item->automatic && item->value == EPD_MEDIA_PAUSE && lease.active)
        lease.pause_consumed = true;
    rt_free(item);
    if (!commands) sleep_guard_release(&commands_hold);
    unlock();
    return true;
}
RTM_EXPORT(epd_media_poll);

bool epd_media_lease_begin(void)
{
    if (!lock()) return false;
    if (lease.active || media_overlay || commands || intent_pending_owner)
    { unlock(); return false; }
    memset(&lease, 0, sizeof(lease));
    lease.active = true;
    lease.owner = media_owner;
    lease.value = media;
    lease.item = media_item;
    lease.intent = media_intent;
    lease.was_playing = media.playing ||
        (!media_quiet && (media.capabilities & EPD_MEDIA_CAN_PAUSE));
    lease.quiet = !media_owner || media_quiet;
    if (media_owner && !lease.quiet)
    {
        if (!(media.capabilities & EPD_MEDIA_CAN_PAUSE) ||
            !queue_command(media_owner, EPD_MEDIA_PAUSE, true, media_item, media_intent))
        { memset(&lease, 0, sizeof(lease)); unlock(); return false; }
    }
    unlock(); return true;
}

static bool lease_valid(void)
{
    char id[EPD_SYSTEM_APP_ID_SIZE];
    return lease.active && !lease.invalid && lease.intent == media_intent &&
        (!lease.owner || epd_service_identity(lease.owner, id, sizeof(id)));
}

bool epd_media_lease_valid(void)
{
    if (!lock()) return false;
    bool valid = lease_valid();
    unlock(); return valid;
}

bool epd_media_lease_ready(void)
{
    if (!lock()) return false;
    bool ready = lease_valid() && (!lease.owner || lease.quiet);
    unlock(); return ready;
}

void epd_media_lease_end(bool restore)
{
    if (!lock()) return;
    if (!lease.active) { unlock(); return; }
    char id[EPD_SYSTEM_APP_ID_SIZE];
    bool valid = !lease.invalid && lease.owner && lease.intent == media_intent &&
        epd_service_identity(lease.owner, id, sizeof(id));
    /* Remove an unconsumed pause, including on acquisition cancellation. */
    media_command_t **link = &commands;
    commands_tail = NULL;
    while (*link)
    {
        media_command_t *item = *link;
        if (item->automatic && item->value == EPD_MEDIA_PAUSE)
        { *link = item->next; rt_free(item); }
        else { commands_tail = item; link = &item->next; }
    }
    if (!commands) sleep_guard_release(&commands_hold);
    if (valid && (!media_owner || media_owner == media_overlay || media_owner == lease.owner))
    {
        if (media_owner != lease.owner) clear_commands();
        media_owner = lease.owner;
        media = lease.value;
        strcpy(media_navigation, lease.value.app_id);
        media_item = lease.item;
        media_quiet = lease.quiet;
        ++media_revision;
        if (restore && lease.was_playing && lease.pause_consumed)
            queue_command(lease.owner, EPD_MEDIA_PLAY, true, lease.item, lease.intent);
    }
    media_overlay = NULL;
    memset(&lease, 0, sizeof(lease));
    unlock();
}

void epd_system_service_stopped(epd_service_t *service)
{
    if (!service || !lock()) return;
    if (intent_pending_owner == service) intent_pending_owner = NULL;
    if (lease.owner == service) { lease.owner = NULL; lease.invalid = true; }
    if (media_overlay == service) media_overlay = NULL;
    if (media_owner == service) clear_media();
    notification_t **link = &notifications;
    while (*link)
    {
        notification_t *item = *link;
        if (item->owner != service) { link = &item->next; continue; }
        item->owner = NULL;
        if (!item->value || item->value->ongoing)
        {
            *link = item->next;
            if (item->value) ++notification_revision;
            rt_free(item->value);
            rt_free(item);
        }
        else link = &item->next;
    }
    unlock();
}

void epd_system_remove_app(const char *app_id)
{
    if (!app_id || !lock()) return;
    char pending_id[EPD_SYSTEM_APP_ID_SIZE];
    if (intent_pending_owner && (!epd_service_identity(intent_pending_owner, pending_id, sizeof(pending_id)) ||
        !strcmp(pending_id, app_id))) intent_pending_owner = NULL;
    if (lease.owner && !strcmp(lease.value.app_id, app_id))
    { lease.owner = NULL; lease.invalid = true; }
    if (media_owner == media_overlay && !strcmp(media.app_id, app_id)) media_overlay = NULL;
    if (media_owner && !strcmp(media.app_id, app_id)) clear_media();
    notification_t **link = &notifications;
    while (*link)
    {
        notification_t *item = *link;
        if (strcmp(item->app_id, app_id)) { link = &item->next; continue; }
        *link = item->next;
        if (item->value) ++notification_revision;
        rt_free(item->value);
        rt_free(item);
    }
    unlock();
}

static bool contains_app(const char (*ids)[EPD_SYSTEM_APP_ID_SIZE], size_t count, const char *id)
{
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(ids[i], id)) return true;
    return false;
}

void epd_system_reconcile_apps(const char (*ids)[EPD_SYSTEM_APP_ID_SIZE], size_t count)
{
    if ((!ids && count) || !lock()) return;
    char pending_id[EPD_SYSTEM_APP_ID_SIZE];
    if (intent_pending_owner && (!epd_service_identity(intent_pending_owner, pending_id, sizeof(pending_id)) ||
        !contains_app(ids, count, pending_id))) intent_pending_owner = NULL;
    if (lease.owner && !contains_app(ids, count, lease.value.app_id))
    { lease.owner = NULL; lease.invalid = true; }
    if (media_owner && media_owner != media_overlay && !contains_app(ids, count, media.app_id)) clear_media();
    notification_t **link = &notifications;
    while (*link)
    {
        notification_t *item = *link;
        if (contains_app(ids, count, item->app_id)) { link = &item->next; continue; }
        *link = item->next;
        if (item->value) ++notification_revision;
        rt_free(item->value);
        rt_free(item);
    }
    unlock();
}
