#include "storage_internal.h"
#include "portal_http.h"
#include "portal.h"
#include "portal_protocol.h"
#include "portal_auth.h"
#include "portal_assets.h"
#include "portal_files.h"
#include "portal_package.h"
#include "portal_service_config.h"
#include "reading_font.h"
#include "epd_memory.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "system_info.h"
#include "device_identity.h"
#include "boards/battery/battery.h"
#include "boards/board_service.h"
#include "wlan_service.h"
#include "platform/app_catalog.h"
#include "platform/app_installer.h"
#include "platform/app_memory_usage.h"
#include <dfs_posix.h>
#include <cJSON.h>
#include <rthw.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <netdev.h>
/* Keep file descriptors on DFS; socket operations use the lwIP entry points. */
#undef read
#undef write
#undef close
#undef fcntl
#undef ioctl

#define IO_SIZE 32768
#define RESPONSE_SIZE 32768
typedef struct
{
    portal_http_config_t config;
    portal_request_t request;
    int socket;
    char header[PORTAL_HEADER_MAX], path[PORTAL_PATH_MAX], relative[PORTAL_PATH_MAX];
    unsigned char io[IO_SIZE];
    char response[RESPONSE_SIZE];
    size_t initial, consumed, header_bytes;
    uint32_t session;
    uint32_t peer;
    bool auth_checked;
} connection_t;
static bool files_blocked;
static unsigned file_users;
static unsigned upload_serial;

