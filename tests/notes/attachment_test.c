#include "notes_store.h"
#include "storage_file.h"
#include "host_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int main(void)
{
    for(int failure=0;failure<4;++failure){
        host_reset();host_new_card();host_new_card();
        notes_store_t store={0};CHECK(notes_store_open(&store)==NOTES_OK);
        note_t note={0};note_init(&note,1,1);note_block_t *block=note_insert(&note,0,NOTE_IMAGE,"image");CHECK(block);
        unsigned char data[20000];for(size_t i=0;i<sizeof(data);++i)data[i]=(unsigned char)(i*37);
        CHECK(storage_file_replace("/sdcard/source.PNG",data,sizeof(data)));
        char destination[NOTES_PATH_SIZE];
        CHECK(notes_note_path(destination,sizeof(destination),1,"attachments/import-00000001.png"));
        CHECK(storage_file_replace(destination,"old",3));
        strcpy(block->attachment,"attachments/previous.png");uint32_t revision=note.revision;
        host_watch_io();
        if(failure==1)host_io.fail_import=1;
        if(failure==2)host_io.swap_on_import=1;
        if(failure==3)host_new_card();
        notes_error_t result=notes_store_attachment(&store,&note,1,"/sdcard/source.PNG");
        size_t size=0;const unsigned char *saved=host_contents(destination,&size);CHECK(saved);
        CHECK(!host_io.imports&&!host_io.import_lock_depth);
        CHECK(host_io.session_imports==(failure==3?0u:1u));
        if(failure!=3){CHECK(host_io.expected_volume==STORAGE_SD);CHECK(host_io.expected_session==store.session);}
        CHECK(host_io.session_rejections==(failure==2?1u:0u));
        if(!failure){
            CHECK(result==NOTES_OK&&size==sizeof(data)&&!memcmp(saved,data,size));
            CHECK(note.revision==revision+1&&!strcmp(block->attachment,"attachments/import-00000001.png"));
        }else{
            CHECK(result==(failure>=2?NOTES_NO_CARD:NOTES_IO));
            CHECK(size==3&&!memcmp(saved,"old",3)&&note.revision==revision);
            CHECK(!strcmp(block->attachment,"attachments/previous.png"));
        }
        note_clear(&note);
    }
    host_reset();puts("notes attachment: passes SD session without outer lock; entry-time swaps reject copying and preserve metadata");return 0;
}
