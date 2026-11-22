#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include "file_page_index.h"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
#define STORAGE_PATH_MAX 512
#define STORAGE_SD_ROOT "/sdcard"
#define STORAGE_FLASH_ROOT "/flash"
#define FILE_ROWS 8
#define DFS_PATH_MAX 256
#define RTM_EXPORT(x)
#define RT_ASSERT(x) CHECK(x)
#define RT_NULL NULL
#define rt_strlen strlen
#define rt_strncpy strncpy
#define rt_strcmp strcmp
#define DFS_FS_FLAG_FULLPATH 1
#define RT_FIOFTRUNCATE 0x52540000u
#define EPD_APP_PSRAM 1
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif
typedef long rt_off_t;
typedef uint32_t rt_uint32_t;
typedef uint16_t rt_uint16_t;
typedef uint8_t rt_uint8_t;
typedef int rt_base_t;
typedef enum {STORAGE_FLASH,STORAGE_SD,STORAGE_COUNT} storage_volume_t;
enum {FT_REGULAR,FT_DIRECTORY,DT_UNKNOWN=0,DT_DIR=1,DT_REG=2};
struct dirent {uint8_t d_type,d_namlen;uint16_t d_reclen;char d_name[DFS_PATH_MAX];};
struct stat {unsigned st_mode;size_t st_size;};
#define S_ISDIR(mode) ((mode)==DT_DIR)
struct dfs_fd;
struct dfs_filesystem;
struct dfs_file_ops {
    int (*open)(struct dfs_fd *);
    int (*close)(struct dfs_fd *);
    int (*ioctl)(struct dfs_fd *,int,void *);
    int (*write)(struct dfs_fd *,const void *,size_t);
};
struct dfs_filesystem_ops {
    const char *name;uint32_t flags;const struct dfs_file_ops *fops;
    int (*unlink)(struct dfs_filesystem *,const char *);
    int (*rename)(struct dfs_filesystem *,const char *,const char *);
};
struct dfs_filesystem {const char *path;const struct dfs_filesystem_ops *ops;};
struct dfs_fd {
    struct dfs_filesystem *fs;const struct dfs_file_ops *fops;const char *path;unsigned flags;
    int type;long pos;void *data;
};
static struct dfs_filesystem volumes[2];
static struct dfs_fd descriptors[8];
static bool handles[8];
static unsigned opens,closes,open_handles,live_caches;
static bool releasing,changing,fail_alloc;
static int last_errno;
static unsigned card_session=7;
static int lock_depth;
static unsigned read_calls,stat_calls,seek_calls,seek_steps,read_mutation,fail_read_at;
static bool fail_seek,use_lfs;
static char directory[STORAGE_PATH_MAX];
typedef struct {char name[256];bool folder,unreadable;uint64_t size;} entry_t;
static entry_t entries[8192];
static unsigned entry_count;
static void *epd_app_alloc(size_t size,int region){CHECK(region==EPD_APP_PSRAM);if(fail_alloc)return NULL;void *p=malloc(size);if(p&&size==sizeof(file_page_cache_t))++live_caches;return p;}
static void epd_app_free(void *p){if(p){CHECK(live_caches);--live_caches;free(p);}}
static void rt_free(void *p){free(p);}
static char *dfs_normalize_path(const char *base,const char *path){(void)base;char *p=malloc(strlen(path)+1);CHECK(p);strcpy(p,path);return p;}
static rt_base_t rt_hw_interrupt_disable(void){return 0;}
static void rt_hw_interrupt_enable(rt_base_t level){(void)level;}
static const char *storage_root(storage_volume_t v){return v==STORAGE_SD?STORAGE_SD_ROOT:STORAGE_FLASH_ROOT;}
static const char *storage_path_root(const char *p){return !strncmp(p,STORAGE_SD_ROOT,7)?STORAGE_SD_ROOT:STORAGE_FLASH_ROOT;}
static struct dfs_filesystem *dfs_filesystem_lookup(const char *p){return &volumes[!strcmp(storage_path_root(p),STORAGE_SD_ROOT)];}
static int raw_open(struct dfs_fd *fd){(void)fd;return 0;}
static int raw_close(struct dfs_fd *fd){(void)fd;return 0;}
static int raw_write(struct dfs_fd *fd,const void *data,size_t size){(void)fd;(void)data;return (int)size;}
static int raw_ioctl(struct dfs_fd *fd,int cmd,void *arg){(void)fd;(void)cmd;(void)arg;return 0;}
static int raw_unlink(struct dfs_filesystem *fs,const char *path){(void)fs;(void)path;return 0;}
static int raw_rename(struct dfs_filesystem *fs,const char *a,const char *b){(void)fs;(void)a;(void)b;return 0;}
#include "watch_actual.inc"
static const struct dfs_file_ops original_files={raw_open,raw_close,raw_ioctl,raw_write};
static const struct dfs_filesystem_ops original_ops={"host",0,&original_files,raw_unlink,raw_rename};
static void storage_lock(void){++lock_depth;}
static void storage_unlock(void){
    CHECK(lock_depth>0);--lock_depth;
    if(!lock_depth&&read_mutation&&read_calls>=read_mutation){
        read_mutation=0;volumes[use_lfs?0:1].ops->unlink(&volumes[use_lfs?0:1],"/changed");
    }
}
static bool storage_changing(void){return changing;}
static bool storage_release_requested(void){return releasing;}
static bool storage_available(storage_volume_t volume){(void)volume;return true;}
static uint32_t storage_card_session(void){return card_session;}
static bool storage_session_valid(storage_volume_t volume,uint32_t session){return volume==STORAGE_FLASH||session==card_session;}
static int stat(const char *path,struct stat *out){
    CHECK(lock_depth);++stat_calls;size_t prefix=strlen(directory);
    if(strncmp(path,directory,prefix)||path[prefix]!='/')return -1;
    for(unsigned i=0;i<entry_count;++i)if(!strcmp(entries[i].name,path+prefix+1)){
        if(entries[i].unreadable)return -1;out->st_mode=entries[i].folder?DT_DIR:DT_REG;out->st_size=(size_t)entries[i].size;return 0;
    }return -1;
}
/* The raw engines model FatFS visible-entry ordinals and LittleFS ordinals
 * including ./..; all DFS offset conversions below are production functions. */
