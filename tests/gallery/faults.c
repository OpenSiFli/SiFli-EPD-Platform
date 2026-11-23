#include "host.h"
#include "decode.inc"
#include "store.inc"
#include "catalog.inc"
static gallery_result_t result;
static void snapshot(const gallery_job_t *job) {
    memset(&result,0,sizeof(result));
    if(job->kind==GALLERY_LOCATE)gallery_catalog_locate(job,&result);
    else gallery_catalog_page(job,&result);
}
static void finish(const gallery_job_t *job) {
    for(unsigned i=0;i<20000;++i){snapshot(job);if(result.complete||result.error_code)return;gallery_catalog_step();}
    CHECK(false);
}
int main(void) {
    add("/sdcard",true,0);add("/sdcard/empty",true,0);
    add("/sdcard/epd-ui-check-20261113",true,0);
    add("/sdcard/epd-ui-check-20261113/photos",true,0);
    add("/sdcard/epd-ui-check-20261113/diagrams",true,0);
    add("/sdcard/epd-ui-check-20261113/photos/a.jpg",false,0);
    add("/sdcard/epd-ui-check-20261113/photos/b.jpeg",false,0);
    add("/sdcard/epd-ui-check-20261113/diagrams/a.png",false,0);
    add("/sdcard/epd-ui-check-20261113/diagrams/b.PNG",false,0);
    add("/sdcard/later",true,0);
    for(unsigned i=0;i<200;++i){char p[512];snprintf(p,sizeof(p),"/sdcard/later/%03u.JPG",i);add(p,false,0);}
    gallery_job_t job={.serial=1,.kind=GALLERY_ALBUMS};strcpy(job.path,"/sdcard");
    writable=false;unsigned before=reads;snapshot(&job);CHECK(!result.complete);
    while(!result.count&&!result.error_code){unsigned start=reads;CHECK(gallery_catalog_step());CHECK(reads-start<=32);snapshot(&job);}
    CHECK(result.count&&!result.complete&&reads-before<80);
    CHECK(!strcmp(result.entries[0].path,"/sdcard/epd-ui-check-20261113/photos"));
    finish(&job);CHECK(result.total==3&&result.count==3&&!result.error_code);
    gallery_catalog_clear();CHECK(!live&&!open_fds);
    writable=true;snapshot(&job);write_fail=io_count+1;gallery_catalog_step();write_fail=0;
    CHECK(catalog_fd<0&&valid);snapshot(&job);CHECK(!result.error_code&&!result.complete);finish(&job);CHECK(result.total==3);gallery_catalog_clear();

    job.kind=GALLERY_LOCATE;strcpy(job.path,"/sdcard/epd-ui-check-20261113/photos");strcpy(job.anchor,"/sdcard/epd-ui-check-20261113/photos/b.jpeg");
    finish(&job);CHECK(result.located&&result.index==1&&!result.next[0]&&!strcmp(result.previous,"/sdcard/epd-ui-check-20261113/photos/a.jpg"));
    unsigned opened=opens;strcpy(job.anchor,"/sdcard/epd-ui-check-20261113/photos/a.jpg");snapshot(&job);CHECK(result.index==0&&result.next[0]&&opens==opened);
    /* Remove the earlier ordinal while keeping the selected filename. */
    find_file(job.anchor)->path[0]=0;++rev;strcpy(job.anchor,"/sdcard/epd-ui-check-20261113/photos/b.jpeg");finish(&job);CHECK(result.located&&result.index==0&&!result.previous[0]&&!strcmp(result.path,job.anchor));
    gallery_catalog_clear();writable=false;finish(&job);CHECK(result.located&&result.index==0&&!result.error_code);
    gallery_catalog_clear();writable=true;CHECK(!live&&!open_fds);

    gallery_job_t photo={.serial=1,.kind=GALLERY_IMAGE};strcpy(photo.path,job.anchor);
    gallery_result_t image={.references=1};gallery_decoder_t decoder={.fd=-1};
    CHECK(gallery_cached_render(&photo,&decoder,&image));CHECK(image.cache_pending&&!saving);epd_app_free(decoder.gray);
    gallery_cache_schedule(&photo,&image);CHECK(image.references==2&&saving);CHECK(gallery_cache_step());
    ++content_token;while(gallery_cache_step()){}CHECK(image.references==1&&!saving);char cache[512];cache_path(&photo,cache,sizeof(cache));CHECK(!find_file(cache));
    epd_app_free(image.pixels);image=(gallery_result_t){.references=1};decoder=(gallery_decoder_t){.fd=-1};
    CHECK(gallery_cached_render(&photo,&decoder,&image));epd_app_free(decoder.gray);gallery_cache_schedule(&photo,&image);CHECK(gallery_cache_step());present=false;++card;CHECK(!gallery_cache_step()&&!saving&&image.references==1);present=true;
    epd_app_free(image.pixels);CHECK(!live&&!open_fds);

    int fd=gallery_file_open(photo.path,O_RDONLY,card);CHECK(fd>=0);close_failures=1;CHECK(!gallery_file_close(fd));CHECK(!owned_find(fd)&&!open_fds);
    fd=gallery_file_open(photo.path,O_RDONLY,card);close_failures=2;CHECK(!gallery_file_close(fd)&&owned_find(fd)&&open_fds==1);CHECK(storage_file_release(STORAGE_SD,1)&&!open_fds);
    fd=gallery_file_open(photo.path,O_RDONLY,card);close_permanent=true;CHECK(!gallery_file_close(fd));before=close_calls;CHECK(!storage_file_release(STORAGE_SD,2));CHECK(close_calls==before+1);before=close_calls;for(unsigned i=0;i<100;++i)CHECK(!storage_file_release(STORAGE_SD,2));CHECK(close_calls==before&&open_fds==1);
    present=false;++card;CHECK(!storage_file_release(STORAGE_SD,2)&&close_calls==before+1);before=close_calls;for(unsigned i=0;i<10;++i)CHECK(!storage_file_release(STORAGE_SD,2));CHECK(close_calls==before);
    close_permanent=false;present=true;++card;CHECK(storage_file_release(STORAGE_SD,3)&&!owned_find(fd)&&!open_fds);
    CHECK(!live&&!locks);printf("gallery faults: %u checks; incremental read-only/write failure, path relocation, deferred cache invalidation, retained close ownership\n",checks);return 0;
}
