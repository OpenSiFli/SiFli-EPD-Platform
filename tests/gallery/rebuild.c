#include "host.h"
#include "decode.inc"
#include "store.inc"
#include "catalog.inc"
static gallery_result_t result;
static void snapshot(const gallery_job_t *job) {
    memset(&result,0,sizeof(result));gallery_catalog_page(job,&result);CHECK(!result.error_code);
}
static void finish(const gallery_job_t *job) {
    for(unsigned i=0;i<10000;++i){snapshot(job);if(result.complete)return;gallery_catalog_step();}
    CHECK(false);
}
static void external_write(void) {
    storage_lock();int fd=open("/sdcard/transfer-check.bin",O_CREAT|O_TRUNC|O_RDWR,0);CHECK(fd>=0);
    CHECK(write(fd,"new",3)==3);CHECK(close(fd)==0);storage_unlock();
}
static bool contains(const char *path) {
    for(unsigned i=0;i<result.count;++i)if(!strcmp(result.entries[i].path,path))return true;
    return false;
}
int main(void) {
    add("/sdcard",true,0);
    for(unsigned i=0;i<30;++i){char p[512];snprintf(p,sizeof(p),"/sdcard/album%02u",i);add(p,true,0);
        snprintf(p,sizeof(p),"/sdcard/album%02u/cover.jpg",i);add(p,false,0);}
    gallery_job_t job={.serial=1,.kind=GALLERY_ALBUMS,.first=18};strcpy(job.path,"/sdcard");
    finish(&job);CHECK(result.index==18&&result.total==30&&result.count==9);
    for(unsigned round=0;round<3;++round){
        external_write();snapshot(&job);CHECK(!result.complete&&result.index==18&&!result.count);
        /* Repeated snapshots hit the cached, still incomplete persistent view. */
        snapshot(&job);CHECK(!result.complete&&result.index==18&&!result.count);
        finish(&job);CHECK(result.index==18&&result.total==30&&contains("/sdcard/album18"));
    }
    strcpy(job.anchor,"/sdcard/album18");
    find_file("/sdcard/album00/cover.jpg")->path[0]=0;external_write();
    snapshot(&job);CHECK(!result.complete&&!result.located&&!result.count&&result.index==18);
    finish(&job);CHECK(result.located&&result.index==9&&result.total==29&&contains(job.anchor));
    /* The same path relocation works without a writable catalog. */
    gallery_catalog_clear();writable=false;snapshot(&job);CHECK(!result.complete&&!result.count&&result.index==18);
    finish(&job);CHECK(result.located&&result.index==9&&contains(job.anchor));
    gallery_catalog_clear();writable=true;
    /* Missing anchors clamp only after a complete scan, in either backend. */
    strcpy(job.anchor,"/sdcard/deleted");job.first=99;
    for(unsigned mode=0;mode<2;++mode){writable=mode==0;snapshot(&job);CHECK(!result.complete&&result.index==99&&!result.count);
        finish(&job);CHECK(!result.located&&result.index==27&&result.count==2);gallery_catalog_clear();}
    writable=true;job.kind=GALLERY_SCAN;job.first=18;strcpy(job.path,"/sdcard/album29");
    for(unsigned i=0;i<30;++i){char p[512];snprintf(p,sizeof(p),"/sdcard/album29/%02u.png",i);add(p,false,0);}
    strcpy(job.anchor,"/sdcard/album29/18.png");finish(&job);CHECK(result.located&&contains(job.anchor));
    find_file("/sdcard/album29/00.png")->path[0]=0;external_write();snapshot(&job);
    CHECK(!result.complete&&!result.count&&result.index==18);finish(&job);CHECK(result.located&&contains(job.anchor));
    gallery_catalog_clear();CHECK(!live&&!open_fds&&!locks);
    printf("gallery rebuild: %u checks; external non-image writes, incomplete later pages, album/image path anchors, read-only fallback\n",checks);
    return 0;
}