void portal_http_storage_block(bool blocked)
{
    rt_base_t level=rt_hw_interrupt_disable(); files_blocked=blocked; rt_hw_interrupt_enable(level);
    portal_package_block(blocked);
}
bool portal_http_storage_idle(void)
{
    rt_base_t level=rt_hw_interrupt_disable(); bool idle=file_users==0; rt_hw_interrupt_enable(level);
    return idle && portal_package_storage_idle();
}
static bool cancelled(connection_t *c)
{
    return epd_service_cancelled(c->config.service) || board_power_pending() || system_reset_busy() ||
        (c->config.keep_running && !c->config.keep_running()) ||
        (c->auth_checked && portal_auth_session(c->request.session_token,NULL,false,NULL)!=PORTAL_AUTH_OK);
}
static bool card_ready(connection_t *c)
{ return !files_blocked && !storage_changing() && !storage_release_requested() && storage_session_valid(STORAGE_SD,c->session); }
static bool file_begin(connection_t *c)
{
    if (app_installer_busy()) return false;
    rt_base_t level=rt_hw_interrupt_disable(); bool ok=!files_blocked;
    if (ok) ++file_users;
    rt_hw_interrupt_enable(level);
    if (!ok) return false;
    c->session=storage_card_session();
    if (card_ready(c)) return true;
    level=rt_hw_interrupt_disable(); --file_users; rt_hw_interrupt_enable(level); return false;
}
static void file_end(void)
{ rt_base_t level=rt_hw_interrupt_disable(); --file_users; rt_hw_interrupt_enable(level); }
static int socket_io(connection_t *c,void *buffer,size_t size,bool writing)
{
    rt_tick_t start=rt_tick_get();
    const rt_tick_t idle_timeout=rt_tick_from_millisecond(15000);
    while (!cancelled(c))
    {
        int n=writing?send(c->socket,buffer,size,0):recv(c->socket,buffer,size,0);
        int error=errno;
        if (n>=0) return n;
        if (error!=EINTR && error!=EAGAIN && error!=EWOULDBLOCK) return -1;
        if ((rt_tick_t)(rt_tick_get()-start)>=idle_timeout)
        { errno=ETIMEDOUT; return -1; }
        /* The short socket timeout bounds cancellation latency. A
         * temporary timeout is not EOF; wait for progress within this limit. */
        rt_thread_mdelay(1);
    }
    errno=ECANCELED;
    return -1;
}
static bool send_all(connection_t *c,const void *data,size_t length)
{
    const unsigned char *p=data;
    while (length && !cancelled(c))
    {
        size_t chunk=length>IO_SIZE?IO_SIZE:length;
        int n=socket_io(c,(void *)p,chunk,true);
        if (n<=0)
        {
            int error=errno;
            rt_kprintf("portal: send failed rc=%d errno=%d remaining=%lu\n",n,error,(unsigned long)length);
            return false;
        }
        p+=n; length-=n;
    }
    return !length;
}
static bool headers(connection_t *c,int status,const char *type,uint64_t length,const char *extra)
{
    char head[1400];
    int n=snprintf(head,sizeof(head),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n%s\r\n",
        status,status<300?"OK":"Error",type,(unsigned long long)length,extra?extra:"");
    return n>0 && n<(int)sizeof(head) && send_all(c,head,(size_t)n);
}
static bool reply(connection_t *c,int status,const char *body,const char *extra)
{
    char error[96];
    if (status>=400 && (!body || !strcmp(body,"{}")))
    {
        const char *code=status==400?"invalid_request":status==401?"unauthenticated":status==403?"forbidden":
            status==404?"not_found":status==409?"conflict":status==413?"payload_too_large":
            status==417?"expectation_failed":status==431?"headers_too_large":status==503?"unavailable":
            status==507?"storage_full":"internal_error";
        snprintf(error,sizeof(error),"{\"error\":\"%s\"}",code); body=error;
    }
    size_t n=body?strlen(body):0;
    return headers(c,status,"application/json; charset=utf-8",n,extra) && (!n || send_all(c,body,n));
}
static void failure(connection_t *c,int status)
{ reply(c,status,"{}",NULL); }
static bool read_header(connection_t *c)
{
    size_t n=0;
    while (n<PORTAL_HEADER_MAX-1 && !cancelled(c))
    {
        int got=socket_io(c,c->header+n,PORTAL_HEADER_MAX-1-n,false);
        if (got<=0) return false; n+=(size_t)got; c->header[n]=0;
        char *end=strstr(c->header,"\r\n\r\n");
        if (end)
        {
            c->header_bytes=(size_t)(end-c->header)+4;
            c->initial=n-c->header_bytes; c->consumed=0;
            /* Parsing terminates the header at its end; retain any body bytes. */
            if (c->initial) memcpy(c->io,c->header+c->header_bytes,c->initial);
            int error=portal_request_parse(c->header,c->header_bytes,&c->request);
            if (error) { failure(c,error); return false; }
            if (c->initial>c->request.length) { failure(c,400); return false; }
            return true;
        }
    }
    failure(c,431); return false;
}
static int receive(connection_t *c,void *buffer,size_t size)
{
    if (cancelled(c)) return -1;
    if (c->consumed<c->initial)
    {
        size_t n=c->initial-c->consumed; if (n>size) n=size;
        memcpy(buffer,c->io+c->consumed,n); c->consumed+=n; return (int)n;
    }
    return socket_io(c,buffer,size,false);
}
static bool json_depth_bounded(const char *text,size_t length)
{
    unsigned depth=0; bool quoted=false;
    for (size_t i=0;i<length;++i)
    {
        char ch=text[i];
        if (quoted)
        {
            if (ch=='\\') { if (++i==length) return false; }
            else if (ch=='"') quoted=false;
        }
        else if (ch=='"') quoted=true;
        else if (ch=='{' || ch=='[') { if (++depth>16) return false; }
        else if (ch=='}' || ch==']') { if (!depth) return false; --depth; }
    }
    return !quoted && !depth;
}
static cJSON *body_json(connection_t *c)
{
    if (!c->request.has_length || c->request.length>=PORTAL_JSON_MAX) { failure(c,413); return NULL; }
    if (c->request.expect_continue && !send_all(c,"HTTP/1.1 100 Continue\r\n\r\n",25)) return NULL;
    size_t n=0;
    while (n<c->request.length)
    { int got=receive(c,c->response+n,(size_t)c->request.length-n); if (got<=0) return NULL; n+=(size_t)got; }
    c->response[n]=0;
    if (cancelled(c)) return NULL;
    if (memchr(c->response,0,n)) { failure(c,400); return NULL; }
    /* Bound recursive parsing before cJSON uses the service thread's stack. */
    if (!json_depth_bounded(c->response,n)) { failure(c,400); return NULL; }
    cJSON *json=cJSON_ParseWithOpts(c->response,NULL,1);
    if (!json) failure(c,400);
    return json;
}
static void auth_failure(connection_t *c,portal_auth_error_t error,unsigned retry)
{
    int status=error==PORTAL_AUTH_RATE_LIMITED?429:
        error==PORTAL_AUTH_BAD_CREDENTIALS||error==PORTAL_AUTH_UNAUTHENTICATED?401:
        error==PORTAL_AUTH_CSRF_INVALID||error==PORTAL_AUTH_SETUP_CLOSED?403:
        error==PORTAL_AUTH_INVALID?400:error==PORTAL_AUTH_BUSY||error==PORTAL_AUTH_ACCOUNT_EXISTS||error==PORTAL_AUTH_ACCOUNT_LIMIT?409:503;
    char body[128],extra[64]="";
    snprintf(body,sizeof(body),"{\"error\":\"%s\"}",portal_auth_error_code(error));
    if (status==429) snprintf(extra,sizeof(extra),"Retry-After: %u\r\n",retry?retry:1);
    reply(c,status,body,extra);
}
static void auth_response(connection_t *c,const portal_auth_session_t *session,bool set_cookie)
{
    portal_json_t json; portal_json_init(&json,c->response,sizeof(c->response));
    portal_json_raw(&json,"{\"authenticated\":true,\"username\":"); portal_json_string(&json,session->username);
    portal_json_raw(&json,",\"csrf\":"); portal_json_string(&json,session->csrf); portal_json_raw(&json,"}");
    char cookie[192]="";
    if (set_cookie) snprintf(cookie,sizeof(cookie),"Set-Cookie: epd_session=%s; Path=/; HttpOnly; SameSite=Strict\r\n",session->token);
    reply(c,200,c->response,cookie);
}
static void auth_status(connection_t *c)
{
    portal_auth_snapshot_t state; portal_auth_snapshot(&state);
    /* The device-only snapshot also carries the setup code. Discard it before
     * building the deliberately smaller public status response. */
    memset(state.setup_code,0,sizeof(state.setup_code));
    if (!state.initialized || state.error!=PORTAL_AUTH_OK) { auth_failure(c,state.error,0); return; }
    portal_auth_session_t session;
    bool logged_in=portal_auth_session(c->request.session_token,NULL,false,&session)==PORTAL_AUTH_OK;
    portal_json_t json; portal_json_init(&json,c->response,sizeof(c->response));
    portal_json_raw(&json,state.configured?"{\"configured\":true":"{\"configured\":false");
    portal_json_raw(&json,state.setup_open?",\"setup_open\":true":",\"setup_open\":false");
    portal_json_raw(&json,logged_in?",\"authenticated\":true,\"username\":":",\"authenticated\":false,\"username\":");
    portal_json_string(&json,logged_in?session.username:""); portal_json_raw(&json,"}");
    reply(c,200,c->response,NULL); memset(&session,0,sizeof(session));
}
static void login(connection_t *c,bool setup)
{
    if (strcmp(c->request.content_type,"application/json") && strcmp(c->request.content_type,"application/json; charset=utf-8"))
    { auth_failure(c,PORTAL_AUTH_INVALID,0); return; }
    cJSON *body=body_json(c); if (!body) return;
    bool valid=cJSON_IsObject(body); unsigned seen=0;
    for (cJSON *item=body->child; item && valid; item=item->next)
    {
        unsigned bit=item->string&&!strcmp(item->string,"username")?1:item->string&&!strcmp(item->string,"password")?2:
            setup&&item->string&&!strcmp(item->string,"code")?4:0;
        if (!bit || (seen&bit) || !cJSON_IsString(item)) valid=false;
        seen|=bit;
    }
    for (const char *p=c->response; *p; ++p)
        if (*p=='\\' && p[1]) { if (!strncmp(p+1,"u0000",5)) valid=false; ++p; }
    valid=valid && seen==(setup?7u:3u);
    portal_auth_session_t session={0}; unsigned retry=0;
    portal_auth_error_t error=PORTAL_AUTH_INVALID;
    if (valid)
    {
        const char *username=cJSON_GetObjectItemCaseSensitive(body,"username")->valuestring;
        const char *password=cJSON_GetObjectItemCaseSensitive(body,"password")->valuestring;
        error=setup?portal_auth_setup(c->peer,username,password,cJSON_GetObjectItemCaseSensitive(body,"code")->valuestring,&session,&retry):
            portal_auth_login(c->peer,username,password,&session,&retry);
    }
    for (cJSON *item=body->child; item; item=item->next)
        if (cJSON_IsString(item)) { volatile char *p=item->valuestring; size_t n=strlen(item->valuestring); while (n--) *p++=0; }
    cJSON_Delete(body); memset(c->response,0,PORTAL_JSON_MAX);
    if (error==PORTAL_AUTH_OK) auth_response(c,&session,true); else auth_failure(c,error,retry);
    memset(&session,0,sizeof(session));
}
static void storage_json(portal_json_t *j)
{
    storage_info_t info={0}; storage_info(STORAGE_SD,&info);
    portal_json_raw(j,info.mounted?"{\"mounted\":true,\"total\":" : "{\"mounted\":false,\"total\":");
    portal_json_number(j,(int64_t)info.total); portal_json_raw(j,",\"used\":"); portal_json_number(j,(int64_t)info.used);
    portal_json_raw(j,files_blocked?",\"busy\":true}":",\"busy\":false}");
}
static void string_field(portal_json_t *j,const char *name,const char *value)
{ portal_json_string(j,name); portal_json_raw(j,":"); portal_json_string(j,value); }
static void info(connection_t *c)
{
    system_info_t data={0}; system_info_read(&data);
    struct netdev *ap=netdev_get_by_name("w1");
    if (!data.mac[0] && ap && ap->hwaddr_len==6)
        snprintf(data.mac,sizeof(data.mac),"%02X:%02X:%02X:%02X:%02X:%02X",
            ap->hwaddr[0],ap->hwaddr[1],ap->hwaddr[2],ap->hwaddr[3],ap->hwaddr[4],ap->hwaddr[5]);
    char name[DEVICE_IDENTITY_NAME_CAPACITY]=""; device_identity_get_name(name,sizeof(name));
    battery_snapshot_t battery; battery_get_snapshot(&battery);
    portal_json_t j; portal_json_init(&j,c->response,sizeof(c->response));
    portal_json_raw(&j,"{"); string_field(&j,"name",name); portal_json_raw(&j,","); string_field(&j,"model",data.model);
    portal_json_raw(&j,","); string_field(&j,"sdk",data.sdk); portal_json_raw(&j,","); string_field(&j,"version",data.ui_version);
    portal_json_raw(&j,","); string_field(&j,"build_date",data.build_date); portal_json_raw(&j,","); string_field(&j,"mac",data.mac);
    portal_json_raw(&j,","); string_field(&j,"bluetooth",data.bt); portal_json_raw(&j,",\"uptime\":"); portal_json_number(&j,(int64_t)data.uptime_minutes);
    portal_json_raw(&j,",\"display\":{\"width\":684,\"height\":1216,\"grays\":16},\"battery\":{\"percent\":");
    if (battery.percentage_valid) portal_json_number(&j,battery.percentage); else portal_json_raw(&j,"null");
    portal_json_raw(&j,battery.charge_valid&&battery.charging?",\"charging\":true}":",\"charging\":false}");
    portal_json_raw(&j,",\"storage\":"); storage_json(&j);
    portal_json_raw(&j,",\"addresses\":[");
    for (unsigned i=0;i<data.address_count;++i) { if (i) portal_json_raw(&j,","); portal_json_string(&j,data.addresses[i]); }
    if (data.address_count) portal_json_raw(&j,",");
    portal_json_string(&j,c->config.address);
    portal_json_raw(&j,"],\"wifi\":{");
    wlan_snapshot_t wlan={0}; wlan_service_snapshot(&wlan); string_field(&j,"ssid",wlan.connected?wlan.current.id.ssid:""); wlan_service_snapshot_free(&wlan);
    portal_json_raw(&j,"}");
    app_memory_snapshot_t memory;
    if (app_memory_snapshot(&memory))
    {
        portal_json_raw(&j,",\"memory\":[");
        for (unsigned i=0;i<2;++i)
        {
            if (i) portal_json_raw(&j,",");
            portal_json_raw(&j,"{\"total\":"); portal_json_number(&j,memory.ram[i].total);
            portal_json_raw(&j,",\"available\":"); portal_json_number(&j,memory.ram[i].available);
            portal_json_raw(&j,"}");
        }
        portal_json_raw(&j,"]");
    }
    portal_json_raw(&j,"}"); reply(c,j.failed?500:200,j.failed?"{}":c->response,NULL);
}
static bool requested_path(connection_t *c)
{
    return portal_query(c->request.target,"path",c->relative,sizeof(c->relative)) && portal_path(c->relative,c->path,sizeof(c->path));
}
static bool unsigned_query(connection_t *c,const char *key,unsigned fallback,unsigned limit,unsigned *out)
{
    char value[24]; *out=fallback;
    int result=portal_query_value(c->request.target,key,value,sizeof(value));
    if (!result) return true;
    if (result<0) return false;
    if (!*value) return false;
    uint64_t n=0; for (char *p=value;*p;++p) { if (*p<'0'||*p>'9') return false; n=n*10+(unsigned)(*p-'0'); if (n>limit) return false; }
    *out=(unsigned)n; return true;
}
static void list(connection_t *c)
{
    unsigned offset,limit;
    if (!requested_path(c) || !unsigned_query(c,"offset",0,1000000,&offset) || !unsigned_query(c,"limit",40,40,&limit) || !limit) { failure(c,400); return; }
    portal_json_t j; portal_json_init(&j,c->response,sizeof(c->response));
    portal_json_raw(&j,"{\"storage\":"); storage_json(&j); portal_json_raw(&j,",\"entries\":[");
    if (!file_begin(c)) { portal_json_raw(&j,"],\"more\":false}"); reply(c,200,c->response,NULL); return; }
    storage_lock(); DIR *directory=card_ready(c)?opendir(c->path):NULL; storage_unlock();
    if (!directory) { file_end(); failure(c,404); return; }
    unsigned skipped=0,count=0; bool more=false;
    for (;;)
    {
        struct stat st={0}; char name[256], full[PORTAL_PATH_MAX]; bool valid=false, ended=false, writable=false;
        storage_lock();
        if (!card_ready(c) || cancelled(c)) ended=true;
        else
        {
            struct dirent *entry=readdir(directory);
            if (!entry) ended=true;
            else if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..") && strncmp(entry->d_name,".portal-upload-",15))
            {
                size_t n=strlen(entry->d_name);
                if (n<sizeof(name))
                {
                    memcpy(name,entry->d_name,n+1);
                    int bytes=snprintf(full,sizeof(full),"%s/%s",c->path,name);
                    char relative[PORTAL_PATH_MAX],checked[PORTAL_PATH_MAX];
                    int relative_bytes=snprintf(relative,sizeof(relative),"%s%s%s",c->relative,!strcmp(c->relative,"/")?"":"/",name);
                    valid=bytes>0 && bytes<(int)sizeof(full) && relative_bytes>0 && relative_bytes<(int)sizeof(relative) &&
                        portal_path(relative,checked,sizeof(checked)) && stat(full,&st)==0;
                    if (valid) writable=portal_path_mutable(relative);
                }
            }
        }
        storage_unlock();
        if (ended) break;
        if (!valid || skipped++<offset) continue;
        if (count==limit) { more=true; break; }
        if (count++) portal_json_raw(&j,",");
        portal_json_raw(&j,"{"); string_field(&j,"name",name); portal_json_raw(&j,S_ISDIR(st.st_mode)?",\"directory\":true":",\"directory\":false");
        portal_json_raw(&j,writable?",\"writable\":true":",\"writable\":false");
        portal_json_raw(&j,",\"size\":"); portal_json_number(&j,st.st_size); portal_json_raw(&j,",\"modified\":"); portal_json_number(&j,st.st_mtime); portal_json_raw(&j,"}");
        if (j.failed) break;
    }
    storage_lock(); closedir(directory); bool ready=card_ready(c); storage_unlock(); file_end();
    if (!ready) { failure(c,503); return; }
    portal_json_raw(&j,more?"],\"more\":true}":"],\"more\":false}");
    reply(c,j.failed?500:200,j.failed?"{}":c->response,NULL);
}
static void download(connection_t *c)
{
    if (!requested_path(c)) { failure(c,400); return; }
    if (!file_begin(c)) { failure(c,503); return; }
    struct stat st={0}; storage_lock();
    int fd=card_ready(c)&&stat(c->path,&st)==0&&S_ISREG(st.st_mode)?open(c->path,O_RDONLY,0):-1;
    storage_unlock();
    if (fd<0) { file_end(); failure(c,404); return; }
    const unsigned char *name=(const unsigned char *)strrchr(c->path,'/')+1;
    char disposition[900]="Content-Disposition: attachment; filename=\"download\"; filename*=UTF-8''";
    size_t used=strlen(disposition); const char *digits="0123456789ABCDEF";
    while (*name && used+5<sizeof(disposition)) { disposition[used++]='%'; disposition[used++]=digits[*name>>4]; disposition[used++]=digits[*name&15]; ++name; }
    memcpy(disposition+used,"\r\n",3);
    bool ok=headers(c,200,"application/octet-stream",(uint64_t)st.st_size,disposition);
    uint64_t remaining=(uint64_t)st.st_size;
    while (ok && remaining && card_ready(c) && !cancelled(c))
    {
        size_t amount=remaining>IO_SIZE?IO_SIZE:(size_t)remaining;
        storage_lock();
        bool ready=card_ready(c);
        int got=ready?read(fd,c->io,amount):-1;
        int error=errno;
        storage_unlock();
        if (got<=0)
        {
            rt_kprintf("portal: download read rc=%d errno=%d ready=%d remaining=%lu\n",got,error,ready,(unsigned long)remaining);
            break;
        }
        ok=send_all(c,c->io,(size_t)got);
        if (ok) remaining-=(unsigned)got;
    }
    if (remaining)
        rt_kprintf("portal: download interrupted remaining=%lu send_ok=%d ready=%d cancelled=%d\n",
                   (unsigned long)remaining,ok,card_ready(c),cancelled(c));
    storage_lock(); close(fd); storage_unlock(); file_end();
}
static void upload(connection_t *c)
{
    if (!requested_path(c) || !strcmp(c->relative,"/") || !c->request.has_length) { failure(c,400); return; }
    if (!portal_path_mutable(c->relative)) { failure(c,403); return; }
    if (c->request.length>INT32_MAX) { failure(c,413); return; }
    unsigned replace=0;
    if (!unsigned_query(c,"replace",0,1,&replace)) { failure(c,400); return; }
    if (!file_begin(c)) { failure(c,503); return; }
    struct stat st; char temporary[PORTAL_PATH_MAX]; strcpy(temporary,c->path);
    char *base=strrchr(temporary,'/')+1;
    size_t name_capacity=sizeof(temporary)-(size_t)(base-temporary);
    if (name_capacity<24) { file_end(); failure(c,400); return; }
    storage_lock();
    bool exists=stat(c->path,&st)==0;
    int fd=-1,status=200;
    if (!card_ready(c) || !system_reset_write_allowed()) status=503;
    else if (exists && (!replace || !S_ISREG(st.st_mode))) status=409;
    else
    {
        for (unsigned attempt=0;attempt<64;++attempt)
        {
            snprintf(base,name_capacity,".portal-upload-%08x",++upload_serial);
            fd=open(temporary,O_CREAT|O_EXCL|O_WRONLY,0);
            if (fd>=0 || (errno!=EEXIST && errno!=-EEXIST)) break;
        }
        if (fd<0) status=507;
    }
    storage_unlock();
    if (fd<0) { file_end(); failure(c,status); return; }
    if (c->request.expect_continue && !send_all(c,"HTTP/1.1 100 Continue\r\n\r\n",25)) status=400;
    uint64_t remaining=c->request.length;
    /* Preserve bytes coalesced with the HTTP header before using the I/O buffer. */
    if (status==200 && c->initial)
    {
        storage_lock(); int written=card_ready(c)&&!cancelled(c)?write(fd,c->io,c->initial):-1; storage_unlock();
        if (written!=(int)c->initial) status=507; else remaining-=c->initial;
        c->consumed=c->initial;
    }
    while (status==200 && remaining)
    {
        if (!card_ready(c) || cancelled(c)) { status=503; break; }
        int got=receive(c,c->io,remaining>IO_SIZE?IO_SIZE:(size_t)remaining);
        if (got<=0) { status=400; break; }
        storage_lock(); int written=card_ready(c)&&!cancelled(c)?write(fd,c->io,(size_t)got):-1; storage_unlock();
        if (written!=got) { status=507; break; } remaining-=(unsigned)got;
    }
    storage_lock();
    if (status==200 && cancelled(c)) status=403;
    if (status==200 && fsync(fd)!=0) status=507;
    if (close(fd)!=0 && status==200) status=507;
    if (status==200 && (!card_ready(c) || !system_reset_write_allowed() || cancelled(c))) status=503;
    if (status==200 && !replace && stat(c->path,&st)==0) status=409;
    if (status==200)
    {
        /* Configuration-file recovery owns fixed .old names. User files do
         * not: reserve a separate backup name without removing existing data. */
        char backup[PORTAL_PATH_MAX]; strcpy(backup,temporary);
        char *base=strrchr(backup,'/')+1;
        snprintf(base,sizeof(backup)-(size_t)(base-backup),".portal-backup-%08x",upload_serial);
        status=cancelled(c)?403:portal_upload_publish(temporary,c->path,backup,replace!=0);
    }
    if (status!=200 && storage_session_valid(STORAGE_SD,c->session)) unlink(temporary);
    storage_unlock(); file_end(); reply(c,status,"{}",NULL);
}
static void remove_file(connection_t *c)
{
    if (!requested_path(c) || !strcmp(c->relative,"/")) { failure(c,400); return; }
    if (!portal_path_mutable(c->relative)) { failure(c,403); return; }
    if (!file_begin(c)) { failure(c,503); return; }
    storage_lock();
    int status=card_ready(c) && system_reset_write_allowed() && !cancelled(c)?
        portal_file_remove(c->relative):503;
    storage_unlock(); file_end();
    reply(c,status,status==409?"{\"error\":\"conflict\",\"message\":\"文件正在使用，或文件夹不是空的，请先删除文件夹中的文件。\"}":"{}",NULL);
}
static void folder(connection_t *c)
{
    cJSON *body=body_json(c); if (!body) return;
    cJSON *path=cJSON_GetObjectItemCaseSensitive(body,"path");
    bool valid=cJSON_IsString(path) && portal_path(path->valuestring,c->path,sizeof(c->path)) && strcmp(path->valuestring,"/");
    bool writable=valid && portal_path_mutable(path->valuestring);
    cJSON_Delete(body);
    if (!valid) { failure(c,400); return; }
    if (!writable) { failure(c,403); return; }
    if (!file_begin(c)) { failure(c,503); return; }
    storage_lock(); int result=card_ready(c)&&system_reset_write_allowed()&&!cancelled(c)?mkdir(c->path,0):-1; storage_unlock(); file_end();
    reply(c,result==0?201:409,"{}",NULL);
}
static void package_status(connection_t *c)
{
    char id[PORTAL_PACKAGE_ID_SIZE];
    if (!portal_query(c->request.target,"upload",id,sizeof(id))) { failure(c,400); return; }
    if (!portal_package_result(id,c->response,sizeof(c->response))) { failure(c,404); return; }
    reply(c,200,c->response,NULL);
}
static void package_upload(connection_t *c)
{
    char id[PORTAL_PACKAGE_ID_SIZE],path[PORTAL_PACKAGE_PATH_MAX];
    unsigned volume;
    if (!portal_query(c->request.target,"upload",id,sizeof(id)) ||
        !unsigned_query(c,"volume",STORAGE_COUNT,STORAGE_COUNT-1,&volume) || volume>=STORAGE_COUNT ||
        !c->request.has_length || !c->request.length ||
        strcmp(c->request.content_type,"application/octet-stream"))
    { failure(c,400); return; }
    int status=portal_package_begin(id,(storage_volume_t)volume,c->request.length);
    if (status==200) { package_status(c); return; }
    if (status!=201) { failure(c,status); return; }
    status=200;
    if (c->request.expect_continue && !send_all(c,"HTTP/1.1 100 Continue\r\n\r\n",25)) status=400;
    uint64_t remaining=c->request.length;
    if (status==200 && c->initial)
    {
        status=cancelled(c)?403:portal_package_write(c->io,c->initial);
        remaining-=c->initial; c->consumed=c->initial;
    }
    while (status==200 && remaining)
    {
        int got=receive(c,c->io,remaining>IO_SIZE?IO_SIZE:(size_t)remaining);
        if (got<=0) { status=400; break; }
        status=cancelled(c)?403:portal_package_write(c->io,(size_t)got);
        remaining-=(unsigned)got;
    }
    if (status==200 && cancelled(c)) status=403;
    if (status==200) status=portal_package_finish(path);
    if (status==200 && cancelled(c)) status=403;
    if (status==200)
    {
        uint32_t ticket;
        if (portal_settings_install(path,volume,&ticket)!=RT_EOK) status=409;
    }
    if (status!=200)
    {
        portal_package_abort(status==507?"安装包写入失败，请检查存储空间。":
            status==409?"安装请求未提交：设备正在处理其他操作，请稍后重新上传。":"上传中断或目标存储不可用，安装未开始。");
        if (portal_package_result(id,c->response,sizeof(c->response))) reply(c,status,c->response,NULL);
        else failure(c,status);
        return;
    }
    if (portal_package_result(id,c->response,sizeof(c->response))) reply(c,202,c->response,NULL);
    else failure(c,500);
}
/* Reading-font API (authenticated; every PUT/POST also requires CSRF):
 * GET /api/fonts/status returns the service snapshot; /api/fonts/catalog adds
 * fonts[]. selected is the effective font, preferred is the saved choice.
 * GET /api/fonts/status?ticket=N resolves a current or retained completed job;
 * 404 font_job_unavailable means its record is no longer available.
 * PUT /api/font?name=<basename.ttf>&storage=sd|internal streams a raw body,
 * 1..32 MiB, through begin/write/finish on the destination. Default: sd.
 * POST /api/fonts/select {"id":uint32}; /api/fonts/delete additionally accepts
 * {"confirm_active":true}, required for a selected/preferred font.
 * 202 {accepted,ticket} means queued, never completed. Poll status?ticket=N
 * until done/failed; unrelated catalog scans do not replace this result.
 * An upload ticket belongs only to this connection. No client-supplied ticket
 * may write, finish or abort another connection's upload. */
