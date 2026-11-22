#include "notes_store.h"
#include "host_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} }while(0)
static bool capture_row(void *user,const notes_row_t *row){*(notes_row_t*)user=*row;return true;}
static void recovery(void)
{
    host_reset();notes_store_t store={0};note_t n={0},changed={0};note_buffer_t saved={0};notes_row_t row={0};
    CHECK(!notes_store_open(&store));CHECK(!notes_store_new(&store,&n));strcpy(n.title,"Before");
    CHECK(note_insert(&n,0,NOTE_TASK,"Task"));CHECK(!notes_store_save(&store,&n));CHECK(note_encode(&n,&saved));
    CHECK(!notes_store_scan(&store,capture_row,&row));CHECK(row.tasks==1&&!strcmp(row.title,"Before"));
    host_new_card();CHECK(!notes_store_resume(&store,n.id,&saved));CHECK(notes_store_valid(&store));
    CHECK(note_clone(&changed,&n));strcpy(changed.title,"Changed elsewhere");++changed.revision;CHECK(!notes_store_save(&store,&changed));
    host_new_card();CHECK(notes_store_resume(&store,n.id,&saved)==NOTES_CONFLICT);CHECK(!notes_store_valid(&store));
    CHECK(!notes_store_open(&store));char path[512];notes_note_path(path,sizeof(path),n.id,"summary.bin");
    CHECK(!notes_store_write(&store,path,"bad",3));CHECK(!notes_store_scan(&store,capture_row,&row));CHECK(!strcmp(row.title,changed.title));
    note_t pending={0};CHECK(!notes_store_new(&store,&pending));notes_store_abandon(&store,pending.id);
    notes_note_path(path,sizeof(path),pending.id,"");CHECK(!host_exists(path));
    notes_store_abandon(&store,n.id);CHECK(!notes_store_load(&store,n.id,&pending));CHECK(!strcmp(pending.title,changed.title));
    note_clear(&pending);note_clear(&n);note_clear(&changed);note_buffer_clear(&saved);host_reset();
}
static void scan(void)
{
    host_reset();notes_store_t store;CHECK(!notes_store_open(&store));note_t source={0},copy={0};CHECK(!notes_store_new(&store,&source));
    strcpy(source.title,"会议记录");strcpy(source.notebook,"工作");
    note_block_t *text=note_insert(&source,0,NOTE_TEXT,"带格式的中文正文");CHECK(text);CHECK(note_style(&source,text->id,0,9,NOTE_BOLD));
    CHECK(note_insert(&source,1,NOTE_TASK,"检查清单"));
    note_block_t *audio=note_insert(&source,2,NOTE_AUDIO,"录音");CHECK(audio);audio->milliseconds=130000;strcpy(audio->attachment,"attachments/record.wav");uint32_t audio_id=audio->id;
    CHECK(note_mark(&source,audio_id,30000,"重点"));
    note_block_t *transcript=note_insert(&source,3,NOTE_TRANSCRIPT,"校正后的转写");CHECK(transcript);transcript->source=audio_id;transcript->milliseconds=120000;transcript->original=note_string("原始转写",strlen("原始转写"));
    char path[512],archive[512];CHECK(notes_note_path(path,sizeof(path),source.id,"attachments/record.wav"));
    unsigned char payload[4097];for(size_t i=0;i<sizeof(payload);++i)payload[i]=(unsigned char)(i*29);
    CHECK(!notes_store_write(&store,path,payload,sizeof(payload)));CHECK(!notes_store_save(&store,&source));
    CHECK(!notes_store_load(&store,source.id,&copy));CHECK(copy.count==4&&copy.blocks[0].span_count==1);note_clear(&copy);
    CHECK(!notes_store_export(&store,&source,2,archive,sizeof(archive)));size_t bytes;const unsigned char *wire=host_contents(archive,&bytes);CHECK(wire&&bytes>sizeof(payload));
    unsigned char *saved=malloc(bytes);CHECK(saved);memcpy(saved,wire,bytes);
    CHECK(!notes_store_import(&store,archive,&copy));CHECK(copy.id!=source.id&&copy.count==4&&!strcmp(copy.title,source.title));
    CHECK(copy.blocks[0].spans[0].style==NOTE_BOLD&&copy.blocks[2].marks[0].milliseconds==30000);
    CHECK(!strcmp(copy.blocks[3].original,"原始转写")&&copy.blocks[3].source==audio_id&&copy.blocks[3].milliseconds==120000);
    CHECK(notes_note_path(path,sizeof(path),copy.id,copy.blocks[2].attachment));size_t actual;const unsigned char *pcm=host_contents(path,&actual);CHECK(actual==sizeof(payload)&&!memcmp(pcm,payload,actual));
    for(size_t n=0;n<bytes;n+=97){CHECK(!notes_store_write(&store,"/sdcard/broken.zip",saved,n));CHECK(notes_store_import(&store,"/sdcard/broken.zip",&copy)!=NOTES_OK);CHECK(copy.count==4);}
    saved[100]^=0x80;CHECK(!notes_store_write(&store,"/sdcard/broken.zip",saved,bytes));CHECK(notes_store_import(&store,"/sdcard/broken.zip",&copy)!=NOTES_OK);saved[100]^=0x80;
    for(size_t i=0;i+12<bytes;++i)if(!memcmp(saved+i,"attachments/",12))memcpy(saved+i,"../outside//",12);
    CHECK(!notes_store_write(&store,"/sdcard/traversal.zip",saved,bytes));CHECK(notes_store_import(&store,"/sdcard/traversal.zip",&copy)==NOTES_INVALID);
    host_fail_after(100);CHECK(notes_store_save(&store,&source)==NOTES_IO);host_fail_after(-1);
    note_t intact={0};CHECK(!notes_store_load(&store,source.id,&intact));CHECK(!strcmp(intact.title,"会议记录"));note_clear(&intact);
    host_fail_after(200);CHECK(notes_store_export(&store,&source,2,archive,sizeof(archive))==NOTES_IO);host_fail_after(-1);
    wire=host_contents(archive,&actual);CHECK(wire&&actual==bytes);
    CHECK(notes_store_purge(&store,copy.id)==NOTES_INVALID);copy.deleted=true;CHECK(!notes_store_save(&store,&copy));CHECK(!notes_store_purge(&store,copy.id));
    host_new_card();CHECK(notes_store_save(&store,&source)==NOTES_NO_CARD);
    free(saved);note_clear(&source);note_clear(&copy);host_reset();
}

