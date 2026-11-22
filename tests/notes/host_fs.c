#include "storage.h"
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
host_io_t host_io;
static int lock_depth;
static bool watching;
void host_watch_io(void){memset(&host_io,0,sizeof(host_io));watching=true;}
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
void host_reset(void){for(size_t i=0;i<256;++i)free(files[i].data);memset(files,0,sizeof(files));memset(handles,0,sizeof(handles));remaining=-1;session=1;tick=1;watching=false;lock_depth=0;memset(&host_io,0,sizeof(host_io));}
void host_fail_after(long bytes){remaining=bytes;}
void host_new_card(void){++session;}
const unsigned char *host_contents(const char *path,size_t *size){file_t *f=find(path);if(!f)return NULL;*size=f->size;return f->data;}
int host_exists(const char *path){return find(path)!=NULL;}
void storage_lock(void){++lock_depth;}
void storage_unlock(void){--lock_depth;}
bool storage_available(storage_volume_t volume){return volume==STORAGE_SD;}
bool storage_session_valid(storage_volume_t volume,uint32_t value){return volume==STORAGE_SD&&value==session;}
uint32_t storage_card_session(void){return session;}
uint32_t rt_tick_get(void){return tick++;}
void rt_thread_yield(void){}
int64_t system_time_get_utc(void){return 1000;}
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
int stat(const char *path,struct stat *out){file_t *f=find(path);if(!f)return -1;out->st_size=(long)f->size;out->st_mode=f->directory;return 0;}
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
bool storage_file_recover(const char *path){return find(path)!=NULL;}
bool storage_file_commit(const char *temporary,const char *path)
{file_t *f=find(temporary);if(!f)return false;unlink(path);strcpy(f->path,path);return true;}
bool storage_file_replace(const char *path,const void *data,size_t size)
{
    char temp[512];snprintf(temp,sizeof(temp),"%s.tmp",path);int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0);if(fd<0)return false;
    bool ok=write(fd,data,size)==(int)size;close(fd);if(ok)ok=storage_file_commit(temp,path);else unlink(temp);return ok;
}
bool storage_file_import(const char *source,const char *destination){
    if(watching){++host_io.imports;host_io.import_lock_depth=lock_depth;
        if(host_io.swap_on_import){++session;return false;}
        if(host_io.fail_import)return false;}
    file_t *f=find(source);return f&&storage_file_replace(destination,f->data,f->size);
}
bool storage_file_import_session(const char *source,const char *destination,
                                 storage_volume_t volume,uint32_t expected_session){
    if(watching){++host_io.session_imports;host_io.import_lock_depth=lock_depth;
        host_io.expected_volume=(int)volume;host_io.expected_session=expected_session;
        /* Simulate a swap after the caller's check, before the API takes its lock. */
        if(host_io.swap_on_import)host_new_card();}
    storage_lock();
    bool valid=storage_session_valid(volume,expected_session);
    if(watching&&!valid)++host_io.session_rejections;
    bool ok=false;
    if(valid&&!(watching&&host_io.fail_import)){
        file_t *f=find(source);ok=f&&storage_file_replace(destination,f->data,f->size);
    }
    storage_unlock();return ok;
}
