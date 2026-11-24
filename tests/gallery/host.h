/* In-memory filesystem with short I/O, media changes and allocation faults. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <errno.h>
#define STORAGE_PATH_MAX 512
#define STORAGE_SD_ROOT "/sdcard"
#define STORAGE_FLASH_ROOT "/flash"
#define RT_EOK 0
#define EPD_APP_PSRAM 1
#define RT_ALIGN(x,a) (((x)+(uintptr_t)(a)-1)&~((uintptr_t)(a)-1))
#define LV_IMAGE_HEADER_MAGIC 25
#define LV_COLOR_FORMAT_RGB565 1
typedef struct { bool used; } epd_memory_retry_t;
typedef struct {struct {unsigned magic,cf,w,h,stride;} header;const uint8_t *data;size_t data_size;} lv_image_dsc_t;
typedef struct {uint8_t *data;size_t data_size;} lv_draw_buf_t;
#include "types.inc"
#include "raster_types.inc"
typedef raster_decoder_t gallery_decoder_t;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(2);}}while(0)
#ifndef GALLERY_REAL_MEMORY
#include "activity_fixture.h"
typedef int rt_base_t;
static int rt_hw_interrupt_disable(void){return 0;}
static void rt_hw_interrupt_enable(int v){(void)v;}
#include "resource_fixture.h"
#endif
static unsigned locks,live,allocations,fail_at,opens,reads,rev=1,card=1,cancel_at,remove_at;
static size_t live_bytes,peak_bytes;
static bool present=true,writable=true,cancelled=false;
static uint32_t content_token=1;static unsigned writer_count;
static unsigned io_count,remove_io,cancel_io,write_fail,close_calls,close_failures;
static bool close_permanent;
static const char *fail_directory;
static unsigned readdir_error_at;
#ifdef GALLERY_REAL_MEMORY
void *epd_app_alloc(size_t n,epd_app_memory_t where){CHECK(!locks);if(++allocations==fail_at)return NULL;void*p=ledger_alloc(n,where);if(p){++live;live_bytes+=n;if(live_bytes>peak_bytes)peak_bytes=live_bytes;}return p;}
void epd_app_free(void*p){CHECK(!locks);if(p){epd_memory_block_snapshot_t b;CHECK(epd_memory_block_get(p,&b)==EPD_MEMORY_OK);live_bytes-=b.size;--live;ledger_free(p);}}
#else
void *epd_app_alloc(size_t n,int where){(void)where;CHECK(!locks);if(++allocations==fail_at)return NULL;size_t*p=malloc(n+sizeof(size_t));if(!p)return NULL;*p=n;++live;live_bytes+=n;if(live_bytes>peak_bytes)peak_bytes=live_bytes;return p+1;}
void epd_app_free(void*p){CHECK(!locks);if(p){size_t*q=(size_t*)p-1;live_bytes-=*q;--live;free(q);}}
#endif
#ifndef HOST_SERVICE
void gallery_result_retain(gallery_result_t*r){CHECK(r->references);++r->references;}
void gallery_result_free(gallery_result_t*r){if(r&&!--r->references){epd_app_free(r->pixels);epd_app_free(r);}}
#endif
static void storage_lock(void){++locks;}
static void storage_unlock(void){CHECK(locks);--locks;}
typedef enum {STORAGE_SD,STORAGE_FLASH,STORAGE_COUNT} storage_volume_t;
static const char *storage_path_root(const char*p){return !strncmp(p,"/sdcard",7)?STORAGE_SD_ROOT:STORAGE_FLASH_ROOT;}
static uint32_t storage_volume_revision(int v){CHECK(v==STORAGE_SD);return rev;}
static uint32_t storage_card_session(void){return card;}
static uint32_t storage_path_revision(const char*p){(void)p;return content_token;}
static bool storage_path_pin(const char*p){(void)p;return true;}
static void storage_path_unpin(const char*p){(void)p;}
static unsigned storage_file_writers(const char*p);
static bool storage_path_available(const char*p){return !strncmp(p,"/flash",6)||present;}
static bool storage_session_valid(int v,uint32_t s){return v==STORAGE_FLASH || (present&&s==card);}
static bool storage_available(storage_volume_t v){return v==STORAGE_FLASH || present;}
typedef struct { uint64_t total,used; } storage_info_t;
static bool storage_info(storage_volume_t v,storage_info_t *info)
{ *info=(storage_info_t){64u*1024u*1024u,0};return storage_available(v); }
#ifndef HOST_SERVICE
static bool gallery_cancelled(uint32_t serial){return serial&&(cancelled||(cancel_at&&reads>=cancel_at));}
#else
bool gallery_cancelled(uint32_t serial);
void gallery_result_retain(gallery_result_t*);void gallery_result_free(gallery_result_t*);
#endif
static bool epd_memory_retry_wait(epd_memory_retry_t*r,size_t n,bool(*cb)(void*),void*c){(void)r;(void)n;(void)cb;(void)c;return false;}
static void epd_app_clean_draw_buffer(lv_draw_buf_t*b){(void)b;}
#define FS_MAX 4096
typedef struct {char path[512];bool dir;time_t modified;uint8_t *data;size_t size,capacity;} file_t;
static file_t files[FS_MAX];static unsigned file_count;
static file_t *find_file(const char*p){for(unsigned i=0;i<file_count;++i)if(!strcmp(files[i].path,p))return &files[i];return NULL;}
static file_t *add(const char*p,bool dir,time_t m){file_t*f=find_file(p);if(f)return f;CHECK(file_count<FS_MAX);f=&files[file_count++];snprintf(f->path,512,"%s",p);f->dir=dir;f->modified=m;if(!dir){f->data=calloc(1,8);f->size=f->capacity=8;}return f;}
typedef struct {char path[512];unsigned offset;} DIR;
struct dirent {char d_name[512];};
struct host_stat {int st_mode;time_t st_mtime;size_t st_size;};
#define stat host_stat
#define S_ISDIR(x) ((x)==1)
#define S_ISREG(x) ((x)==2)
static DIR *opendir(const char*p){CHECK(locks>=1);++opens;file_t*f=find_file(p);if(!f||!f->dir||(fail_directory&&!strcmp(p,fail_directory)))return NULL;
#ifdef GALLERY_REAL_MEMORY
DIR*d=ledger_alloc(4096,EPD_APP_SRAM);if(!d)return NULL;memset(d,0,sizeof(*d));
#else
DIR*d=calloc(1,sizeof(*d));
#endif
strcpy(d->path,p);return d;}
static struct dirent *readdir(DIR*d){CHECK(locks>=1);static struct dirent e;++reads;if(readdir_error_at==reads){errno=EIO;return NULL;}if(remove_at&&reads>=remove_at)present=false;size_t n=strlen(d->path);while(d->offset<file_count){file_t*f=&files[d->offset++];if(!strncmp(f->path,d->path,n)&&f->path[n]=='/'&&!strchr(f->path+n+1,'/')){strcpy(e.d_name,f->path+n+1);return &e;}}return NULL;}
static int host_stat(const char*p,struct host_stat*s){CHECK(locks>=1);file_t*f=find_file(p);if(!f){errno=ENOENT;return -1;}s->st_mode=f->dir?1:2;s->st_mtime=f->modified;s->st_size=f->size;return 0;}
static void closedir(DIR*d){CHECK(locks>=1);
#ifdef GALLERY_REAL_MEMORY
ledger_free(d);
#else
free(d);
#endif
}
#define O_RDONLY 0
#define O_RDWR 1
#define O_CREAT 2
#define O_TRUNC 4
typedef struct {file_t*f;size_t position;int flags;} fd_t;
static fd_t fds[16];static unsigned open_fds,peak_fds;
static unsigned storage_file_writers(const char*p)
{ unsigned count=writer_count;for(unsigned i=0;i<16;++i)if(fds[i].f && (fds[i].flags&O_RDWR) && !strcmp(fds[i].f->path,p))++count;return count; }
#ifdef GALLERY_REAL_MEMORY
static void *fd_memory[16];
#endif
static int host_open(const char*p,int flags,...){CHECK(locks>=1);if(!present||((flags&O_CREAT)&&!writable))return -1;file_t*f=find_file(p);if(!f&&(flags&O_CREAT))f=add(p,false,0);if(!f)return -1;if(flags&O_TRUNC){f->size=0;++rev;}for(int i=0;i<16;++i)if(!fds[i].f){
#ifdef GALLERY_REAL_MEMORY
fd_memory[i]=ledger_alloc(4096,EPD_APP_SRAM);if(!fd_memory[i])return -1;
#endif
fds[i]=(fd_t){f,0,flags};if(++open_fds>peak_fds)peak_fds=open_fds;return i;}return -1;}
static int host_close(int fd){CHECK(locks>=1&&fd>=0&&fd<16&&fds[fd].f);++close_calls;if(close_permanent||close_failures){if(close_failures)--close_failures;return -1;}
#ifdef GALLERY_REAL_MEMORY
ledger_free(fd_memory[fd]);fd_memory[fd]=NULL;
#endif
fds[fd].f=NULL;--open_fds;return 0;}
static off_t host_lseek(int fd,off_t n,int whence){CHECK(locks>=1&&whence==SEEK_SET&&n>=0);fds[fd].position=n;return n;}
static void io_fault(void){++io_count;if(io_count==remove_io){present=false;++card;}if(io_count==cancel_io)cancelled=true;}
static int host_read(int fd,void*p,size_t n){CHECK(locks>=1&&fds[fd].f);io_fault();fd_t*d=&fds[fd];if(d->position>=d->f->size)return 0;if(n>d->f->size-d->position)n=d->f->size-d->position;memcpy(p,d->f->data+d->position,n);d->position+=n;return (int)n;}
static int host_write(int fd,const void*p,size_t n){CHECK(locks>=1&&fds[fd].f);io_fault();if(!writable||(write_fail&&io_count>=write_fail))return -1;++rev;fd_t*d=&fds[fd];if(n>37)n=37;size_t end=d->position+n;if(end>d->f->capacity){d->f->capacity=end*2+64;d->f->data=realloc(d->f->data,d->f->capacity);CHECK(d->f->data);}memcpy(d->f->data+d->position,p,n);d->position=end;if(end>d->f->size)d->f->size=end;return (int)n;}
static int host_unlink(const char*p){CHECK(locks>=1);file_t*f=find_file(p);if(!f)return -1;++rev;f->path[0]=0;f->size=0;return 0;}
static int host_rename(const char*a,const char*b){CHECK(locks>=1);file_t*f=find_file(a);if(!f)return -1;++rev;strcpy(f->path,b);return 0;}
#define fstat host_fstat
static int host_fstat(int fd,struct host_stat*s){return host_stat(fds[fd].f->path,s);}
#define open host_open
#define close host_close
#define lseek host_lseek
#define read host_read
#define write host_write
#define unlink host_unlink
#define rename host_rename
static int mkdir(const char*p,int mode){(void)mode;CHECK(locks>=1);if(!writable||!present)return -1;add(p,true,0);++rev;return 0;}
static bool storage_mkdirs(const char*p){if(!writable||!present)return false;add(p,true,0);return true;}
#include "../storage_perf/owned_adapter.h"
static bool storage_file_recover(const char *path) { (void)path;return true; }
#ifndef RT_WAITING_FOREVER
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_PRIO 1
#endif
#define BUDGET_REGULAR 2
#define BUDGET_STORAGE_LOCKED locks
#include "../cache_budget/port.h"
static bool storage_file_signature_with(const char*p,uint32_t*h,bool(*update)(void*,const void*,size_t,uint32_t*),void*c){file_t*f=find_file(p);if(!f||f->dir||!present)return false;*h=2166136261u;return update(c,f->data,f->size,h);}
static unsigned decodes;
static bool decode_error;static void(*decode_hook)(void);
static bool raster_io_open(raster_decoder_t*d){CHECK(d->read_at&&d->fd==-1);return true;}
static void raster_fit(unsigned sw,unsigned sh,unsigned mw,unsigned mh,unsigned*w,unsigned*h,bool enlarge){uint64_t x=mw,y=(uint64_t)sh*x/sw;if(y>mh){y=mh;x=(uint64_t)sw*y/sh;}if(!enlarge&&x>sw){x=sw;y=sh;}*w=x?x:1;*h=y?y:1;}
static bool raster_decode(raster_decoder_t*d){++decodes;if(decode_error)return false;d->source_width=64;d->source_height=48;d->width=64;d->height=48;d->gray=epd_app_alloc(64*48,1);if(!d->gray)return false;if(decode_hook)decode_hook();for(unsigned i=0;i<64*48;++i)d->gray[i]=(uint8_t)(i%16*17);return true;}
