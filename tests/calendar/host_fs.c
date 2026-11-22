#include "storage.h"
#include <errno.h>
#include "storage_file.h"
#include "system_time.h"
#include "host_fs.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char path[512]; unsigned char *data; size_t size; bool directory; } file_t;
static file_t files[256];
static struct { file_t *file; size_t position; } handles[64];
static uint32_t session=1,tick=1;
static long remaining=-1;
static int stat_error;
static file_t *find(const char *path)
{
    char name[512];snprintf(name,sizeof(name),"%s",path);size_t n=strlen(name);
    while(n>1&&name[n-1]=='/')name[--n]=0;
    for(size_t i=0;i<256;++i)if(*files[i].path&&!strcmp(files[i].path,name))return &files[i];
    return NULL;
}
static file_t *create(const char *path)
{
    for(size_t i=0;i<256;++i)if(!*files[i].path){snprintf(files[i].path,sizeof(files[i].path),"%s",path);size_t n=strlen(files[i].path);while(n>1&&files[i].path[n-1]=='/')files[i].path[--n]=0;return &files[i];}return NULL;
}
void host_reset(void){for(size_t i=0;i<256;++i)free(files[i].data);memset(files,0,sizeof(files));memset(handles,0,sizeof(handles));remaining=-1;stat_error=0;session=1;tick=1;}
void host_fail_after(long bytes){remaining=bytes;}
void host_stat_error(int error){stat_error=error;}
void host_new_card(void){++session;}
const unsigned char *host_contents(const char *path,size_t *size){file_t *f=find(path);if(!f)return NULL;*size=f->size;return f->data;}
int host_exists(const char *path){return find(path)!=NULL;}
static int storage_depth;
int host_storage_depth(void){return storage_depth;}
void storage_lock(void){++storage_depth;}
void storage_unlock(void){--storage_depth;}
bool storage_available(storage_volume_t volume){return volume==STORAGE_SD||volume==STORAGE_FLASH;}
bool storage_session_valid(storage_volume_t volume,uint32_t value){return volume==STORAGE_SD&&value==session;}
uint32_t storage_card_session(void){return session;}
uint32_t rt_tick_get(void){return tick++;}
void rt_thread_yield(void){}
int64_t fake_now=1793307600;
int64_t system_time_get_utc(void){return fake_now;}
int32_t system_time_local_day(int64_t t){return (int32_t)((t+28800)/86400);}
int open(const char *path,int flags,...)
{
    file_t *f=find(path);if(f&&(flags&O_EXCL))return -1;if(!f&&(flags&O_CREAT))f=create(path);if(!f||f->directory)return -1;
    for(int i=3;i<64;++i)if(!handles[i].file){if(flags&O_TRUNC){free(f->data);f->data=NULL;f->size=0;}handles[i].file=f;handles[i].position=0;return i;}return -1;
}
int close(int fd){if(fd<3||fd>=64||!handles[fd].file)return -1;handles[fd].file=NULL;return 0;}
int read(int fd,void *out,size_t size)
{
    if(fd<3||fd>=64||!handles[fd].file)return -1;file_t *f=handles[fd].file;
    if(handles[fd].position>=f->size)return 0;if(size>f->size-handles[fd].position)size=f->size-handles[fd].position;
    memcpy(out,f->data+handles[fd].position,size);handles[fd].position+=size;return (int)size;
}
int write(int fd,const void *data,size_t size)
{
    if(fd<3||fd>=64||!handles[fd].file||!remaining)return -1;
    if(remaining>0&&(size_t)remaining<size)size=(size_t)remaining;
    file_t *f=handles[fd].file;size_t end=handles[fd].position+size;
    if(end>f->size){unsigned char *next=realloc(f->data,end);if(!next)return -1;memset(next+f->size,0,end-f->size);f->data=next;f->size=end;}
    memcpy(f->data+handles[fd].position,data,size);handles[fd].position=end;if(remaining>0)remaining-=(long)size;return (int)size;
}
off_t lseek(int fd,off_t position,int origin)
{
    if(fd<3||fd>=64||!handles[fd].file)return -1;
    long at=origin==SEEK_SET?position:origin==SEEK_CUR?(long)handles[fd].position+position:(long)handles[fd].file->size+position;
    if(at<0)return -1;handles[fd].position=(size_t)at;return at;
}
int stat(const char *path,struct stat *out){if(stat_error){errno=stat_error;return -1;}file_t *f=find(path);if(!f){errno=-ENOENT;return -1;}out->st_size=(long)f->size;out->st_mode=f->directory;return 0;}
int fstat(int fd,struct stat *out){if(fd<3||fd>=64||!handles[fd].file)return -1;return stat(handles[fd].file->path,out);}
int mkdir(const char *path,int mode){(void)mode;if(find(path))return -1;file_t *f=create(path);if(!f)return -1;f->directory=true;return 0;}
int unlink(const char *path){file_t *f=find(path);if(!f)return -1;free(f->data);memset(f,0,sizeof(*f));return 0;}
int rmdir(const char *path){return unlink(path);}
int fsync(int fd){return fd>=3&&fd<64&&handles[fd].file?0:-1;}
DIR *opendir(const char *path){file_t *f=find(path);if(!f||!f->directory)return NULL;DIR *d=calloc(1,sizeof(*d));if(d)strcpy(d->path,path);return d;}
struct dirent *readdir(DIR *d)
{
    size_t length=strlen(d->path);
    while(d->index<256){file_t *f=&files[d->index++];if(!strncmp(f->path,d->path,length)&&f->path[length]=='/'&&!strchr(f->path+length+1,'/')){snprintf(d->entry.d_name,sizeof(d->entry.d_name),"%s",f->path+length+1);return &d->entry;}}return NULL;
}
int closedir(DIR *d){free(d);return 0;}
bool storage_mkdirs(const char *path){file_t *f=find(path);return f?f->directory:mkdir(path,0)==0;}
bool storage_file_recover(const char *path){(void)path;return true;}
bool storage_file_commit(const char *temporary,const char *path)
{file_t *f=find(temporary);if(!f)return false;unlink(path);strcpy(f->path,path);return true;}
bool storage_file_replace(const char *path,const void *data,size_t size)
{
    char temp[512];snprintf(temp,sizeof(temp),"%s.tmp",path);int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0);if(fd<0)return false;
    bool ok=write(fd,data,size)==(int)size;close(fd);if(ok)ok=storage_file_commit(temp,path);else unlink(temp);return ok;
}
bool storage_file_import(const char *source,const char *destination){file_t *f=find(source);return f&&storage_file_replace(destination,f->data,f->size);}

uint32_t storage_crc32(const void *data,size_t n){const unsigned char *p=data;uint32_t crc=~0u;while(n--){crc^=*p++;for(int i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u& (0u-(crc&1u)));}return ~crc;}
bool storage_record_save(const char *path,uint32_t magic,uint32_t version,const void *data,size_t n){uint32_t *p=malloc(n+16);if(!p)return false;p[0]=magic;p[1]=version;p[2]=(uint32_t)n;p[3]=storage_crc32(data,n);memcpy(p+4,data,n);bool ok=storage_file_replace(path,p,n+16);free(p);return ok;}
bool storage_record_load(const char *path,uint32_t magic,uint32_t version,void *out,size_t n){size_t size;const uint32_t *p=(const uint32_t *)host_contents(path,&size);if(!p||size!=n+16||p[0]!=magic||p[1]!=version||p[2]!=n||p[3]!=storage_crc32(p+4,n))return false;memcpy(out,p+4,n);return true;}
