#include "notes_ai.h"
#include "notes_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct {uint32_t argument;} job_t;
typedef struct {bool preview;epd_ai_error_t ai_error;} notes_status_t;
static struct {note_t document,preview,preview_original;notes_store_t store;notes_status_t view;bool cancel,stopping;epd_ai_cancel_t ai_cancel;uint32_t preview_source;notes_ai_mode_t preview_mode;} instance,*state=&instance;
static unsigned cancel_stage,writes,removes;
static bool valid=true,save_failure;
static void lock(void){}static void unlock(void){}static void changed(void){}
static void storage_lock(void){}static void storage_unlock(void){}
static bool stopping(void *unused){(void)unused;return state->cancel||state->stopping;}
static bool document_snapshot(note_t *out){return note_clone(out,&state->document);}
bool notes_store_valid(const notes_store_t *store){(void)store;return valid;}
bool notes_note_path(char *out,size_t capacity,uint32_t id,const char *file){snprintf(out,capacity,"%u/%s",id,file);return true;}
notes_error_t notes_store_write(notes_store_t *store,const char *path,const void *data,size_t size)
{(void)store;(void)path;(void)data;(void)size;++writes;if(cancel_stage==2)state->cancel=true;return save_failure?NOTES_IO:NOTES_OK;}
static int remove_file(const char *path){(void)path;++removes;return 0;}
#define unlink remove_file
bool notes_ai_pack(const note_t *original,const note_t *result,notes_ai_mode_t mode,note_buffer_t *out)
{(void)result;(void)mode;return note_encode(original,out);}
epd_ai_error_t notes_ai_run(epd_service_t *owner,const note_t *source,notes_ai_mode_t mode,epd_ai_cancel_t *cancel,note_t *out)
{(void)owner;(void)mode;(void)cancel;CHECK(note_clone(out,source));if(cancel_stage==1)state->cancel=true;if(cancel_stage==3)note_edit(&state->document,1,"newer edit");return EPD_AI_OK;}
#include "ai_job_actual.inc"
int main(void)
{
    for(cancel_stage=0;cancel_stage<5;++cancel_stage){
        memset(state,0,sizeof(*state));writes=removes=0;save_failure=cancel_stage==4;
        note_init(&state->document,1,10);note_insert(&state->document,0,NOTE_TEXT,"submitted original");
        job_t job={AI_CLEAR};ai_job(NULL,&job);
        if(cancel_stage==1||cancel_stage==2){CHECK(!state->view.preview&&state->view.ai_error==EPD_AI_CANCELLED);CHECK(writes==(cancel_stage==2));if(cancel_stage==2)CHECK(removes==3);}
        else if(save_failure)CHECK(!state->view.preview&&state->view.ai_error==EPD_AI_STORAGE);
        else{CHECK(state->view.preview&&writes==1);CHECK(!strcmp(state->preview_original.blocks[0].text,"submitted original"));if(cancel_stage==3)CHECK(state->document.revision!=state->preview_source);}
        note_clear(&state->document);note_clear(&state->preview);note_clear(&state->preview_original);
    }
    puts("notes AI completion boundaries passed");return 0;
}