typedef int FRESULT;
enum {FR_OK=0,FR_INT_ERR=1,AM_DIR=16};
typedef struct {unsigned cursor;} fat_dir_t;
typedef struct {long fptr;} FIL;
typedef struct {char fname[256],altname[16];unsigned fattrib;} FILINFO;
static int f_lseek(FIL *fd,long offset){fd->fptr=offset;return FR_OK;}
static int elm_result_to_dfs(int result){return result?-EIO:0;}
static int dir_sdi(fat_dir_t *dir,unsigned offset){dir->cursor=offset;return FR_OK;}
static int dir_read(fat_dir_t *dir,int flags){(void)flags;++seek_steps;return dir->cursor<entry_count?FR_OK:FR_INT_ERR;}
static int dir_next(fat_dir_t *dir,int flags){(void)flags;++dir->cursor;return FR_OK;}
static int f_readdir(fat_dir_t *dir,FILINFO *info){
    memset(info,0,sizeof(*info));if(dir->cursor==entry_count)return FR_OK;
    CHECK(dir->cursor<entry_count);entry_t *e=&entries[dir->cursor++];strcpy(info->fname,e->name);info->fattrib=e->folder?AM_DIR:0;return FR_OK;
}
#define DIR fat_dir_t
#include "fat_seek_actual.inc"
#include "fat_actual.inc"
#undef DIR
typedef long lfs_soff_t;
typedef struct {unsigned pos;} lfs_file_t;
typedef struct {unsigned pos;} lfs_dir_t;
typedef int lfs_t;
typedef struct {lfs_t *lfs;union {lfs_file_t file;lfs_dir_t dir;} u;} dfs_lfs_fd_t;
struct lfs_info {char name[256];unsigned type;};
enum {LFS_SEEK_SET=0,LFS_TYPE_DIR=1,LFS_TYPE_REG=2};
static int _lfs_result_to_dfs(int result){return result<0?result:0;}
static long lfs_file_seek(lfs_t *lfs,lfs_file_t *file,long offset,int origin){(void)lfs;(void)origin;file->pos=(unsigned)offset;return offset;}
static long lfs_dir_seek(lfs_t *lfs,lfs_dir_t *dir,long offset){
    (void)lfs;dir->pos=0;if(offset<0)return -EINVAL;
    /* The real rawseek rewinds, advances ordinals, and rejects an exhausted tail. */
    if((unsigned long)offset>=entry_count+2){dir->pos=entry_count+2;return -EINVAL;}
    dir->pos=(unsigned)offset;return 0;
}
static int lfs_dir_read(lfs_t *lfs,lfs_dir_t *dir,struct lfs_info *info){
    (void)lfs;memset(info,0,sizeof(*info));if(dir->pos>=entry_count+2)return 0;
    if(dir->pos<2){strcpy(info->name,dir->pos++?"..":".");info->type=LFS_TYPE_DIR;return 1;}
    entry_t *e=&entries[dir->pos++-2];strcpy(info->name,e->name);info->type=e->folder?LFS_TYPE_DIR:LFS_TYPE_REG;return 1;
}
#include "lfs_actual.inc"
typedef struct {int fd;char buf[512];int num,cur;} DIR;
static fat_dir_t fat_directories[8];
static dfs_lfs_fd_t lfs_directories[8];
static struct dfs_fd *fd_get(int fd){return fd>=3&&fd<11&&handles[fd-3]?&descriptors[fd-3]:NULL;}
static void fd_put(struct dfs_fd *fd){(void)fd;}
static void rt_set_errno(int error){last_errno=error;}
static int rt_get_errno(void){return last_errno;}
static int dfs_file_getdents(struct dfs_fd *fd,struct dirent *out,uint32_t size){
    ++read_calls;
    if(fail_read_at&&read_calls==fail_read_at){fail_read_at=0;return -EIO;}
    return use_lfs?_dfs_lfs_getdents(fd,out,size):dfs_elm_getdents(fd,out,size);
}
static int dfs_file_lseek(struct dfs_fd *fd,long offset){
    ++seek_calls;if(fail_seek)return -EIO;
    int result=use_lfs?_dfs_lfs_lseek(fd,offset):dfs_elm_lseek(fd,offset);
    if(result>=0)fd->pos=result;return result;
}
#include "posix_actual.inc"
static DIR *opendir(const char *path){
    CHECK(lock_depth);CHECK(!releasing&&!changing);if(strcmp(path,directory))return NULL;
    unsigned slot=0;while(slot<8&&handles[slot])++slot;CHECK(slot<8);
    DIR *dir=calloc(1,sizeof(*dir));CHECK(dir);dir->fd=3+(int)slot;
    memset(&fat_directories[slot],0,sizeof(fat_directories[slot]));memset(&lfs_directories[slot],0,sizeof(lfs_directories[slot]));
    struct dfs_filesystem *fs=&volumes[use_lfs?0:1];
    descriptors[slot]=(struct dfs_fd){fs,fs->ops->fops,path+strlen(fs->path),O_RDONLY,FT_DIRECTORY,0,use_lfs?(void *)&lfs_directories[slot]:(void *)&fat_directories[slot]};
    CHECK(!descriptors[slot].fops->open(&descriptors[slot]));handles[slot]=true;++opens;++open_handles;return dir;
}
static int closedir(DIR *dir){CHECK(lock_depth);unsigned slot=(unsigned)dir->fd-3;CHECK(slot<8&&handles[slot]);int result=descriptors[slot].fops->close(&descriptors[slot]);handles[slot]=false;--open_handles;++closes;free(dir);return result;}
typedef int rt_err_t;
typedef void lv_obj_t;
typedef void lv_timer_t;
typedef void ui_settings_list_t;
typedef struct {int dummy;} app_content_handler_t;
typedef void (*files_picker_callback_t)(void);
#include "file_view_actual.inc"
static struct {void *feature;int id;bool foreground;} context,*current=&context;
static void text_update(void *object,const char *text){(void)object;(void)text;CHECK(!lock_depth);}
static const char *epd_get_str(const char *s){return s;}
#define platform_storage_unavailable "unavailable"
#define platform_operation_failed "failed; retry"
#define platform_folder "folder"
#define platform_unreadable "unreadable"
static void storage_format_size(uint64_t size,char *out,size_t capacity){snprintf(out,capacity,"%llu",(unsigned long long)size);}
static void files_retranslate(void);
static void files_render(void);
static void picker_detach(files_view_t *view){view->pick_owner=NULL;}
static void lv_timer_delete(void *timer){(void)timer;}
static void ui_settings_list_destroy(void *list){(void)list;}
static void lv_free(void *view){free(view);}
#include "file_page_actual.inc"
static void files_retranslate(void){files_render_directory(current->feature);}
static void files_render(void){files_render_directory(current->feature);}
static void page_render(files_view_t *view){
    bool tracked=false;for(files_view_t *v=file_views;v;v=v->next_view)if(v==view)tracked=true;
    if(!tracked)files_track(view);
    current->feature=view;files_render_directory(view);
}
enum {UI_PAGE_FILES,UI_PAGE_STORAGE,UI_PAGE_FILE_PICKER,UI_PAGE_HOME};
typedef struct {void (*create)(lv_obj_t *);void (*resume)(void);void (*pause)(void);void (*stop)(void);bool (*action)(int *);} ui_page_ops_t;
static void files_create(lv_obj_t *screen){(void)screen;}
static void storage_page_create(lv_obj_t *screen){(void)screen;}
static bool files_action(int *target){(void)target;return false;}
#include "file_ops_actual.inc"
static unsigned release_request,release_notifications;
static bool font_release_done;
static int pending_page;
#define NAV_IDLE 0
static void app_card_process(void){}
static uint32_t storage_release_request_id(void){return release_request;}
static bool lv_refreshing_done(void){return true;}
static void reading_fonts_release_storage(void){CHECK(!open_handles);++release_notifications;}
static bool reading_font_release_storage(void){CHECK(!open_handles);return font_release_done;}
static void ui_shell_storage_changed(void){CHECK(!open_handles);}
static void home_pause(void){}
static void app_catalog_changed(void){}
#include "file_release_actual.inc"
static files_view_t fixture(bool little,unsigned count){
    CHECK(!file_views&&!open_handles&&!live_caches);
    use_lfs=little;fail_seek=false;entry_count=count;read_calls=stat_calls=seek_calls=seek_steps=read_mutation=fail_read_at=0;
    opens=closes=0;releasing=changing=fail_alloc=false;
    for(unsigned v=0;v<2;++v){volumes[v]=(struct dfs_filesystem){storage_root((storage_volume_t)v),&original_ops};storage_watch_mount((storage_volume_t)v);}
    strcpy(directory,little?STORAGE_FLASH_ROOT:STORAGE_SD_ROOT);
    for(unsigned i=0;i<count;++i){entries[i]=(entry_t){0};snprintf(entries[i].name,sizeof(entries[i].name),"entry-%05u.txt",i);entries[i].size=i;}
    files_view_t view={0};strcpy(view.path,directory);view.page_rows=8;return view;
}
static bool expected_accepts(const files_view_t *view,const char *name){
    const char *suffix=strrchr(name,'.');if(!suffix)return false;
    char lower[32];size_t length=strlen(suffix);if(length>=sizeof(lower))return false;
    for(size_t i=0;i<=length;++i)lower[i]=(char)tolower((unsigned char)suffix[i]);
    if(!strcmp(view->pick_extensions,".txt"))return !strcmp(lower,".txt");
    if(view->pick_extensions[0])return !strcmp(lower,".png")||!strcmp(lower,".md")||!strcmp(lower,".mp3");
    if(view->pick_json)return !strcmp(lower,".json");
    return !strcmp(lower,".png")||!strcmp(lower,".jpg")||!strcmp(lower,".jpeg");
}
static unsigned expected_rows(const files_view_t *view,unsigned *rows){
    unsigned count=0;
    for(unsigned i=0;i<entry_count;++i){entry_t *e=&entries[i];
        if(!strcmp(e->name,".")||!strcmp(e->name,"..")||(!strcmp(view->path,storage_root(use_lfs?STORAGE_FLASH:STORAGE_SD))&&!strcmp(e->name,".portal-sap")))continue;
        if(view->picker&&(e->unreadable||(!e->folder&&!expected_accepts(view,e->name))))continue;
        rows[count++]=i;
    }return count;
}
static void check_page(files_view_t *view){
    unsigned rows[8192],total=expected_rows(view,rows),first=view->first;
    unsigned expected=total>first?total-first:0;if(expected>view->page_rows)expected=view->page_rows;
    if(view->count!=expected){fprintf(stderr,"first=%u expected_count=%u actual_count=%u seek_calls=%u\n",first,expected,view->count,seek_calls);exit(1);}
    CHECK(view->more==(total>first+expected));CHECK(!lock_depth);
    for(unsigned i=0;i<expected;++i){const char *wanted=entries[rows[first+i]].name;
        if(strcmp(view->names[i],wanted)){fprintf(stderr,"row=%u expected=%s actual=%s seek_calls=%u\n",first+i,wanted,view->names[i],seek_calls);exit(1);}
        CHECK(view->directories[i]==entries[rows[first+i]].folder);CHECK(view->readable[i]==!entries[rows[first+i]].unreadable);
    }
}
static void scan(files_view_t *view){page_render(view);check_page(view);}
static void roundtrip(bool little,unsigned count){
    files_view_t view=fixture(little,count);scan(&view);
    unsigned pages=1;
    while(view.more){view.first+=view.count;scan(&view);++pages;}
    unsigned reads=read_calls,steps=seek_steps;CHECK(opens==1&&!open_handles);
    while(view.first){view.first-=view.first<view.page_rows?view.first:view.page_rows;scan(&view);}
    CHECK(!seek_calls&&!steps);CHECK(reads==count+1);
    CHECK(sizeof(file_page_cache_t)<36u*1024u);
    printf("roundtrip entries=%u pages=%u forward_getdents=%u seek_calls=%u cache_bytes=%zu\n",count,pages,reads,seek_calls,sizeof(file_page_cache_t));
    files_dispose(&view);
}
static void picker_case(unsigned mode){
    files_view_t view=fixture(false,240);view.picker=true;view.pick_json=mode==1;
    if(mode==2)strcpy(view.pick_extensions,".PNG;.md;.mp3");
    const char *ext[]={".png",".JPEG",".jPeG",".json",".JSON",".md",".MP3",".txt",".png.exe",""};
    for(unsigned i=0;i<entry_count;++i){snprintf(entries[i].name,sizeof(entries[i].name),"文件-%u%s",i,ext[i%10]);entries[i].folder=i%17==0;entries[i].unreadable=false;}
    strcpy(entries[0].name,".portal-sap");scan(&view);
    while(view.more){view.first+=view.count;scan(&view);}
    while(view.first){view.first-=view.first<view.page_rows?view.first:view.page_rows;scan(&view);}
    CHECK(!picker_accepts(&view,"wrong.png.exe"));
    if(mode==0){CHECK(picker_accepts(&view,"a.JPEG"));CHECK(!picker_accepts(&view,"a.json"));}
    if(mode==1){CHECK(picker_accepts(&view,"a.JsOn"));CHECK(!picker_accepts(&view,"a.png"));}
    if(mode==2){CHECK(picker_accepts(&view,"a.MP3"));CHECK(!picker_accepts(&view,"a.txt"));}
    files_dispose(&view);
}
static void mutation_case(const char *kind){
    files_view_t view=fixture(false,40);scan(&view);view.first=16;scan(&view);unsigned seeks=seek_calls;
    if(!strcmp(kind,"delete")){memmove(entries,entries+10,(entry_count-10)*sizeof(*entries));entry_count-=10;volumes[1].ops->unlink(&volumes[1],"/entry-00000.txt");}
    else if(!strcmp(kind,"rename")){strcpy(entries[0].name,"renamed.txt");volumes[1].ops->rename(&volumes[1],"/entry-00000.txt","/renamed.txt");}
    else if(!strcmp(kind,"remount")){++card_session;volumes[1].ops=&original_ops;storage_watch_mount(STORAGE_SD);strcpy(entries[0].name,"new-card.txt");}
    else if(!strcmp(kind,"subtree")){strcpy(view.path,"/sdcard/books");strcpy(directory,view.path);scan(&view);view.first=16;scan(&view);seeks=seek_calls;
        strcpy(directory,"/sdcard/old-books");volumes[1].ops->rename(&volumes[1],"/books","/old-books");page_render(&view);CHECK(view.page_failed&&!view.count&&!view.more&&!view.first);files_dispose(&view);return;}
    scan(&view);CHECK(!view.first);CHECK(seek_calls==seeks);files_dispose(&view);
}
static void during_read(void){
    files_view_t view=fixture(false,40);read_mutation=3;page_render(&view);
    CHECK(view.page_failed&&!view.count&&!view.more&&!view.cursor);read_mutation=0;scan(&view);files_dispose(&view);
}
static void layout_change(void){
    files_view_t view=fixture(false,90);scan(&view);view.first=24;scan(&view);
    view.page_rows=3;scan(&view);view.first+=view.count;scan(&view);view.first-=3;scan(&view);
    view.page_rows=8;scan(&view);files_dispose(&view);
}
static void seek_failure(void){
    files_view_t view=fixture(false,40);scan(&view);view.first=8;fail_seek=true;scan(&view);CHECK(!seek_calls);files_dispose(&view);
}
static void stat_recovery(void){
    files_view_t view=fixture(false,40);view.picker=true;strcpy(view.pick_extensions,".txt");
    entries[0].unreadable=true;page_render(&view);CHECK(view.page_failed&&!view.count&&!view.cursor);CHECK(!file_page_find(view.cache,0));
    /* A transient stat error need not change directory contents or their revision. */
    entries[0].unreadable=false;scan(&view);view.first=8;scan(&view);files_dispose(&view);
}
static void flash_remount(void){
    files_view_t view=fixture(true,40);scan(&view);view.first=16;
    volumes[0].ops=&original_ops;storage_watch_mount(STORAGE_FLASH);scan(&view);CHECK(!view.first&&!seek_calls);files_dispose(&view);
}
static void metadata_refresh(bool little){
    files_view_t view=fixture(little,40);scan(&view);view.first=8;scan(&view);
    unsigned reads=read_calls,stats=stat_calls;uint32_t revision=storage_directory_revision(view.path);
    struct dfs_filesystem *fs=&volumes[little?STORAGE_FLASH:STORAGE_SD];
    struct dfs_fd writer={fs,fs->ops->fops,"/entry-00000.txt",O_WRONLY,FT_REGULAR,0,NULL};
    CHECK(!writer.fops->open(&writer));entries[0].size=987654;
    CHECK(writer.fops->write(&writer,"new",3)==3);CHECK(!writer.fops->close(&writer));
    CHECK(storage_directory_revision(view.path)==revision);
    view.first=0;scan(&view);CHECK(read_calls==reads&&stat_calls==stats+8);
    file_page_t *page=file_page_find(view.cache,0);CHECK(page&&page->entries[0].size==987654);
    files_dispose(&view);
}
static void recent_window(void){
    files_view_t view=fixture(false,4096);scan(&view);
    for(unsigned page=1;page<40;++page){view.first+=8;scan(&view);}
    unsigned reads=read_calls,opened=opens;
    for(unsigned page=0;page<15;++page){view.first-=8;scan(&view);CHECK(read_calls==reads&&opens==opened);}
    view.first-=8;scan(&view);CHECK(opens==opened+1);
    reads=read_calls;opened=opens;
    for(unsigned page=0;page<15;++page){view.first-=8;scan(&view);CHECK(read_calls==reads&&opens==opened);}
    unsigned valid=0;for(unsigned i=0;i<FILE_PAGE_COUNT;++i)if(view.cache->pages[i].valid)++valid;
    CHECK(valid<=16&&sizeof(*view.cache)<36u*1024u);files_dispose(&view);
}
static void filter_change(void){
    files_view_t view=fixture(false,80);view.picker=true;strcpy(view.pick_extensions,".txt");
    for(unsigned i=0;i<entry_count;i+=2)snprintf(entries[i].name,sizeof(entries[i].name),"image-%u.PNG",i);
    scan(&view);view.first=8;scan(&view);unsigned opened=opens;
    strcpy(view.pick_extensions,".PNG;.md;.mp3");scan(&view);CHECK(!view.first&&opens==opened+1);
    view.picker=false;scan(&view);CHECK(!view.first);files_dispose(&view);
}
static void failure_retry(void){
    files_view_t view=fixture(false,40);scan(&view);view.first=8;
    entries[12].unreadable=true;page_render(&view);CHECK(view.page_failed&&!view.count&&!view.cursor&&!open_handles);
    CHECK(!file_page_find(view.cache,8));entries[12].unreadable=false;scan(&view);CHECK(view.first==8);files_dispose(&view);
}
static void allocation_retry(void){
    files_view_t view=fixture(false,40);fail_alloc=true;page_render(&view);
    CHECK(view.page_failed&&!view.cache&&!opens);fail_alloc=false;scan(&view);files_dispose(&view);
}
static void read_retry(void){
    files_view_t view=fixture(true,40);fail_read_at=4;page_render(&view);
    CHECK(view.page_failed&&!view.count&&!view.cursor&&!open_handles);CHECK(!file_page_find(view.cache,0));
    scan(&view);CHECK(!strcmp(view.names[0],entries[0].name));files_dispose(&view);
}
static void pending_metadata(void){
    files_view_t view=fixture(false,40);scan(&view);entries[8].size=654321;
    view.first=8;scan(&view);file_page_t *page=file_page_find(view.cache,8);CHECK(page&&page->entries[0].size==654321);
    files_dispose(&view);
}
static void lifecycle(unsigned id,bool little){
    files_view_t initial=fixture(little,40);files_view_t *view=malloc(sizeof(*view));CHECK(view);*view=initial;
    files_track(view);current->feature=view;current->id=(int)id;
    page_ops[id].resume();check_page(view);CHECK(view->cursor&&open_handles==1);
    unsigned reads=read_calls,opened=opens;
    page_ops[id].pause();CHECK(view->paused&&!view->cursor&&!open_handles&&live_caches==1);
    page_render(view);CHECK(view->page_failed&&!open_handles&&opens==opened);
    page_ops[id].resume();check_page(view);CHECK(!view->paused&&!view->cursor&&reads==read_calls&&opened==opens);
    view->first=8;page_ops[id].resume();check_page(view);CHECK(view->cursor&&opens==opened+1);
    page_ops[id].stop();CHECK(!current->feature&&!open_handles&&!file_views&&!live_caches);
}
static void handoff(void){
    files_view_t initial=fixture(false,40);files_view_t *views[3];
    for(unsigned i=0;i<3;++i){views[i]=malloc(sizeof(**views));CHECK(views[i]);*views[i]=initial;files_track(views[i]);current->feature=views[i];files_resume();CHECK(views[i]->cursor);}
    current->feature=views[0];files_pause();CHECK(!views[0]->cursor&&open_handles==2);
    current->feature=views[1];current->id=UI_PAGE_FILES;releasing=true;release_request=11;font_release_done=false;
    CHECK(!release_begin());CHECK(!open_handles&&release_notifications==1);
    unsigned opened=opens;files_resume();CHECK(views[1]->page_failed&&!open_handles&&opens==opened);
    font_release_done=true;CHECK(release_begin());CHECK(!open_handles);
    releasing=false;release_request=0;
    for(unsigned i=0;i<3;++i){current->feature=views[i];files_resume();check_page(views[i]);CHECK(!open_handles);files_stop();}
    CHECK(!file_views&&!live_caches);
}
static void sdk_position(void){
    fixture(false,30);storage_lock();DIR *dir=opendir(directory);CHECK(dir);
    CHECK(!strcmp(readdir(dir)->d_name,entries[0].name));CHECK(telldir(dir)==0);
    CHECK(!strcmp(readdir(dir)->d_name,entries[1].name));long offset=telldir(dir);CHECK(offset==(long)sizeof(struct dirent));
    closedir(dir);dir=opendir(directory);seekdir(dir,offset);CHECK(!strcmp(readdir(dir)->d_name,entries[1].name));closedir(dir);storage_unlock();
}
int main(int argc,char **argv){
    const char *cases[]={"sdk_position","fat_empty","fat_exact","fat_roundtrip","fat_partial","fat_large","lfs_small","lfs_large","picker_image","picker_json","picker_extensions","delete","rename","subtree","remount","flash_remount","during_read","layout","seek_failure","stat_recovery","metadata_fat","metadata_lfs","recent_window","filter_change","failure_retry","allocation_retry","read_retry","pending_metadata","lifecycle_files","lifecycle_storage","lifecycle_picker","handoff"};
    if(argc==2&&!strcmp(argv[1],"--list")){for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i)puts(cases[i]);return 0;}
    CHECK(argc==2);const char *name=argv[1];
    if(!strcmp(name,"sdk_position"))sdk_position();
    else if(!strcmp(name,"fat_empty"))roundtrip(false,0);
    else if(!strcmp(name,"fat_exact"))roundtrip(false,8);
    else if(!strcmp(name,"fat_roundtrip"))roundtrip(false,40);
    else if(!strcmp(name,"fat_partial"))roundtrip(false,73);
    else if(!strcmp(name,"fat_large"))roundtrip(false,4096);
    else if(!strcmp(name,"lfs_small"))roundtrip(true,40);
    else if(!strcmp(name,"lfs_large"))roundtrip(true,4096);
    else if(!strcmp(name,"picker_image"))picker_case(0);
    else if(!strcmp(name,"picker_json"))picker_case(1);
    else if(!strcmp(name,"picker_extensions"))picker_case(2);
    else if(!strcmp(name,"during_read"))during_read();
    else if(!strcmp(name,"layout"))layout_change();
    else if(!strcmp(name,"seek_failure"))seek_failure();
    else if(!strcmp(name,"stat_recovery"))stat_recovery();
    else if(!strcmp(name,"flash_remount"))flash_remount();
    else if(!strcmp(name,"metadata_fat"))metadata_refresh(false);
    else if(!strcmp(name,"metadata_lfs"))metadata_refresh(true);
    else if(!strcmp(name,"recent_window"))recent_window();
    else if(!strcmp(name,"filter_change"))filter_change();
    else if(!strcmp(name,"failure_retry"))failure_retry();
    else if(!strcmp(name,"allocation_retry"))allocation_retry();
    else if(!strcmp(name,"read_retry"))read_retry();
    else if(!strcmp(name,"pending_metadata"))pending_metadata();
    else if(!strcmp(name,"lifecycle_files"))lifecycle(UI_PAGE_FILES,false);
    else if(!strcmp(name,"lifecycle_storage"))lifecycle(UI_PAGE_STORAGE,true);
    else if(!strcmp(name,"lifecycle_picker"))lifecycle(UI_PAGE_FILE_PICKER,false);
    else if(!strcmp(name,"handoff"))handoff();
    else mutation_case(name);
    CHECK(!open_handles&&!live_caches&&!file_views);printf("PASS %s\n",name);return 0;
}