static void audio_import(void)
{
    host_reset();notes_store_t store={0};note_t n={0},loaded={0};CHECK(!notes_store_open(&store));
    const char payload[]="audio bytes";CHECK(!notes_store_write(&store,"/sdcard/会议.MP3",payload,sizeof(payload)));
    CHECK(!notes_store_import_audio(&store,"/sdcard/会议.MP3",95400,&n));CHECK(n.count==1&&n.blocks[0].kind==NOTE_AUDIO&&n.blocks[0].milliseconds==95400);
    CHECK(!strcmp(n.title,"会议")&&!strcmp(n.blocks[0].text,"会议")&&note_word_count(&n)==0);
    CHECK(!strncmp(n.blocks[0].attachment,"attachments/import-",19));char path[512];CHECK(notes_note_path(path,sizeof(path),n.id,n.blocks[0].attachment));
    size_t length=0;const unsigned char *data=host_contents(path,&length);CHECK(length==sizeof(payload)&&!memcmp(data,payload,length));
    CHECK(!notes_store_load(&store,n.id,&loaded)&&loaded.count==1&&loaded.blocks[0].milliseconds==95400);
    uint32_t id=n.id;CHECK(notes_store_import_audio(&store,"/sdcard/会议.jpg",100,&n)==NOTES_INVALID&&n.id==id);
    CHECK(notes_store_import_audio(&store,"/sdcard/missing.wav",100,&n)==NOTES_IO&&n.id==id);
    CHECK(notes_store_import(&store,"/sdcard/会议.MP3",&n)==NOTES_INVALID&&n.id==id);
    CHECK(!notes_store_write(&store,"/sdcard/example.Markdown","# Heading\n\nBody",15));CHECK(!notes_store_import(&store,"/sdcard/example.Markdown",&n));
    CHECK(n.count>0);note_clear(&loaded);note_clear(&n);host_reset();
}

int main(void){audio_import();recovery();scan();puts("notes archive: round-trip, CRC, truncation, traversal, full disk, recovery and summary cache passed");return 0;}
