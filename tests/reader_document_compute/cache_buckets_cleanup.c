/* Actual document cache_open + cleanup_work, including failed O_EXCL ownership. */
#define main unused_hcpu_main
#include "body_nav_host.inc"
#undef main

static document_t fresh_spool(void)
{
    document_t d={0}; d.input=d.output=-1; d.cached=(document_cache_t *)&cache_token;
    d.generation=d.cache_generation=storage_card_session(); return d;
}
static void retained(const char *path, const char *text)
{
    FILE *f=fopen(path,"rb"); assert(f); char buffer[64]={0};
    size_t n=fread(buffer,1,sizeof(buffer)-1,f); fclose(f);
    assert(n==strlen(text)&&!strcmp(buffer,text));
}
int main(int argc, char **argv)
{
    assert(argc==2); InitializeCriticalSection(&irq_lock); InitializeCriticalSection(&io_lock);
    snprintf(test_root,sizeof(test_root),"%s",argv[1]);
    for(unsigned i=1;i<=8;++i) {
        char p[600]; snprintf(p,sizeof(p),"%s/%08x.tmp",test_root,i);
        FILE *f=fopen(p,"wb"); assert(f); fputs("unowned",f); fclose(f);
    }
    document_t d=fresh_spool(); unsigned before=fd_open_attempts;
    fd_open_error=EEXIST;
    assert(!cache_open(&d)&&!d.temp_owned&&fd_open_attempts-before==8);
    fd_open_error=0;
    FILE *conflict=fopen(d.cache,"wb"); assert(conflict); fputs("unowned",conflict); fclose(conflict);
    cleanup_work(&d); assert(d.cleanup_done);
    retained(d.cache,"unowned");
    for(unsigned i=1;i<=8;++i) {
        char p[600]; snprintf(p,sizeof(p),"%s/%08x.tmp",test_root,i); retained(p,"unowned");
    }
    puts("PASS actual cleanup preserves every O_EXCL conflict");
    document_t live=fresh_spool(),other=fresh_spool();
    assert(cache_open(&live)&&cache_open(&other)&&strcmp(live.cache,other.cache));
    cleanup_work(&live); struct stat st; assert(stat(live.cache,&st)!=0&&stat(other.cache,&st)==0);
    cleanup_work(&other); assert(stat(other.cache,&st)!=0);
    puts("PASS actual cleanup removes only its owned spool");
    sd_media=true; document_t removed=fresh_spool(); assert(cache_open(&removed));
    storage_lock(); assert(close(removed.output)==0); storage_unlock(); removed.output=-1;
    ++media_generation;
    FILE *replacement=fopen(removed.cache,"wb"); assert(replacement); fputs("replacement",replacement); fclose(replacement);
    cleanup_work(&removed); retained(removed.cache,"replacement");
    puts("PASS actual cleanup leaves replacement-card path untouched");
    assert(!allocated_bytes&&!fd_live); DeleteCriticalSection(&io_lock); DeleteCriticalSection(&irq_lock);
    return 0;
}
