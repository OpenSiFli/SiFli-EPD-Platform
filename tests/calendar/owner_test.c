#define REMINDER_OWNER_TEST
#define main reminder_functional_main
#include "reminder_test.c"
#undef main

typedef struct {epd_memory_owner_t root,service,worker,previous;} caller_t;
static caller_t caller_open(const char *id)
{
    service_app=id;
    caller_t c;c.root=epd_memory_owner_create(id);
    c.service=epd_memory_scope_create(c.root,EPD_MEMORY_SCOPE_SERVICE);
    c.worker=epd_memory_scope_create(c.service,EPD_MEMORY_SCOPE_TASK);
    c.previous=epd_memory_owner_enter(c.worker);
    CHECK(c.previous==EPD_MEMORY_OWNER_SYSTEM);return c;
}
static void caller_close(caller_t c)
{
    epd_memory_owner_snapshot_t s;
    CHECK(epd_memory_owner_current()==c.worker);epd_memory_owner_leave(c.previous);
    epd_memory_owner_close(c.worker);epd_memory_owner_release(c.worker);
    epd_memory_owner_close(c.service);epd_memory_owner_release(c.service);
    epd_memory_owner_close(c.root);
    CHECK(epd_memory_owner_get(c.root,&s)&&s.references==1);
    CHECK(!epd_memory_owner_get(c.worker,&s)&&!epd_memory_owner_get(c.service,&s));
    epd_memory_owner_release(c.root);CHECK(!epd_memory_owner_get(c.root,&s));
}
static void discard_resident(void)
{
    while(agendas){agenda_t *a=agendas;agendas=a->next;epd_app_free(a->entries);epd_app_free(a);}
    while(acknowledgements){ack_t *a=acknowledgements;acknowledgements=a->next;epd_app_free(a);}
    restored=false;CHECK(!physical_used);
}
static epd_reminder_t rule(void)
{
    epd_reminder_t r={0};r.id=1;strcpy(r.title,"resident reminder");
    r.rule.start_day=day(2026,11,19);r.rule.until_day=r.rule.start_day;
    r.rule.minute=600;r.rule.timezone_minutes=480;return r;
}
static void resident_owners(void)
{
    for(agenda_t *a=agendas;a;a=a->next){
        CHECK(((allocation_t *)a-1)->owner==EPD_MEMORY_OWNER_SYSTEM);
        CHECK(((allocation_t *)a->entries-1)->owner==EPD_MEMORY_OWNER_SYSTEM);
    }
}
static void load_owned(epd_memory_owner_t owner,const char *expected)
{
    void *data=NULL;size_t size=0;
    CHECK(epd_reminder_load((void*)1,&data,&size)==EPD_REMINDER_OK);
    CHECK(data&&size==strlen(expected)+1&&!strcmp(data,expected));
    CHECK(((allocation_t *)data-1)->owner==owner&&current_owner==owner);
    epd_app_free(data);
}
static void repeated_commits(void)
{
    const char *ids[]={"calendr","probe","weather"};size_t stable[6]={0};
    epd_reminder_t r=rule();restore();
    for(unsigned cycle=0;cycle<24;++cycle){
        caller_t c=caller_open(ids[cycle%3]);
        unsigned systems=system_allocations,callers=caller_allocations;
        CHECK(epd_reminder_commit((void*)1,"all",4,&r,cycle&1)==EPD_REMINDER_OK);
        CHECK(epd_memory_owner_current()==c.worker);
#ifdef REMINDER_REPRODUCE_RETENTION
        epd_memory_owner_leave(c.previous);
        epd_memory_owner_close(c.worker);epd_memory_owner_release(c.worker);
        epd_memory_owner_close(c.service);epd_memory_owner_release(c.service);
        epd_memory_owner_close(c.root);epd_memory_owner_snapshot_t root,service,worker;
        CHECK(epd_memory_owner_get(c.root,&root)&&root.closed&&root.references==2);
        CHECK(epd_memory_owner_get(c.service,&service)&&service.closed&&service.references==1);
        CHECK(epd_memory_owner_get(c.worker,&worker)&&worker.closed&&!worker.references&&worker.allocations==2);
        discard_resident();epd_memory_owner_release(c.root);
        CHECK(!epd_memory_owner_get(c.root,&root));
        puts("PASS negative control: full reminder commit retains closed root refs=2, service refs=1, worker refs=0 blocks=2 without platform ownership boundary");return;
#endif
        CHECK(system_allocations-systems==2&&caller_allocations-callers==1);
        resident_owners();load_owned(c.worker,"all");
        CHECK(reminder_remove_app(service_app)&&!find(service_app)->header.enabled);
        load_owned(c.worker,"all");CHECK(find(service_app)->header.enabled);
        caller_close(c);
        if(cycle>=6&&cycle<12)stable[cycle%6]=physical_used;
        if(cycle>=12)CHECK(physical_used==stable[cycle%6]);
    }
    discard_resident();host_reset();
    puts("PASS production commit/load/remove: 24 cycles, three real identities, stable resident bytes, caller root/service/worker collected");
}
static void failure_paths(void)
{
    restore();caller_t c=caller_open("probe");epd_reminder_t r=rule();
    for(int existing=0;existing<2;++existing){
        if(existing)CHECK(epd_reminder_commit((void*)1,"old",4,&r,1)==EPD_REMINDER_OK);
        size_t before=physical_used;agenda_t *old=find("probe");entry_t *entries=old?old->entries:NULL;
        fail_system_enter=true;
        CHECK(epd_reminder_commit((void*)1,"new",4,&r,1)==EPD_REMINDER_MEMORY);
        CHECK(physical_used==before&&epd_memory_owner_current()==c.worker);
        for(int fail=0;fail<3;++fail){
            fail_allocation=fail;
            CHECK(epd_reminder_commit((void*)1,"new",4,&r,1)==(fail<2?EPD_REMINDER_MEMORY:EPD_REMINDER_IO));
            CHECK(physical_used==before&&epd_memory_owner_current()==c.worker);
            CHECK(find("probe")==old&&(!old||old->entries==entries));
        }
        fail_allocation=-1;host_fail_after(0);
        CHECK(epd_reminder_commit((void*)1,"new",4,&r,1)==EPD_REMINDER_IO);
        CHECK(physical_used==before);host_fail_after(-1);
        if(existing){
            load_owned(c.worker,"old");
            for(int fail=0;fail<2;++fail){
                void *p=(void*)1;size_t n=123;fail_allocation=fail;
                CHECK(epd_reminder_load((void*)1,&p,&n)==(fail?EPD_REMINDER_MEMORY:EPD_REMINDER_IO));
                CHECK(!p&&!n&&physical_used==before&&epd_memory_owner_current()==c.worker);
            }
            fail_allocation=-1;
        }
    }
    caller_close(c);discard_resident();host_reset();
    puts("PASS new/replacement failures: context entry, both resident allocations, persistence scratch, I/O, both load allocations; old data preserved");
}
static void lazy_restore(void)
{
    epd_reminder_t r=rule();caller_t a=caller_open("alpha");
    CHECK(epd_reminder_commit((void*)1,"seed",5,&r,1)==EPD_REMINDER_OK);
    caller_close(a);discard_resident();
    caller_t b=caller_open("beta");
    CHECK(epd_reminder_commit((void*)1,"next",5,&r,1)==EPD_REMINDER_OK);
    CHECK(find("alpha")&&find("beta"));resident_owners();caller_close(b);discard_resident();
    /* Commit allocations precede restore's read, agenda and entry allocations. */
    for(int fail=2;fail<10;++fail){
        b=caller_open("beta");fail_allocation=fail;
        (void)epd_reminder_commit((void*)1,"next",5,&r,1);
        fail_allocation=-1;CHECK(epd_memory_owner_current()==b.worker);
        resident_owners();caller_close(b);discard_resident();
    }
    b=caller_open("beta");
    CHECK(epd_reminder_commit((void*)1,"next",5,&r,1)==EPD_REMINDER_OK);
    resident_owners();load_owned(b.worker,"next");service_app="alpha";load_owned(b.worker,"seed");
    caller_close(b);discard_resident();host_reset();
    puts("PASS production lazy restore inside commit: resident ownership and allocation failure cleanup, disk records remain loadable");
}
static void credit_and_ack(void)
{
    epd_reminder_t r=rule();fake_now=(int64_t)r.rule.start_day*86400+3600;
    caller_t c=caller_open("probe");epd_memory_credit_spec_t spec={0};spec.bytes[backend]=65536;
    epd_memory_credit_t credit;CHECK(epd_memory_credit_request(c.worker,&spec,&credit)==EPD_MEMORY_OK);
    epd_memory_owner_t previous=epd_memory_owner_enter(credit);CHECK(previous==c.worker);
    epd_memory_credit_snapshot_t before,after;CHECK(epd_memory_credit_get(credit,&before));
    CHECK(epd_reminder_commit((void*)1,"credit",7,&r,1)==EPD_REMINDER_OK);
    resident_owners();load_owned(credit,"credit");
    CHECK(epd_memory_credit_current()==credit&&epd_memory_owner_current()==c.worker);
    CHECK(epd_memory_credit_get(credit,&after)&&before.unused[backend]==after.unused[backend]);
    epd_memory_owner_leave(previous);CHECK(epd_memory_credit_end(credit));epd_memory_owner_release(credit);
    caller_close(c);fake_now+=3600;process();CHECK(publications==1&&published.id[0]);
    c=caller_open("viewer");fail_system_enter=true;
    CHECK(!reminder_dismiss("probe",published.id)&&!acknowledgements);
    fail_allocation=0;CHECK(!reminder_dismiss("probe",published.id)&&!acknowledgements);fail_allocation=-1;
    CHECK(reminder_dismiss("probe",published.id)&&acknowledgements);
    CHECK(((allocation_t *)acknowledgements-1)->owner==EPD_MEMORY_OWNER_SYSTEM);
    CHECK(((allocation_t *)acknowledgements-1)->region==EPD_APP_SRAM);
    caller_close(c);process();CHECK(!acknowledgements&&!find("probe")->header.notice.id[0]);
    discard_resident();host_reset();
    puts("PASS caller credit preserved; reminder fires after owner retirement; deferred dismissal survives caller close and is freed by process");
}
int main(void)
{
    CHECK(epd_memory_init()==EPD_MEMORY_OK);
    epd_memory_backend_config_t config={EPD_MEMORY_REGION_HCPU_PSRAM,1024*1024,0,EPD_MEMORY_BACKEND_NONE};
    CHECK(epd_memory_backend_register(&backend,&config,&backend)==EPD_MEMORY_OK);
    config.region=EPD_MEMORY_REGION_SRAM;
    CHECK(epd_memory_backend_register(&sram_backend,&config,&sram_backend)==EPD_MEMORY_OK);
    host_reset();reminder_start();repeated_commits();
#ifndef REMINDER_REPRODUCE_RETENTION
    failure_paths();lazy_restore();credit_and_ack();
#endif
    epd_memory_metadata_snapshot_t metadata;epd_memory_metadata_snapshot(&metadata);
    CHECK(!physical_used&&!metadata.protocol_errors&&metadata.descriptors_used==1);
    puts("PASS owner accounting: only SYSTEM descriptor remains; no live test allocations or protocol errors");return 0;
}
