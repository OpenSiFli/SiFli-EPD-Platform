static void budget_write(const char *path, epd_cold_class_t kind, size_t size)
{
    epd_cold_lease_t lease;
    CHECK(epd_cold_budget_begin(path,kind,size,0,&lease)==EPD_COLD_BUDGET_OK);
    FILE *file=fopen(path,"wb");CHECK(file && size && size<=LONG_MAX);
    CHECK(!fseek(file,(long)size-1,SEEK_SET) && fputc(0,file)!=EOF && !fclose(file));
    CHECK(epd_cold_budget_finish(lease,kind,true)==EPD_COLD_BUDGET_OK);
}
static bool budget_exists(const char *path)
{ FILE *f=fopen(path,"rb");if(!f)return false;CHECK(!fclose(f));return true; }
static epd_cold_budget_snapshot_t budget_snapshot(void)
{ epd_cold_budget_snapshot_t s;epd_cold_budget_snapshot(STORAGE_FLASH,&s);return s; }
static void budget_reset(void)
{
    struct rt_object_global_scope scope={0};epd_memory_owner_t saved=context;context=0;
    CHECK(!rt_object_global_scope_enter(&scope));CHECK(!test_init_cold_budget_init());
    CHECK(!rt_object_global_scope_leave(&scope));context=saved;
}
static void budget_order(void)
{
    current=&threads[1];
    budget_write("./state.bin",EPD_COLD_OPTIONAL,3u*1024u*1024u);
    budget_write("./derived.bin",EPD_COLD_DERIVED,3u*1024u*1024u);
    budget_write("./required.bin",EPD_COLD_REQUIRED,3u*1024u*1024u);
    CHECK(budget_exists("./state.bin") && !budget_exists("./derived.bin"));
    CHECK(budget_snapshot().evicted_derived==1 && !budget_snapshot().evicted_optional);
    budget_write("./required2.bin",EPD_COLD_REQUIRED,3u*1024u*1024u);
    CHECK(!budget_exists("./state.bin") && budget_exists("./required.bin"));
    CHECK(budget_snapshot().evicted_optional==1 && !budget_snapshot().reserved);
}
static void budget_metadata(void)
{
    current=&threads[1];context=2;refs[2]=1;
    epd_cold_budget_snapshot_t before=budget_snapshot();
    CHECK(!budget_control_memory && !before.records && before.index_bytes && !io_calls);
    epd_cold_lease_t lease=1;
    fail_alloc=true;
    CHECK(epd_cold_budget_begin("./metadata",EPD_COLD_REQUIRED,1,0,&lease)==EPD_COLD_BUDGET_IO);
    CHECK(!lease && !budget_control_memory && !global_scope && context==2 && !io_calls);
    fail_alloc=false;budget_mutex_fail=true;
    CHECK(epd_cold_budget_begin("./metadata",EPD_COLD_REQUIRED,1,0,&lease)==EPD_COLD_BUDGET_IO);
    CHECK(!budget_control_memory && !global_scope && context==2 && !io_calls);
    budget_mutex_fail=false;budget_control_reenter=true;
    CHECK(epd_cold_budget_begin("./metadata",EPD_COLD_REQUIRED,1,0,&lease)==EPD_COLD_BUDGET_OK);
    CHECK(budget_control_reentered==EPD_COLD_BUDGET_BUSY && budget_control_memory && context==2 && !global_scope);
    CHECK(epd_cold_budget_finish(lease,EPD_COLD_REQUIRED,false)==EPD_COLD_BUDGET_OK);
    budget_reset();CHECK(!budget_control_memory);context=0;refs[2]=0;
}
static void budget_required(void)
{
    current=&threads[1];
    budget_write("./unique.bin",EPD_COLD_REQUIRED,6u*1024u*1024u);
    epd_cold_lease_t lease=123;
    CHECK(epd_cold_budget_begin("./new.bin",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&lease)==EPD_COLD_BUDGET_FULL);
    CHECK(!lease && budget_exists("./unique.bin"));
    CHECK(!budget_snapshot().evicted_optional && !budget_snapshot().evicted_derived);
    CHECK(epd_cold_budget_begin("./unique.bin",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&lease)==EPD_COLD_BUDGET_FULL);
    CHECK(budget_exists("./unique.bin")); /* Replacement pays old + new peak. */
}
static void budget_slots(void)
{
    current=&threads[1];
    budget_write("./unique.bin",EPD_COLD_REQUIRED,16);
    budget_write("./state.bin",EPD_COLD_OPTIONAL,16);
    budget_write("./pinned.bin",EPD_COLD_DERIVED,16);
    epd_cold_lease_t pinned;
    CHECK(epd_cold_budget_begin("./pinned.bin",EPD_COLD_DERIVED,0,0,&pinned)==EPD_COLD_BUDGET_OK);
    for(unsigned i=0;i<EPD_COLD_BUDGET_RECORDS*3;++i) {
        char path[64];snprintf(path,sizeof(path),"./derived-%u.bin",i);
        budget_write(path,EPD_COLD_DERIVED,16);
        CHECK(budget_exists(path) && budget_exists("./unique.bin") &&
              budget_exists("./state.bin") && budget_exists("./pinned.bin"));
    }
    CHECK(budget_snapshot().records==EPD_COLD_BUDGET_RECORDS);
    CHECK(budget_snapshot().evicted_derived==EPD_COLD_BUDGET_RECORDS*2+3);
    CHECK(!budget_snapshot().evicted_optional && budget_snapshot().active==1);
    CHECK(epd_cold_budget_finish(pinned,EPD_COLD_DERIVED,true)==EPD_COLD_BUDGET_OK);
    CHECK(!budget_snapshot().active && !budget_snapshot().reserved);
}
static void budget_leases(void)
{
    current=&threads[1];
    budget_write("./pinned.bin",EPD_COLD_DERIVED,3u*1024u*1024u);
    epd_cold_lease_t read,write,rejected;
    CHECK(epd_cold_budget_begin("./pinned.bin",EPD_COLD_DERIVED,0,0,&read)==EPD_COLD_BUDGET_OK);
    CHECK(epd_cold_budget_begin("./pinned.bin",EPD_COLD_DERIVED,1,0,&rejected)==EPD_COLD_BUDGET_BUSY);
    CHECK(epd_cold_budget_begin("./inflight.bin",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&write)==EPD_COLD_BUDGET_OK);
    CHECK(epd_cold_budget_begin("./third.bin",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&rejected)==EPD_COLD_BUDGET_FULL);
    CHECK(budget_exists("./pinned.bin") && budget_snapshot().active==2);
    CHECK(epd_cold_budget_finish(write,EPD_COLD_REQUIRED,false)==EPD_COLD_BUDGET_OK);
    CHECK(epd_cold_budget_finish(read,EPD_COLD_DERIVED,true)==EPD_COLD_BUDGET_OK);
    CHECK(epd_cold_budget_finish(read,EPD_COLD_DERIVED,true)==EPD_COLD_BUDGET_INVALID);
    CHECK(!budget_snapshot().reserved && !budget_snapshot().active);
}
static void budget_reboot(void)
{
    current=&threads[1];budget_write("./derived.bin",EPD_COLD_DERIVED,6u*1024u*1024u);
    budget_reset();budget_write("./state.bin",EPD_COLD_REQUIRED,3u*1024u*1024u);
    CHECK(!budget_exists("./derived.bin") && budget_snapshot().evicted_derived==1);
    unsigned before=index_commits;
    for(unsigned i=0;i<100;++i) {
        epd_cold_lease_t lease;
        CHECK(epd_cold_budget_begin("./state.bin",EPD_COLD_REQUIRED,0,0,&lease)==EPD_COLD_BUDGET_OK);
        CHECK(epd_cold_budget_finish(lease,EPD_COLD_REQUIRED,true)==EPD_COLD_BUDGET_OK);
    }
    CHECK(index_commits==before); /* Reads never rewrite the index or CRC payloads. */
}
static void budget_index_failure(void)
{
    current=&threads[1];budget_write("./old.bin",EPD_COLD_OPTIONAL,1024);
    epd_cold_lease_t lease;
    fail_index=true;
    CHECK(epd_cold_budget_begin("./old.bin",EPD_COLD_REQUIRED,2048,0,&lease)==EPD_COLD_BUDGET_IO);
    CHECK(budget_exists("./old.bin") && !lease && !budget_snapshot().reserved);
    fail_index=false;budget_write("./old.bin",EPD_COLD_REQUIRED,6u*1024u*1024u);
    budget_reset();
    CHECK(epd_cold_budget_begin("./other.bin",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&lease)==EPD_COLD_BUDGET_FULL);
    CHECK(budget_exists("./old.bin"));
}
static void budget_cold_cross_class(void)
{
    current=&threads[1];
    size_t fill=EPD_COLD_BUDGET_VOLUME_BYTES-budget_snapshot().index_bytes-1024u;
    budget_write("./pixels.frame",EPD_COLD_DERIVED,fill);
    current=&threads[0];epd_cold_state_spec_t spec=cold_spec("any-owner/position",1,false);epd_cold_state_t id;
    CHECK(epd_cold_state_put(&spec,"only user data",15,&id));cold_pump();
    CHECK(cold_get(id).state==EPD_COLD_STATE_SAVED && !budget_exists("./pixels.frame"));
    CHECK(epd_cold_state_release(id) && !refs[4]);
    CHECK(budget_snapshot().evicted_derived==1 && !budget_snapshot().active && !budget_snapshot().reserved);
}
static void budget_deferred(void)
{
    current=&threads[1];epd_cold_lease_t lease,other;
    CHECK(epd_cold_budget_begin("./frame.bin",EPD_COLD_DERIVED,6u*1024u*1024u,0,&lease)==EPD_COLD_BUDGET_OK);
    char path[128];CHECK(epd_cold_budget_temporary("./frame.bin",path,sizeof(path)));
    FILE *f=fopen(path,"wb");CHECK(f && fputc(7,f)!=EOF && !fclose(f));blocked_file=path;
    CHECK(epd_cold_budget_finish(lease,EPD_COLD_DERIVED,false)==EPD_COLD_BUDGET_IO);
    CHECK(budget_snapshot().deferred==1 && budget_snapshot().reserved==6u*1024u*1024u);
    CHECK(epd_cold_budget_begin("./next.bin",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&other)==EPD_COLD_BUDGET_FULL);
    blocked_file=NULL;
    CHECK(epd_cold_budget_begin("./next.bin",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&other)==EPD_COLD_BUDGET_OK);
    CHECK(!budget_exists(path) && !budget_snapshot().deferred);
    CHECK(epd_cold_budget_finish(other,EPD_COLD_REQUIRED,false)==EPD_COLD_BUDGET_OK);
    CHECK(!budget_snapshot().reserved);
}
static void budget_media(void)
{
    current=&threads[1];budget_sd=true;epd_cold_lease_t lease,next;
    CHECK(epd_cold_budget_begin("./sd/frame",EPD_COLD_DERIVED,1024,1,&lease)==EPD_COLD_BUDGET_OK);
    ++budget_session;
    FILE *f=fopen("./sd/frame.stage","wb");CHECK(f && fputc(8,f)!=EOF && !fclose(f));
    CHECK(epd_cold_budget_finish(lease,EPD_COLD_DERIVED,false)==EPD_COLD_BUDGET_IO);
    epd_cold_budget_snapshot_t s;epd_cold_budget_snapshot(STORAGE_SD,&s);CHECK(s.deferred==1 && s.reserved==1024);
    CHECK(epd_cold_budget_begin("./flash-trigger",EPD_COLD_REQUIRED,1,0,&next)==EPD_COLD_BUDGET_OK);
    CHECK(epd_cold_budget_finish(next,EPD_COLD_REQUIRED,false)==EPD_COLD_BUDGET_OK);
    epd_cold_budget_snapshot(STORAGE_SD,&s);CHECK(!s.deferred && !s.reserved && !s.records);
    CHECK(budget_exists("./sd/frame.stage")); /* Never remove a new card's same-named file. */
}
static void budget_reboot_pending(void)
{
    current=&threads[1];budget_write("./frame",EPD_COLD_DERIVED,1024);
    FILE *f=fopen("./frame.stage","wb");CHECK(f && !fseek(f,6u*1024u*1024u-1u,SEEK_SET) && fputc(1,f)!=EOF && !fclose(f));
    budget_reset();blocked_file="./frame.stage";epd_cold_lease_t lease;
    CHECK(epd_cold_budget_begin("./need",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&lease)==EPD_COLD_BUDGET_FULL);
    CHECK(budget_snapshot().deferred==1 && budget_snapshot().reserved==6u*1024u*1024u);
    blocked_file=NULL;
    CHECK(epd_cold_budget_begin("./need",EPD_COLD_REQUIRED,3u*1024u*1024u,0,&lease)==EPD_COLD_BUDGET_OK);
    CHECK(epd_cold_budget_finish(lease,EPD_COLD_REQUIRED,false)==EPD_COLD_BUDGET_OK);
    CHECK(!budget_exists("./frame.stage") && !budget_snapshot().reserved);
}
static void budget_corrupt_index(void)
{
    current=&threads[1];budget_write("./only-copy",EPD_COLD_REQUIRED,1024);
    FILE *f=fopen("./system/cold-budget.bin","r+b");CHECK(f && fputc(0,f)!=EOF && !fclose(f));
    budget_reset();epd_cold_lease_t lease;
    CHECK(epd_cold_budget_begin("./new",EPD_COLD_DERIVED,1024,0,&lease)==EPD_COLD_BUDGET_IO);
    CHECK(!lease && budget_exists("./only-copy"));
}
