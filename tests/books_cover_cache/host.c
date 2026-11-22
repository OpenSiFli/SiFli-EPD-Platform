#define HOST_REAL_COVER_CACHE
#define epd_service_wake unused_service_wake
#define epd_service_ui_request unused_service_ui_request
#include "../books_scan_perf/host_api.h"
#undef epd_service_wake
#undef epd_service_ui_request
static unsigned service_wakes,ui_requests,power_notifies,unlink_failures;
static bool power_event;
static void epd_service_wake(epd_service_t *s) {
    (void)s;++service_wakes;++power_notifies;power_event=true;
}
static void epd_service_ui_request(epd_service_t *s,int delay) {
    (void)s;(void)delay;++ui_requests;++power_notifies;power_event=true;
}
#undef unlink
static int cleanup_unlink(const char *path) {
    if(unlink_failures){--unlink_failures;errno=EACCES;return -1;}
    return host_unlink(path);
}
#define unlink cleanup_unlink
#include "../storage_perf/owned_adapter.h"
#define BUDGET_REGULAR _S_IFREG
#define BUDGET_STORAGE_LOCKED storage_depth
#ifndef RT_IPC_FLAG_PRIO
#define RT_IPC_FLAG_PRIO 1
#endif
#include "../cache_budget/port.h"
bool document_cache_file_signature(const char *path, uint32_t *signature)
{ (void)path;(void)signature;CHECK(false);return false; }
#include "bookshelf.inc"
#include "cover_cache.inc"
#include "shelf_data.inc"

