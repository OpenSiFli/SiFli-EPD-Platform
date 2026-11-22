#include "epd_ai.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#define RTM_EXPORT(name)
#define EPD_APP_PSRAM 1
typedef struct {bool initialized;epd_ai_error_t initialization_error;size_t profile_count;char default_id[32];epd_ai_profile_t profiles[8];} ai_settings_snapshot_t;
typedef struct {int operation;char app_id[32],profile_id[32];} ai_settings_job_t;
typedef struct {int operation;epd_ai_error_t error;} ai_settings_result_t;
enum{AI_SETTINGS_GRANT=1};
static ai_settings_snapshot_t snapshot;
static ai_settings_job_t submitted;
static bool wrong_thread,invalid_owner,changing;
static epd_ai_error_t submit_error,poll_error,result_error;
static int ui_owner=1;
static int rt_thread_self(void){return wrong_thread?2:1;}
static int rt_interrupt_get_nest(void){return 0;}
static bool storage_changing(void){return changing;}
static bool epd_service_identity(epd_service_t *owner,char *out,size_t capacity){(void)owner;snprintf(out,capacity,"notes");return !invalid_owner;}
static void *epd_app_alloc(size_t n,int pool){(void)pool;return calloc(1,n);}
static void epd_app_free(void *p){free(p);}
static epd_ai_error_t ai_settings_start(void){return EPD_AI_OK;}
static epd_ai_error_t ai_settings_snapshot(ai_settings_snapshot_t *out){*out=snapshot;return EPD_AI_OK;}
static epd_ai_error_t ai_settings_submit(const ai_settings_job_t *job,uint32_t *ticket){submitted=*job;if(!submit_error)*ticket=42;return submit_error;}
static epd_ai_error_t ai_settings_poll(uint32_t ticket,ai_settings_result_t *out){CHECK(ticket==42);out->operation=AI_SETTINGS_GRANT;out->error=result_error;return poll_error;}
#include "authorize_actual.inc"
int main(void)
{
    snapshot.initialized=true;snapshot.profile_count=2;strcpy(snapshot.default_id,"preferred");
    for(unsigned i=0;i<2;++i){snapshot.profiles[i].enabled=true;snapshot.profiles[i].model_count=1;snapshot.profiles[i].models[0].capabilities=EPD_AI_CAP_TEXT;}
    strcpy(snapshot.profiles[0].id,"first");strcpy(snapshot.profiles[1].id,"preferred");uint32_t ticket=0;
    CHECK(epd_ai_authorize_service(NULL,&ticket)==EPD_AI_OK&&ticket==42);CHECK(!strcmp(submitted.profile_id,"preferred")&&!strcmp(submitted.app_id,"notes"));
    poll_error=EPD_AI_BUSY;CHECK(epd_ai_authorization_poll(ticket)==EPD_AI_BUSY);poll_error=EPD_AI_OK;result_error=EPD_AI_STORAGE;CHECK(epd_ai_authorization_poll(ticket)==EPD_AI_STORAGE);result_error=EPD_AI_OK;CHECK(epd_ai_authorization_poll(ticket)==EPD_AI_OK);
    snapshot.profiles[1].models[0].capabilities=0;CHECK(epd_ai_authorize_service(NULL,&ticket)==EPD_AI_OK&&!strcmp(submitted.profile_id,"first"));
    snapshot.profiles[0].enabled=false;CHECK(epd_ai_authorize_service(NULL,&ticket)==EPD_AI_NOT_FOUND&&!ticket);snapshot.profiles[0].enabled=true;
    submit_error=EPD_AI_BUSY;CHECK(epd_ai_authorize_service(NULL,&ticket)==EPD_AI_BUSY&&!ticket);submit_error=EPD_AI_OK;
    invalid_owner=true;CHECK(epd_ai_authorize_service(NULL,&ticket)==EPD_AI_INVALID);invalid_owner=false;wrong_thread=true;CHECK(epd_ai_authorize_service(NULL,&ticket)==EPD_AI_BUSY);wrong_thread=false;
    changing=true;CHECK(epd_ai_authorize_service(NULL,&ticket)==EPD_AI_BUSY);changing=false;
    puts("platform direct authorization passed");return 0;
}
