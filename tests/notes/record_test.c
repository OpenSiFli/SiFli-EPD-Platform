#include "epd_record.h"
#include "speech_audio.h"
#include "speech_core.h"
#include "speech_media.h"
#include "storage_internal.h"
#include "host_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} }while(0)
struct speech_audio_input { unsigned available; bool finished; };
struct speech_provider_session { speech_provider_sink_t sink; bool finished; };
static uint32_t clock_ms;
static unsigned holds, captures, releases, acknowledgements, opens;
static bool restore_seen, net_ready=true, media_ready=true, release_card, account_revoked;
static uint32_t revoke_at;
void *speech_alloc(size_t size){return calloc(1,size);}
void speech_free(void *data){free(data);}
uint32_t speech_now_ms(void){return clock_ms;}
void rt_thread_mdelay(int milliseconds){clock_ms+=(unsigned)milliseconds;}
bool sleep_guard_acquire(sleep_guard_owner_t *o){if(!o->held){o->held=true;++holds;}return true;}
void sleep_guard_release(sleep_guard_owner_t *o){if(o->held){o->held=false;--holds;}}
bool storage_info(storage_volume_t v,storage_info_t *out){(void)v;*out=(storage_info_t){200000000,0};return true;}
bool storage_release_requested(void){return release_card;}
bool network_ready(void){return net_ready;}
epd_speech_error_t epd_speech_capability(epd_speech_kind_t k,epd_speech_capability_t *out){(void)k;*out=(epd_speech_capability_t){.input_available=true};return EPD_SPEECH_OK;}
bool speech_tts_suspend(speech_session_t *s,uint32_t *id,uint32_t *intent){(void)s;*id=*intent=0;return true;}
epd_speech_error_t speech_media_acquire(epd_speech_kind_t k,speech_cancel_fn cb,void *user){(void)k;return cb(user)?EPD_SPEECH_CANCELLED:media_ready?EPD_SPEECH_OK:EPD_SPEECH_BUSY;}
bool speech_media_can_start(epd_speech_kind_t k){(void)k;return media_ready;}
void speech_media_release(epd_speech_kind_t k,bool restore){(void)k;++releases;restore_seen|=restore;}
epd_speech_error_t speech_audio_input_open_limited(speech_cancel_fn cb,void *user,uint32_t limit,speech_audio_input_t **out){
    CHECK(limit&&limit<=3600000);CHECK(!cb(user));*out=calloc(1,sizeof(**out));(*out)->available=6400;++captures;return EPD_SPEECH_OK;
}
int speech_audio_input_read(speech_audio_input_t *in,void *pcm,size_t capacity){size_t n=in->available;if(n>capacity)n=capacity;memset(pcm,0x42,n);in->available-=(unsigned)n;return (int)n;}
void speech_audio_input_ack(speech_audio_input_t *in,size_t bytes){(void)in;acknowledgements+=(unsigned)bytes;}
void speech_audio_input_status(speech_audio_input_t *in,speech_audio_input_status_t *out){memset(out,0,sizeof(*out));out->finished=in->finished;}
void speech_audio_input_finish(speech_audio_input_t *in){in->finished=true;in->available+=512;}
void speech_audio_input_close(speech_audio_input_t *in){free(in);--captures;}
epd_speech_error_t speech_config_account(epd_speech_kind_t k,speech_account_t *out){(void)k;memset(out,0,sizeof(*out));out->enabled=!account_revoked;out->provider=SPEECH_ALI;return account_revoked?EPD_SPEECH_NOT_CONFIGURED:EPD_SPEECH_OK;}
epd_speech_error_t speech_provider_open(const speech_account_t *a,epd_speech_kind_t k,epd_speech_language_t l,speech_cancel_fn cb,void *u,const speech_provider_sink_t *sink,speech_provider_session_t **out,speech_failure_t *f){
    (void)a;(void)k;(void)l;(void)f;if(cb(u))return EPD_SPEECH_CANCELLED;*out=calloc(1,sizeof(**out));(*out)->sink=*sink;++opens;return EPD_SPEECH_OK;
}
epd_speech_error_t speech_provider_audio(speech_provider_session_t *s,const void *p,size_t n){(void)s;(void)p;CHECK(n<=6400);return EPD_SPEECH_OK;}
epd_speech_error_t speech_provider_finish(speech_provider_session_t *s){s->finished=true;return EPD_SPEECH_OK;}
epd_speech_error_t speech_provider_poll(speech_provider_session_t *s,uint32_t ms,bool *done){clock_ms+=ms;if(revoke_at&&clock_ms>=revoke_at)account_revoked=true;*done=s->finished;if(*done&&!s->sink.text(s->sink.user,1,true,"完成转写",12))return EPD_SPEECH_PROTOCOL;return EPD_SPEECH_OK;}
void speech_provider_close(speech_provider_session_t *s){free(s);--opens;}
static bool cancel(void *unused){(void)unused;return false;}
static bool cancel_now(void *unused){(void)unused;return true;}
static uint32_t get32(const unsigned char *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main(void){
    host_reset();epd_record_t *record=NULL;const char *path="/sdcard/take.wav";
    CHECK(!epd_record_open(path,1,cancel,NULL,&record));CHECK(captures==1&&holds==1);
    CHECK(!epd_record_pump(record));CHECK(!epd_record_pause(record));CHECK(!captures&&!holds);
    size_t size;const unsigned char *wav=host_contents(path,&size);CHECK(size==44+6912&&get32(wav+40)==6912&&acknowledgements==6912);
    CHECK(!epd_record_resume(record));CHECK(!epd_record_pump(record));CHECK(!epd_record_close(record));CHECK(!holds&&!captures&&!restore_seen&&releases==2);
    wav=host_contents(path,&size);CHECK(size==44+13824&&get32(wav+40)==13824);
    CHECK(epd_record_open(path,1,cancel,NULL,&record)==EPD_SPEECH_STORAGE);
    wav=host_contents(path,&size);CHECK(wav&&size==44+13824);
    int fd=open(path,O_RDWR,0);unsigned char zero[4]={0};CHECK(lseek(fd,40,SEEK_SET)==40&&write(fd,zero,4)==4);close(fd);
    uint32_t duration;CHECK(!epd_record_recover(path,1,&duration)&&duration==432);
    char text[128];CHECK(!epd_record_transcribe(path,1,0,432,cancel,NULL,text,sizeof(text)));CHECK(!strcmp(text,"完成转写")&&!holds&&!opens);
    net_ready=false;CHECK(epd_record_transcribe(path,1,0,432,cancel,NULL,text,sizeof(text))==EPD_SPEECH_NO_NETWORK);net_ready=true;
    CHECK(epd_record_transcribe(path,1,0,432,cancel,NULL,text,4)==EPD_SPEECH_TEXT_TOO_LONG);CHECK(!holds&&!opens);
    CHECK(epd_record_transcribe(path,1,0,432,cancel_now,NULL,text,sizeof(text))==EPD_SPEECH_CANCELLED);CHECK(!holds&&!opens);
    revoke_at=clock_ms+100;CHECK(epd_record_transcribe(path,1,0,432,cancel,NULL,text,sizeof(text))==EPD_SPEECH_CANCELLED);CHECK(!holds&&!opens);
    revoke_at=0;account_revoked=false;
    CHECK(!epd_record_open("/sdcard/resume.wav",1,cancel,NULL,&record));CHECK(!epd_record_pause(record));media_ready=false;
    CHECK(epd_record_resume(record)==EPD_SPEECH_BUSY);CHECK(!holds&&!captures);media_ready=true;
    CHECK(!epd_record_resume(record));CHECK(!epd_record_close(record));CHECK(!holds&&!captures&&!restore_seen);
    media_ready=false;CHECK(epd_record_open("/sdcard/fail.wav",1,cancel,NULL,&record)==EPD_SPEECH_BUSY);CHECK(!host_exists("/sdcard/fail.wav")&&!holds);media_ready=true;
    CHECK(!epd_record_open("/sdcard/full.wav",1,cancel,NULL,&record));host_fail_after(10);
    CHECK(epd_record_pump(record)==EPD_SPEECH_STORAGE);host_fail_after(-1);CHECK(epd_record_close(record)==EPD_SPEECH_STORAGE);CHECK(!holds&&!captures);
    CHECK(!epd_record_open("/sdcard/card.wav",1,cancel,NULL,&record));CHECK(!epd_record_pause(record));host_new_card();
    CHECK(epd_record_resume(record)==EPD_SPEECH_STORAGE);CHECK(epd_record_close(record)==EPD_SPEECH_STORAGE);CHECK(!holds&&!captures&&!restore_seen);
    host_reset();puts("notes recording: tail drain, pause/resume, WAV repair, disk/card errors, ASR and cleanup passed");return 0;
}