static const char *font_error_message(rt_err_t error)
{
    if (error==RT_EOK) return "";
    if (error==-RT_EBUSY) return "设备正在处理其他字体操作，请稍后重试。";
    if (error==-RT_EINVAL) return "字体或请求无效；仅支持静态 TrueType .ttf。";
    if (error==-RT_ENOMEM) return "设备可用内存不足，请稍后重试。";
    if (error==-RT_EFULL) return "字体目录或目标存储空间不足。";
    if (error==-RT_EIO) return "字体读取或写入失败，请检查目标存储。";
    return "字体操作未完成，请检查字体文件和目标存储后重试。";
}
static void font_failure(connection_t *c,rt_err_t error)
{
    int status=error==-RT_EBUSY?409:error==-RT_EINVAL?400:error==-RT_EFULL?507:503;
    portal_json_t j; portal_json_init(&j,c->response,sizeof(c->response));
    portal_json_raw(&j,"{\"error\":\"font_error\",\"code\":"); portal_json_number(&j,error);
    portal_json_raw(&j,",\"message\":"); portal_json_string(&j,font_error_message(error)); portal_json_raw(&j,"}");
    reply(c,status,c->response,NULL);
}
static void font_snapshot(connection_t *c,bool catalog)
{
    static const char *phases[]={"idle","queued","receiving","copying","validating","committing","done","failed"};
    reading_font_status_t state={0};
    char value[24]; unsigned ticket=0;
    int supplied=catalog?0:portal_query_value(c->request.target,"ticket",value,sizeof(value));
    if (supplied<0 || (supplied && (!unsigned_query(c,"ticket",0,UINT32_MAX,&ticket) || !ticket)))
    { failure(c,400); return; }
    if (supplied)
    {
        if (!reading_font_job_status((uint32_t)ticket,&state))
        { reply(c,404,"{\"error\":\"font_job_unavailable\",\"message\":\"任务记录已不可用，请检查字体列表和当前实际字体。\"}",NULL); return; }
    }
    else reading_font_get_status(&state);
    portal_json_t j; portal_json_init(&j,c->response,sizeof(c->response));
    portal_json_raw(&j,state.ready?"{\"ready\":true":"{\"ready\":false");
    portal_json_raw(&j,state.busy?",\"busy\":true":",\"busy\":false");
    portal_json_raw(&j,",\"phase\":");
    portal_json_string(&j,(unsigned)state.phase<sizeof(phases)/sizeof(phases[0])?phases[state.phase]:"unknown");
    portal_json_raw(&j,",\"ticket\":"); portal_json_number(&j,state.ticket);
    portal_json_raw(&j,",\"completed\":"); portal_json_number(&j,state.completed);
    portal_json_raw(&j,",\"total\":"); portal_json_number(&j,state.total);
    portal_json_raw(&j,",\"result_id\":"); portal_json_number(&j,state.result_id);
    portal_json_raw(&j,",\"revision\":"); portal_json_number(&j,state.revision);
    portal_json_raw(&j,",\"selected\":"); portal_json_number(&j,state.selected);
    portal_json_raw(&j,",\"preferred\":"); portal_json_number(&j,state.preferred);
    portal_json_raw(&j,",\"error\":"); portal_json_number(&j,state.error);
    portal_json_raw(&j,",\"message\":"); portal_json_string(&j,font_error_message(state.error));
    if (catalog)
    {
        reading_font_info_t items[READING_FONT_LIMIT+1];
        size_t count=reading_font_list(items,sizeof(items)/sizeof(items[0]));
        if (count>sizeof(items)/sizeof(items[0])) { failure(c,500); return; }
        portal_json_raw(&j,",\"max_upload_bytes\":"); portal_json_number(&j,READING_FONT_FILE_MAX);
        portal_json_raw(&j,",\"fonts\":[");
        for (size_t i=0;i<count;++i)
        {
            const reading_font_info_t *font=&items[i];
            if (i) portal_json_raw(&j,",");
            portal_json_raw(&j,"{\"id\":"); portal_json_number(&j,font->id);
            portal_json_raw(&j,",\"name\":"); portal_json_string(&j,font->name);
            portal_json_raw(&j,",\"signature\":"); portal_json_number(&j,font->signature);
            portal_json_raw(&j,",\"size\":"); portal_json_number(&j,font->size);
            portal_json_raw(&j,",\"storage\":"); portal_json_string(&j,font->id==READING_FONT_BUILTIN?"builtin":font->volume==STORAGE_SD?"sd":"internal");
            portal_json_raw(&j,font->available?",\"available\":true":",\"available\":false");
            portal_json_raw(&j,font->selected?",\"selected\":true":",\"selected\":false");
            portal_json_raw(&j,font->preferred?",\"preferred\":true}":",\"preferred\":false}");
        }
        portal_json_raw(&j,"]");
    }
    portal_json_raw(&j,"}"); reply(c,j.failed?500:200,j.failed?"{}":c->response,NULL);
}
static void font_accepted(connection_t *c,uint32_t ticket)
{
    snprintf(c->response,sizeof(c->response),"{\"accepted\":true,\"ticket\":%lu}",(unsigned long)ticket);
    reply(c,202,c->response,NULL);
}
static void font_upload(connection_t *c)
{
    char name[256],destination[16],relative[258],checked[PORTAL_PATH_MAX];
    int storage=portal_query_value(c->request.target,"storage",destination,sizeof(destination));
    if (!storage) strcpy(destination,"sd");
    if (storage<0 || (strcmp(destination,"sd") && strcmp(destination,"internal")) ||
        !portal_query(c->request.target,"name",name,sizeof(name)) || !*name ||
        strchr(name,'/') || strchr(name,'\\') || !c->request.has_length || !c->request.length ||
        strcmp(c->request.content_type,"application/octet-stream")) { failure(c,400); return; }
    size_t n=strlen(name);
    if (n>=READING_FONT_NAME_MAX)
    { reply(c,400,"{\"error\":\"font_name_too_long\",\"message\":\"字体文件名过长，请缩短电脑上的文件名后重新上传。\"}",NULL); return; }
    if (n<5 || name[n-4]!='.' || (name[n-3]!='t' && name[n-3]!='T') ||
        (name[n-2]!='t' && name[n-2]!='T') || (name[n-1]!='f' && name[n-1]!='F')) { failure(c,400); return; }
    snprintf(relative,sizeof(relative),"/%s",name);
    if (!portal_path(relative,checked,sizeof(checked))) { failure(c,400); return; }
    if (c->request.length>READING_FONT_FILE_MAX) { failure(c,413); return; }
    if (cancelled(c) || !system_reset_write_allowed()) { failure(c,503); return; }
    uint32_t ticket=0;
    rt_err_t error=reading_font_upload_begin(name,!strcmp(destination,"sd")?STORAGE_SD:STORAGE_FLASH,
                                            (uint32_t)c->request.length,&ticket);
    if (error!=RT_EOK) { font_failure(c,error); return; }
    if (c->request.expect_continue && !send_all(c,"HTTP/1.1 100 Continue\r\n\r\n",25)) error=-RT_EIO;
    uint64_t remaining=c->request.length;
    /* Drain header-coalesced bytes before reusing the shared receive buffer. */
    if (error==RT_EOK && c->initial)
    {
        error=cancelled(c)?-RT_EIO:reading_font_upload_write(ticket,c->io,c->initial);
        remaining-=c->initial; c->consumed=c->initial;
    }
    while (error==RT_EOK && remaining)
    {
        int got=receive(c,c->io,remaining>IO_SIZE?IO_SIZE:(size_t)remaining);
        if (got<=0 || cancelled(c)) { error=-RT_EIO; break; }
        error=reading_font_upload_write(ticket,c->io,(size_t)got); remaining-=(unsigned)got;
    }
    if (error==RT_EOK && (cancelled(c) || !system_reset_write_allowed())) error=-RT_EIO;
    if (error==RT_EOK) error=reading_font_upload_finish(ticket);
    if (error!=RT_EOK) { reading_font_upload_abort(ticket); font_failure(c,error); return; }
    font_accepted(c,ticket);
}
static void font_action(connection_t *c,bool remove)
{
    if (strcmp(c->request.content_type,"application/json") && strcmp(c->request.content_type,"application/json; charset=utf-8"))
    { failure(c,400); return; }
    cJSON *body=body_json(c); if (!body) return;
    bool valid=cJSON_IsObject(body),confirmed=false; unsigned seen=0; uint32_t id=0;
    for (const char *p=c->response; *p; ++p)
        if (*p=='\\' && p[1]) { if (!strncmp(p+1,"u0000",5)) valid=false; ++p; }
    for (cJSON *item=body->child; item && valid; item=item->next)
    {
        unsigned bit=item->string&&!strcmp(item->string,"id")?1:
            remove&&item->string&&!strcmp(item->string,"confirm_active")?2:0;
        if (!bit || (seen&bit)) { valid=false; break; }
        seen|=bit;
        if (bit==1)
        {
            double value=item->valuedouble;
            valid=cJSON_IsNumber(item) && value>=0 && value<=UINT32_MAX;
            if (valid) { id=(uint32_t)value; valid=value==(double)id; }
        }
        else { valid=cJSON_IsBool(item); confirmed=cJSON_IsTrue(item); }
    }
    cJSON_Delete(body);
    if (!valid || !(seen&1)) { failure(c,400); return; }
    if (cancelled(c) || !system_reset_write_allowed()) { failure(c,503); return; }
    reading_font_info_t font;
    if (!reading_font_get(id,&font)) { failure(c,404); return; }
    if (remove && id==READING_FONT_BUILTIN) { failure(c,403); return; }
    if (!font.available) { failure(c,503); return; }
    if (remove && (font.selected || font.preferred) && !confirmed)
    { reply(c,409,"{\"error\":\"font_confirmation_required\",\"message\":\"此字体正在使用或已保存为阅读偏好，请确认删除后改用 MiSans。\"}",NULL); return; }
    uint32_t ticket=0; rt_err_t error=remove?reading_font_delete(id,&ticket):reading_font_select(id,&ticket);
    if (error!=RT_EOK) font_failure(c,error); else font_accepted(c,ticket);
}
static void service_config_read(connection_t *c, bool speech)
{
    char *text = NULL;
    int status = portal_service_config_read(speech, &text);
    reply(c, status, text, NULL); epd_app_free(text);
}
static void service_config_submit(connection_t *c, bool speech)
{
    char result[384] = "{}";
    int status = 400;
    if (!c->request.has_length || !c->request.length || c->request.length > PORTAL_SERVICE_CONFIG_MAX_BYTES)
    { status = 413; goto done; }
    if (cancelled(c) || !system_reset_write_allowed()) { status = 503; goto done; }
    if (c->request.expect_continue && !send_all(c,"HTTP/1.1 100 Continue\r\n\r\n",25)) goto done;
    size_t n = 0;
    while (n < c->request.length)
    {
        int got = receive(c,c->response+n,(size_t)c->request.length-n);
        if (got <= 0) goto done;
        n += (size_t)got;
    }
    c->response[n] = 0;
    if (cancelled(c) || !system_reset_write_allowed()) { status = 503; goto done; }
    status = portal_service_config_submit(speech,c->response,n,result,sizeof(result));
done:
    /* Body bytes may also have arrived with the HTTP header. */
    portal_service_config_wipe(c->response,sizeof(c->response));
    portal_service_config_wipe(c->io,sizeof(c->io));
    portal_service_config_wipe(c->header,sizeof(c->header));
    reply(c,status,result,NULL);
}
static void service_config_poll(connection_t *c, bool speech)
{
    unsigned ticket;
    if (!unsigned_query(c,"ticket",1,UINT32_MAX,&ticket)) { failure(c,400); return; }
    int status = portal_service_config_poll(speech,ticket,c->response,sizeof(c->response));
    reply(c,status,c->response,NULL);
}
static void handle(connection_t *c)
{
    if (!read_header(c)) return;
    char expected[64],origin[80],origin_port[80]; snprintf(expected,sizeof(expected),"%s:80",c->config.address);
    snprintf(origin,sizeof(origin),"http://%s",c->config.address); snprintf(origin_port,sizeof(origin_port),"http://%s:80",c->config.address);
    if (strcmp(c->request.host,c->config.address) && strcmp(c->request.host,expected)) { failure(c,403); return; }
    if (*c->request.origin && strcmp(c->request.origin,origin) && strcmp(c->request.origin,origin_port)) { failure(c,403); return; }
    char *query=strchr(c->request.target,'?'); size_t path_size=query?(size_t)(query-c->request.target):strlen(c->request.target);
    bool get=!strcmp(c->request.method,"GET"),post=!strcmp(c->request.method,"POST"),put=!strcmp(c->request.method,"PUT");
    bool remove=!strcmp(c->request.method,"DELETE");
    if (!get && !*c->request.origin) { failure(c,403); return; }
#define ROUTE(path) (path_size==sizeof(path)-1 && !memcmp(c->request.target,path,sizeof(path)-1))
    if (get && strncmp(c->request.target,"/api/",5))
    {
        const char *path=ROUTE("/")?"/index.html":c->request.target;
        for (unsigned i=0;i<portal_asset_count;++i) if (!strcmp(path,portal_assets[i].path))
        {
            const portal_asset_t *asset=&portal_assets[i];
            if (headers(c,200,asset->type,asset->size,"Content-Encoding: gzip\r\n")) send_all(c,asset->bytes,asset->size);
            return;
        }
        failure(c,404); return;
    }
    if (get && ROUTE("/api/auth/status")) { auth_status(c); return; }
    if (post && (ROUTE("/api/session") || ROUTE("/api/auth/setup")))
    {
        if (portal_auth_session(c->request.session_token,NULL,false,NULL)==PORTAL_AUTH_OK &&
            portal_auth_session(c->request.session_token,c->request.csrf,true,NULL)!=PORTAL_AUTH_OK)
        { auth_failure(c,PORTAL_AUTH_CSRF_INVALID,0); return; }
        login(c,ROUTE("/api/auth/setup")); return;
    }
    portal_auth_session_t session;
    portal_auth_error_t auth_error=portal_auth_session(c->request.session_token,c->request.csrf,!get,&session);
    if (auth_error!=PORTAL_AUTH_OK) { auth_failure(c,auth_error,0); return; }
    if (get && ROUTE("/api/session")) { auth_response(c,&session,false); memset(&session,0,sizeof(session)); return; }
    memset(&session,0,sizeof(session));
    if (post && ROUTE("/api/logout"))
    {
        portal_auth_logout(c->request.session_token);
        reply(c,200,"{\"authenticated\":false}","Set-Cookie: epd_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n"); return;
    }
    c->auth_checked=true;
    if (get && ROUTE("/api/ai/config")) { service_config_read(c,false); return; }
    if (post && ROUTE("/api/ai/config")) { service_config_submit(c,false); return; }
    if (get && ROUTE("/api/ai/config/job")) { service_config_poll(c,false); return; }
    if (get && ROUTE("/api/speech/config")) { service_config_read(c,true); return; }
    if (post && ROUTE("/api/speech/config")) { service_config_submit(c,true); return; }
    if (get && ROUTE("/api/speech/config/job")) { service_config_poll(c,true); return; }
    if (get && ROUTE("/api/fonts/status")) { font_snapshot(c,false); return; }
    if (get && ROUTE("/api/fonts/catalog")) { font_snapshot(c,true); return; }
    if (put && ROUTE("/api/font")) { font_upload(c); return; }
    if (post && ROUTE("/api/fonts/select")) { font_action(c,false); return; }
    if (post && ROUTE("/api/fonts/delete")) { font_action(c,true); return; }
    /* Do not queue filesystem work ahead of job polling on the single worker. */
    if (app_installer_busy() && (ROUTE("/api/info") || ROUTE("/api/files") || ROUTE("/api/file") || ROUTE("/api/folders")))
    { failure(c,409); return; }
    if (get && ROUTE("/api/info")) { info(c); return; }
    if (get && ROUTE("/api/files")) { list(c); return; }
    if (get && ROUTE("/api/file")) { download(c); return; }
    if (put && ROUTE("/api/file")) { upload(c); return; }
    if (remove && ROUTE("/api/file")) { remove_file(c); return; }
    if (post && ROUTE("/api/folders")) { folder(c); return; }
    if (get && ROUTE("/api/package")) { package_status(c); return; }
    if (put && ROUTE("/api/package")) { package_upload(c); return; }
    if (get && ROUTE("/api/settings"))
    {
        char *text=portal_settings_read(); if (!text) failure(c,503); else { reply(c,200,text,NULL); epd_app_free(text); } return;
    }
    if (post && ROUTE("/api/settings"))
    {
        cJSON *body=body_json(c); if (!body) return; cJSON_Delete(body);
        if (cancelled(c)) return;
        uint32_t ticket; rt_err_t error=portal_settings_submit(c->response,&ticket);
        if (error!=RT_EOK) { failure(c,409); return; }
        snprintf(c->response,sizeof(c->response),"{\"ticket\":%lu}",(unsigned long)ticket); reply(c,202,c->response,NULL); return;
    }
    if (get && ROUTE("/api/settings/job"))
    {
        unsigned ticket; if (!unsigned_query(c,"ticket",0,UINT32_MAX,&ticket)) { failure(c,400); return; }
        char *text=portal_settings_result(ticket);
        if (!text) failure(c,404);
        else { if (reply(c,200,text,NULL)) portal_settings_delivered(ticket); epd_app_free(text); }
        return;
    }
    failure(c,404);
#undef ROUTE
}
int portal_http_run(const portal_http_config_t *config)
{
    if (!config || !config->address || !config->service || !config->netmask) return -RT_EINVAL;
    uint32_t inverse=~ntohl(config->netmask);
    if ((inverse & (inverse+1))!=0) return -RT_EINVAL;
    connection_t *c=epd_app_alloc(sizeof(*c),EPD_APP_PSRAM); if (!c) return -RT_ENOMEM;
    memset(c,0,sizeof(*c)); c->config=*config;
    int listener=socket(AF_INET,SOCK_STREAM,0), result=-RT_EIO;
    if (listener<0) goto done;
    struct sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_port=htons(80);
    if (!inet_aton(config->address,&address.sin_addr) || address.sin_addr.s_addr==INADDR_ANY || address.sin_addr.s_addr==INADDR_BROADCAST) goto close_listener;
    int reuse=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    /* A stale select notification must not block accept during sleep drain. */
    int nonblocking=1;
    if (lwip_ioctl(listener,FIONBIO,&nonblocking)<0) goto close_listener;
    if (bind(listener,(struct sockaddr *)&address,sizeof(address))<0 || listen(listener,4)<0) goto close_listener;
    if (config->ready) config->ready();
    while (!cancelled(c) && (!config->accepting || config->accepting()))
    {
        fd_set readfds; FD_ZERO(&readfds); FD_SET(listener,&readfds); struct timeval timeout={0,200000};
        int ready=select(listener+1,&readfds,NULL,NULL,&timeout);
        if (cancelled(c)) break;
        if (ready<0) break; if (!ready) continue;
        /* Admission and preparation share an interrupt-protected boundary.
         * A winning acquire cancels prepare; committed sleep rejects it. */
        rt_base_t level=rt_hw_interrupt_disable();
        bool accepted=(!config->accepting || config->accepting()) &&
            epd_service_sleep_acquire(config->service);
        rt_hw_interrupt_enable(level);
        if (!accepted) { result=RT_EOK; break; }
        struct sockaddr_in peer; socklen_t length=sizeof(peer);
        c->socket=accept(listener,(struct sockaddr *)&peer,&length);
        if (c->socket<0)
        {
            if (!config->keep_awake) epd_service_sleep_release(config->service);
            continue;
        }
        c->auth_checked=false; c->peer=peer.sin_addr.s_addr;
        /* The selected interface supplies both the address and its netmask. */
        if ((peer.sin_addr.s_addr&config->netmask)==(address.sin_addr.s_addr&config->netmask) && !cancelled(c))
        {
            struct timeval io_timeout={0,200000};
            if (setsockopt(c->socket,SOL_SOCKET,SO_RCVTIMEO,&io_timeout,sizeof(io_timeout))==0 &&
                setsockopt(c->socket,SOL_SOCKET,SO_SNDTIMEO,&io_timeout,sizeof(io_timeout))==0)
                handle(c);
        }
        closesocket(c->socket); c->socket=-1; c->auth_checked=false;
        /* Also erase partial or unauthenticated requests before reusing buffers. */
        portal_service_config_wipe(c->response,sizeof(c->response));
        portal_service_config_wipe(c->io,sizeof(c->io));
        portal_service_config_wipe(c->header,sizeof(c->header));
        portal_service_config_wipe(&c->request,sizeof(c->request));
        if (!config->keep_awake) epd_service_sleep_release(config->service);
    }
    if (cancelled(c) || (config->accepting && !config->accepting())) result=RT_EOK;
close_listener:
    closesocket(listener);
done:
    memset(c,0,sizeof(*c)); epd_app_free(c); return result;
}
