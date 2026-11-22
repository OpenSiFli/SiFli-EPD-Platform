#include "notes_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static long fail_after=-1;
static unsigned allocations, encodes, clones, measures;
static size_t allocated;
static void *test_calloc(size_t count,size_t size)
{
    if(fail_after==0)return NULL;
    if(fail_after>0)--fail_after;
    ++allocations;allocated+=count*size;return calloc(count,size);
}
#define calloc test_calloc
#define note_encode actual_encode
#define note_clone actual_clone
#define note_measure actual_measure
bool actual_measure(const note_t *n,note_size_t *size);
#include "../../modules/notes/notes_model.c"
#undef calloc
#undef note_encode
#undef note_clone
#undef note_measure
bool note_encode(const note_t *n,note_buffer_t *b){++encodes;return actual_encode(n,b);}
bool note_clone(note_t *out,const note_t *n){++clones;return actual_clone(out,n);}
bool note_measure(const note_t *n,note_size_t *size){++measures;return actual_measure(n,size);}
#define HISTORY_COUNT 12
#define RT_WAITING_FOREVER -1
#define RT_EOK 0
typedef void epd_service_t;
static bool epd_service_sleep_acquire(epd_service_t *);
static bool epd_service_reclaim_hold(epd_service_t *owner) { return owner != NULL; }
static void epd_service_reclaim_release(epd_service_t *owner) { (void)owner; }
static bool epd_service_mark_dirty(epd_service_t *owner) { return owner != NULL; }
#include "../unified_operation/cleanup_ports.h"
#include "app_work.inc"
typedef bool (*notes_edit_fn)(note_t *,void *);
static struct {
    int mutex,store;
    void *owner;
    app_work_t work;
    note_t document;
    note_buffer_t undo[HISTORY_COUNT],redo[HISTORY_COUNT];
    unsigned undos,redos;
    bool flushing,stopping,save_failed;
    uint32_t deadline,edited_at,size_id,size_revision,edited_block;
    note_size_t edit_size;
    struct {bool ready,busy,dirty,recording,online;uint32_t document;} view;
} runtime,*state=&runtime;
static uint32_t clock_ms=2000;
static int rt_mutex_take(int mutex,int timeout){(void)mutex;(void)timeout;return 0;}
static void lock(void){}
static void unlock(void){}
static uint32_t now(void){return clock_ms;}
static int system_time_get_utc(void){return 1234;}
static void changed(void){}
static void wake_io(void){}
static bool epd_service_sleep_acquire(void *owner){(void)owner;return true;}
static bool notes_store_valid(const int *store){(void)store;return true;}
#include "edit_actual.inc"
static bool style_edit(note_t *n,void *arg){(void)arg;return note_style(n,1,0,3,NOTE_ITALIC);}
static bool insert_edit(note_t *n,void *arg){(void)arg;return note_insert(n,0,NOTE_TASK,"task")!=NULL;}
static void verify_size(void)
{
    note_size_t size;note_buffer_t data={0};note_t decoded={0};
    CHECK(actual_measure(&state->document,&size));CHECK(actual_encode(&state->document,&data));
    CHECK(size.encoded==data.size);CHECK(size.text==note_text_bytes(&state->document));
    CHECK(note_decode(&decoded,data.data,data.size));
    if(state->size_revision==state->document.revision){CHECK(size.text==state->edit_size.text);CHECK(size.encoded==state->edit_size.encoded);}
    note_buffer_clear(&data);note_clear(&decoded);
}
static void history_reset(void){history_clear(state->undo,&state->undos);history_clear(state->redo,&state->redos);state->edited_block=0;}
static void span_reference(void)
{
    const char *samples[]={"中文abcdef", "中🙂文xy", "", "abc", "🙂文", "中文abzdef", "z中文abcdef", "中文abc"};
    for(unsigned sample=0;sample<8;++sample)for(unsigned mask=0;mask<64;++mask){
        note_t note={0};note_init(&note,1,1);CHECK(note_insert(&note,0,NOTE_TEXT,"中文abcdef"));
        note_block_t *block=note.blocks;
        for(unsigned i=0;i<6;++i)if(mask&(1u<<i))CHECK(note_style(&note,1,6+i,7+i,i&1?NOTE_BOLD:NOTE_ITALIC));
        CHECK(note_style(&note,1,0,6,NOTE_BOLD));
        const char *text=samples[sample];size_t old=strlen(block->text),size=strlen(text),a=0,suffix=0;
        while(a<old&&a<size&&text[a]==block->text[a])++a;
        while(a&&(!boundary(text,a)||!boundary(block->text,a)))--a;
        while(suffix<old-a&&suffix<size-a&&block->text[old-suffix-1]==text[size-suffix-1])++suffix;
        while(suffix&&(!boundary(text,size-suffix)||!boundary(block->text,old-suffix)))--suffix;
        note_span_t expected[32];size_t count=0;
        for(size_t p=0;p<size;){size_t z=p+1;while(z<size&&!boundary(text,z))++z;
            size_t origin=p<a?p:p>=size-suffix?old-(size-p):a?a-1:a;
            CHECK(append_span(expected,&count,p,z,style_at(block,origin)));p=z;}
        note_size_t total;note_text_edit_t edit={0};CHECK(actual_measure(&note,&total));
        CHECK(note_edit_prepare(block,text,total,&edit));CHECK(edit.span_count==count);
        CHECK(!count||!memcmp(edit.spans,expected,count*sizeof(*expected)));
        note_edit_apply(block,&edit);CHECK(actual_measure(&note,&total));CHECK(total.encoded==edit.size.encoded);note_clear(&note);
    }
}
int main(void)
{
    state->owner=state;
    span_reference();
    note_t mirror={0};note_init(&state->document,1,1);state->view.ready=true;
    CHECK(note_insert(&state->document,0,NOTE_TEXT,"中文abcdef"));
    CHECK(note_style(&state->document,1,0,6,NOTE_BOLD));
    for(unsigned i=0;i<700;++i)CHECK(note_insert(&state->document,state->document.count,NOTE_TEXT,"unaffected text"));
    CHECK(actual_clone(&mirror,&state->document));
    char *untouched=state->document.blocks[500].text,*display_untouched=mirror.blocks[500].text;
    CHECK(notes_edit_text(1,"中文a",&mirror));CHECK(state->undos==1);
    encodes=clones=measures=allocations=0;allocated=0;
    CHECK(notes_edit_text(1,"中文ab",&mirror));
    CHECK(!encodes&&!clones&&!measures);CHECK(allocations==4&&allocated<1024);
    CHECK(untouched==state->document.blocks[500].text&&display_untouched==mirror.blocks[500].text);
    CHECK(mirror.revision==state->document.revision);CHECK(!strcmp(mirror.blocks[0].text,"中文ab"));
    CHECK(state->document.blocks[0].span_count==1&&state->document.blocks[0].spans[0].end==6);
    CHECK(!notes_edit_text(1,"中\xff",&mirror));verify_size();
    CHECK(notes_undo(false));CHECK(!strcmp(state->document.blocks[0].text,"中文abcdef"));
    CHECK(notes_undo(true));CHECK(!strcmp(state->document.blocks[0].text,"中文ab"));
    CHECK(notes_mutate(style_edit,NULL,false));CHECK(notes_mutate(insert_edit,NULL,false));
    CHECK(notes_edit_text(1,"中🙂文xyz",&mirror));verify_size();
    CHECK(notes_undo(false));CHECK(notes_undo(false));CHECK(state->document.count==701);CHECK(notes_undo(false));
    CHECK(notes_edit_text(1,"new branch",&mirror));CHECK(!state->redos);verify_size();
    CHECK(notes_edit_metadata("title","book",&mirror));
    encodes=clones=measures=allocations=0;
    CHECK(notes_edit_metadata("title2",mirror.notebook,&mirror));
    CHECK(!encodes&&!clones&&!measures&&!allocations);CHECK(!strcmp(mirror.title,"title2"));verify_size();
    CHECK(notes_undo(false));CHECK(!state->document.title[0]);
    CHECK(notes_undo(true));CHECK(!strcmp(state->document.title,"title2"));
    history_reset();CHECK(actual_clone(&mirror,&state->document));
    /* Both block copies and the history allocation must succeed before publication. */
    for(long failure=0;failure<6;++failure){
        uint32_t revision=state->document.revision;unsigned undos=state->undos;
        char before[64];strcpy(before,state->document.blocks[0].text);
        fail_after=failure;bool ok=notes_edit_text(1,"allocation failure",&mirror);fail_after=-1;
        if(!ok){CHECK(state->document.revision==revision&&mirror.revision==revision);CHECK(!strcmp(state->document.blocks[0].text,before));CHECK(state->undos==undos);}
        else {CHECK(notes_undo(false));CHECK(actual_clone(&mirror,&state->document));history_reset();}
    }
    history_reset();note_clear(&state->document);note_clear(&mirror);note_init(&state->document,2,1);state->size_revision=0;
    char *large=malloc(NOTE_TEXT_LIMIT+1);memset(large,'x',NOTE_TEXT_LIMIT);large[NOTE_TEXT_LIMIT]=0;
    CHECK(note_insert(&state->document,0,NOTE_TEXT,""));
    CHECK(note_insert(&state->document,1,NOTE_TRANSCRIPT,""));state->document.blocks[1].original=note_string(large,NOTE_TEXT_LIMIT-1);
    CHECK(notes_edit_text(1,"a",NULL));CHECK(!notes_edit_text(1,"ab",NULL));verify_size();
    CHECK(notes_edit_text(1,"",NULL));CHECK(notes_edit_text(1,"z",NULL));verify_size();
    history_reset();note_clear(&state->document);free(large);
    /* Encoded capacity includes marks and spans as well as text. */
    note_init(&state->document,3,1);CHECK(note_insert(&state->document,0,NOTE_TEXT,"a"));
    note_size_t size={1,4u*NOTE_TEXT_LIMIT};note_text_edit_t edit={0};
    CHECK(!note_edit_prepare(state->document.blocks,"ab",size,&edit));
    CHECK(note_edit_prepare(state->document.blocks,"",size,&edit));CHECK(edit.size.encoded==4u*NOTE_TEXT_LIMIT-1);note_edit_clear(&edit);
    note_clear(&state->document);
    note_init(&state->document,4,1);CHECK(note_insert(&state->document,0,NOTE_TEXT,"x"));
    char text[NOTE_SPAN_LIMIT+1];memset(text,'x',NOTE_SPAN_LIMIT);text[NOTE_SPAN_LIMIT]=0;
    for(unsigned i=0;i<38;++i){
        note_block_t *block=note_insert(&state->document,state->document.count,NOTE_TEXT,text);CHECK(block);
        block->span_count=NOTE_SPAN_LIMIT;block->spans=note_alloc(NOTE_SPAN_LIMIT*sizeof(*block->spans));CHECK(block->spans);
        for(unsigned j=0;j<NOTE_SPAN_LIMIT;++j)block->spans[j]=(note_span_t){j,j+1,1+(j&1)};
    }
    CHECK(actual_measure(&state->document,&size));size_t padding=4u*NOTE_TEXT_LIMIT-size.encoded;
    state->document.blocks[1].original=note_alloc(padding+1);CHECK(state->document.blocks[1].original);
    memset(state->document.blocks[1].original,'x',padding);state->size_revision=0;
    CHECK(!notes_edit_text(1,"xx",NULL));CHECK(!notes_edit_metadata("x","",NULL));
    CHECK(notes_edit_text(1,"",NULL));CHECK(notes_edit_text(1,"z",NULL));verify_size();
    CHECK(state->edit_size.encoded==4u*NOTE_TEXT_LIMIT);
    history_reset();note_clear(&state->document);
    puts("notes edit: incremental allocations, 512 UTF-8/style comparisons, history, failures and capacity passed");return 0;
}