static epd_service_t service;
static unsigned publications, decoded;
static void no_descriptors(void) { for(unsigned i=1;i<32;++i)CHECK(!descriptors[i].f); }
static void worker_drain(void) {
    unsigned steps=0;while(books_cover_cache_worker_step(&service,false))CHECK(++steps<1000);
    CHECK(!books_cover_cache_busy());no_descriptors();
}
static void seed(unsigned n) {
    ui_bookshelf_close();CHECK(!cache.bytes&&!books_cover_cache_busy());
    visible_snapshot=calloc(1,sizeof(*visible_snapshot));CHECK(visible_snapshot);
    books=visible_snapshot->books;count=visible_snapshot->count=n;
    for(unsigned i=0;i<n;++i){
        snprintf(books[i].file.path,sizeof(books[i].file.path),"/flash/persist-%u.epub",i);
        snprintf(books[i].file.name,sizeof(books[i].file.name),"file %u",i);
        books[i].file.format=DOCUMENT_EPUB;books[i].file.size=4;books[i].file.modified=100+i;
        host_file_t *f=find_file(books[i].file.path,false);if(!f||!f->present)put_file(books[i].file.path,"book",4);
    }
    publications=decoded=0;
}
static bool publish(void) {
    cover_ui=true;bool changed=ui_bookshelf_process();cover_ui=false;
    if(changed)++publications;
    return changed;
}
static void pump(unsigned first) {
    unsigned steps=0;
    for(;;){
        bool work=books_cover_cache_worker_step(&service,false);
        if(metadata.document&&!metadata_job.result){metadata_job.result=1;metadata_job.cover=true;++decoded;}
        bool changed=publish();
        if(changed&&ui_bookshelf_covers_ready(first,4))
            for(unsigned i=first;i<count&&i<first+4;++i)CHECK(books[i].cover.image.data);
        if(!work&&!metadata.document&&!metadata.lookup&&!cache.read&&!cache.write&&!cache.active)break;
        CHECK(++steps<3000);
    }
    no_descriptors();CHECK(cache.bytes==0);
}
static void queue_read(unsigned i) {
    book_file_version_t v=book_file_version(books[i].file.path);
    CHECK(books_cover_cache_request(&books[i].file,&v));
}
static int take(void) {
    char title[BOOKSHELF_NAME_MAX]={0};document_bitmap_t image={0};
    int result=books_cover_cache_take(title,sizeof(title),&image);
    if(result>0){CHECK(image.image.header.w==112&&image.image.header.h==146);CHECK(!strcmp(title,"真实书名"));
        CHECK(image.image.data[0]==0x5a&&image.image.data[TEST_COVER_BYTES-1]==0x5a);epd_app_free(image.memory);}
    return result;
}
static void stage_read(void) {
    queue_read(0);CHECK(books_cover_cache_worker_step(&service,false));
    CHECK(books_cover_cache_worker_step(&service,false));CHECK(books_cover_cache_worker_step(&service,false));
    CHECK(cache.active&&cache.active->offset==COVER_CHUNK);
}
static void save_image(unsigned i) {
    document_bitmap_t image={0};metadata_job.result=1;metadata_job.cover=true;
    char title[128];CHECK(document_metadata_take(&metadata_job,title,sizeof(title),&image)==1);
    book_file_version_t version=book_file_version(books[i].file.path);
    cover_ui=true;books_cover_cache_save(&books[i].file,&version,title,&image);cover_ui=false;
    epd_app_free(image.memory);
}
static void idle_reader_cancel_tests(void) {
    books_cover_cache_cancel();
    unsigned wakes=service_wakes,notifications=power_notifies,epoch=cache.epoch;
    unsigned reads=cover_reads,writes=cover_writes,requests=ui_requests;
    power_event=false;
    for(unsigned i=0;i<10000;++i) {
        CHECK(!books_cover_cache_worker_step(&service,true));
        CHECK(!power_event&&service_wakes==wakes&&cache.epoch==epoch);
    }
    CHECK(power_notifies==notifications&&ui_requests==requests&&cover_reads==reads&&cover_writes==writes);
    queue_read(0);save_image(0);wakes=service_wakes;epoch=cache.epoch;
    books_cover_cache_cancel();CHECK(service_wakes==wakes+1&&cache.epoch==epoch+1);
    CHECK(!cache.read&&!cache.write&&!cache.ready&&!cache.bytes);
    for(unsigned writing=0;writing<2;++writing) {
        if(writing) {save_image(0);CHECK(books_cover_cache_worker_step(&service,false));}
        else stage_read();
        close_failures=2;unlink_failures=writing?1:0;
        wakes=service_wakes;epoch=cache.epoch;reads=cover_reads;writes=cover_writes;
        books_cover_cache_cancel();CHECK(service_wakes==wakes+1&&cache.epoch==epoch+1);
        CHECK(books_cover_cache_busy());
        CHECK(books_cover_cache_worker_step(&service,true)&&!books_cover_cache_busy());
        CHECK(storage_file_release(STORAGE_SD,100+writing));
        CHECK(service_wakes==wakes+1&&cache.epoch==epoch+1&&!cache.bytes);
        CHECK(!close_failures&&!unlink_failures&&cover_reads==reads&&cover_writes==writes);
        books_cover_cache_cancel();CHECK(service_wakes==wakes+1&&cache.epoch==epoch+1);
        no_descriptors();
    }
    stage_read();int fd=cache.active->fd;char held_path[STORAGE_PATH_MAX];strcpy(held_path,cache.active->path);
    close_failures=10000;books_cover_cache_shutdown();CHECK(!cache.active&&!cache.bytes&&owned_find(fd));
    unsigned closes=cover_closes;
    for(unsigned i=0;i<1000;++i)CHECK(!books_cover_cache_worker_step(&service,true));
    CHECK(cover_closes==closes&&storage_file_owned_open(held_path,O_RDWR|O_TRUNC,false)<0);
    close_failures=0;++media_session;CHECK(storage_file_release(STORAGE_SD,500));no_descriptors();
    save_image(0);CHECK(books_cover_cache_worker_step(&service,false));
    unlink_failures=10000;books_cover_cache_shutdown();CHECK(!cache.active&&!cache.bytes&&unlink_failures==9999);no_descriptors();
    unlink_failures=0;save_image(0);worker_drain();queue_read(0);worker_drain();CHECK(take()==1);
    stage_read();while(cache.active->phase!=COVER_CLOSE)CHECK(books_cover_cache_worker_step(&service,false));
    close_failures=1;CHECK(books_cover_cache_worker_step(&service,false)&&take()==-1);no_descriptors();
    unsigned saved_count=cache.saved;save_image(0);
    CHECK(books_cover_cache_worker_step(&service,false));
    while(cache.active->phase!=COVER_CLOSE)CHECK(books_cover_cache_worker_step(&service,false));
    close_failures=1;worker_drain();CHECK(cache.saved==saved_count);
    puts("PASS persistent close hands ownership to resident storage and module shutdown finishes; no repeat worker close; held temporary/path cannot truncate; new session reclaims; persistent unlink is bounded and next save recovers");
    puts("PASS 10000 idle reading worker rounds: zero wake/notify/revision changes and no I/O; real queued/active cancellation wakes once; close failures transfer ownership and unlink attempts are bounded; subsequent work recovers");
}
int main(void) {
    seed(4);unsigned scans=signature_calls;
    ui_bookshelf_metadata_request(0,4);publish();pump(0);
    CHECK(decoded==4&&publications==1&&cache.saved==4);
    CHECK(signature_calls==scans&&cover_read_max<=COVER_CHUNK&&cover_write_max<=COVER_CHUNK);
    unsigned opens_before=metadata_opens,hits=cache.hits;
    seed(4);ui_bookshelf_metadata_request(0,4);publish();pump(0);
    CHECK(!decoded&&metadata_opens==opens_before&&cache.hits==hits+4&&publications==1);
    CHECK(books[0].cover.image.header.cf==LV_COLOR_FORMAT_RGB565);
    puts("PASS cold extraction persists four covers; module teardown/reload restores pixels and titles with zero decoder opens and one page publication; no source hash");

    opens_before=metadata_opens;file_changed(find_file(books[0].file.path,false));
    publish();CHECK(metadata.document&&!metadata.lookup);pump(0);
    CHECK(metadata_opens==opens_before+1&&books[0].cover.memory);
    puts("PASS runtime source mutation bypasses an old disk thumbnail before catalog size/mtime can be refreshed");

    books_cover_cache_cancel();queue_read(0);worker_drain();
    display_busy=true;unsigned read_hits=cache.hits;CHECK(!publish()&&cache.hits==read_hits);
    display_busy=false;CHECK(take()==1);
    for(unsigned change=0;change<6;++change){
        bookshelf_item_t old=books[0].file;
        if(change==0)++books[0].file.modified;
        if(change==1)++books[0].file.size;
        if(change==2)books[0].file.format=DOCUMENT_SPLIT_EPUB;
        cover_key_t key;cover_key(&key,&old);char path[512];CHECK(cover_path(&key,STORAGE_SD,path,sizeof(path)));
        host_file_t *f=find_file(path,false);CHECK(f&&f->present);cover_header_t saved;memcpy(&saved,f->data,sizeof(saved));
        if(change==3)((cover_header_t *)f->data)->version++;
        if(change==4)((cover_header_t *)f->data)->key.width_limit++;
        if(change==5)f->data[sizeof(saved)]^=1;
        queue_read(0);worker_drain();CHECK(take()==-1);
        books[0].file=old;memcpy(f->data,&saved,sizeof(saved));if(change==5)f->data[sizeof(saved)]^=1;
    }
    puts("PASS metadata, format, cache version, target geometry and corrupt pixels reject stale thumbnails; display-busy publication remains deferred");

    for(unsigned reason=0;reason<6;++reason){
        stage_read();unsigned reads=cover_reads;close_failures=1;
        if(reason==0)books_cover_cache_cancel();
        if(reason==1)file_changed(find_file(books[0].file.path,false));
        if(reason==2)++media_session;
        if(reason==3)books_cover_cache_cancel();
        if(reason==4)service_cancelled=true;
        CHECK(books_cover_cache_worker_step(&service,reason==5)&&!books_cover_cache_busy());
        CHECK(cache.bytes==0&&cover_reads==reads);
        memory_pressure=service_cancelled=false;books_cover_cache_cancel();no_descriptors();
    }
    stage_read();file_changed(find_file(books[0].file.path,false));worker_drain();CHECK(take()==-1);
    stage_read();++media_session;worker_drain();CHECK(take()==-1);
    puts("PASS cancel, reader priority, source/media replacement, pressure and service shutdown discard partial data; failed close transfers descriptor ownership to resident storage");

    for(unsigned fail=0;fail<3;++fail){
        unsigned saved=cache.saved;save_image(0);CHECK(cache.write);
        if(fail==0)write_failures=1;if(fail==1)commit_failures=1;if(fail==2)disk_free=COVER_DISK_RESERVE;
        worker_drain();disk_free=64u*1024u*1024u;CHECK(cache.saved==saved&&!cache.bytes);
        queue_read(0);worker_drain();CHECK(take()==1);
    }
    CHECK(books[0].cover.memory);display_busy=true;
    size_t released=0;
    CHECK(shelf_resource_shrink(NULL,1,SIZE_MAX,&released)==EPD_RESOURCE_BUSY&&!released);
    CHECK(!publish()&&books[0].cover.memory);display_busy=false;
    CHECK(shelf_resource_shrink(NULL,1,SIZE_MAX,&released)==EPD_RESOURCE_OK&&released);
    CHECK(publish());for(unsigned i=0;i<count;++i)CHECK(!books[i].cover.memory);
    CHECK(!cache.bytes);memory_pressure=false;pump(0);CHECK(books[0].cover.memory);
    available_bytes=128u*1024u;book_file_version_t v=book_file_version(books[0].file.path);
    CHECK(books_cover_cache_request(&books[0].file,&v));worker_drain();CHECK(take()==1);
    allocation_fail=true;CHECK(!books_cover_cache_request(&books[0].file,&v));allocation_fail=false;
    available_bytes=8u*1024u*1024u;
    puts("PASS write/commit/space failures preserve published covers; provider reclaims idle shelf pixels, protects display, and admitted reads work below the former floor");

    document_bitmap_t full={0};full.memory=malloc(COVER_MAX_BYTES);CHECK(full.memory);
    full.image.header=(host_image_header_t){LV_IMAGE_HEADER_MAGIC,LV_COLOR_FORMAT_RGB565,168,220,336};
    full.image.data=full.memory;full.image.data_size=COVER_MAX_BYTES;memset(full.memory,0x5a,COVER_MAX_BYTES);
    v=book_file_version(books[0].file.path);books_cover_cache_save(&books[0].file,&v,"max",&full);
    CHECK(books_cover_cache_worker_step(&service,false));
    books_cover_cache_save(&books[0].file,&v,"max",&full);CHECK(cache.bytes==COVER_IO_BUDGET);
    books_cover_cache_save(&books[0].file,&v,"max",&full);CHECK(cache.bytes==COVER_IO_BUDGET);
    free(full.memory);books_cover_cache_cancel();close_failures=1;
    CHECK(books_cover_cache_worker_step(&service,false)&&!books_cover_cache_busy()&&!cache.bytes);
    memory_pressure=false;
    puts("PASS maximum 168x220 buffers share a two-thumbnail I/O budget and are reclaimed before a failing descriptor close completes");

    idle_reader_cancel_tests();
    seed(32);media_present[STORAGE_SD]=false;
    for(unsigned i=0;i<count;++i){save_image(i);worker_drain();}
    unsigned files=0,older=UINT_MAX,newer=0;char paths[32][512];
    for(unsigned i=0;i<count;++i){cover_key_t key;cover_key(&key,&books[i].file);CHECK(cover_path(&key,STORAGE_FLASH,paths[i],512));
        for(unsigned j=0;j<i;++j)if(!strcmp(paths[i],paths[j])){older=j;newer=i;}}
    for(unsigned i=0;i<fs_count;++i)if(fs[i].present&&strstr(fs[i].path,"/flash/cache/books/covers/thumb-"))++files;
    CHECK(files<=COVER_SLOTS&&older!=UINT_MAX);
    queue_read(older);worker_drain();CHECK(take()==-1);queue_read(newer);worker_drain();CHECK(take()==1);
    books[0].file.format=DOCUMENT_SPLIT_EPUB;save_image(0);worker_drain();queue_read(0);worker_drain();CHECK(take()==1);
    queue_read(0);CHECK(books_cover_cache_worker_step(&service,false));file_changed(find_file("/flash/unrelated",true));worker_drain();CHECK(take()==-1);
    ui_bookshelf_close();CHECK(!cache.bytes);no_descriptors();
    printf("PASS 32 books occupy %u fixed slots; full paths reject collisions; TF preferred then internal storage; split-source mutation guarded; all I/O <=4096 bytes\n",files);
    return 0;
}
