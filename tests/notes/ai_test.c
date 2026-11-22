#include "notes_ai.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
static const char *reply;
static unsigned requests,profiles;
static epd_ai_finish_t finish;
static epd_ai_error_t failure;
static bool complete,cancel_after,large,nontxt_default;
static char input[8192];
epd_ai_error_t epd_ai_profile_list(epd_service_t *owner,epd_ai_profile_t *out,size_t capacity,size_t *count)
{
    (void)owner;CHECK(capacity>=2);++profiles;memset(out,0,2*sizeof(*out));*count=2;
    for(unsigned i=0;i<2;++i){out[i].enabled=true;out[i].model_count=1;out[i].models[0].capabilities=EPD_AI_CAP_TEXT;strcpy(out[i].models[0].id,"text");}
    strcpy(out[0].id,"first");strcpy(out[1].id,"preferred");out[1].is_default=true;
    if(nontxt_default)out[1].models[0].capabilities=0;
    return EPD_AI_OK;
}
epd_ai_error_t epd_ai_request_measure(epd_service_t *owner,const epd_ai_request_t *request,size_t reserve,size_t *size)
{(void)owner;(void)reserve;*size=request->messages[0].text_size;return large?EPD_AI_LIMIT:EPD_AI_OK;}
epd_ai_error_t epd_ai_perform(epd_service_t *owner,const epd_ai_request_t *request,epd_ai_cancel_t *cancel,epd_ai_event_cb_t cb,void *user,epd_ai_result_t *result)
{
    (void)owner;(void)cb;(void)user;++requests;CHECK(request->message_count==1);
    CHECK(!strcmp(request->profile_id,nontxt_default?"first":"preferred"));
    snprintf(input,sizeof(input),"%s",request->messages[0].text);
    result->complete=complete;result->finish=finish;
    result->text=note_string(reply,strlen(reply));result->text_size=strlen(reply);
    if(cancel_after)cancel->cancelled=1;
    return failure;
}
void epd_ai_result_clear(epd_ai_result_t *result){note_free(result->text);}
const char *epd_ai_error_name(epd_ai_error_t error){(void)error;return "test";}
static epd_ai_error_t run(const note_t *n,notes_ai_mode_t mode,note_t *out)
{epd_ai_cancel_t cancel={0};return notes_ai_run(NULL,n,mode,&cancel,out);}
int main(void)
{
    note_t source={0},result={0};note_init(&source,42,123);strcpy(source.title,"会议");strcpy(source.notebook,"工作");
    note_block_t *b=note_insert(&source,0,NOTE_TEXT,"今天开会");uint32_t first=b->id;
    CHECK(note_style(&source,first,0,6,NOTE_BOLD));
    b=note_insert(&source,1,NOTE_TASK,"明确下一步");b->checked=true;
    note_insert(&source,2,NOTE_AUDIO,"录音");
    complete=true;finish=EPD_AI_FINISH_STOP;
    reply="{\"blocks\":[{\"id\":1,\"runs\":[{\"style\":1,\"text\":\"今天下午\"},{\"style\":0,\"text\":\"开会讨论\"}]},{\"id\":2,\"runs\":[{\"style\":0,\"text\":\"明确下一步安排\"}]}]}";
    CHECK(run(&source,AI_CLEAR,&result)==EPD_AI_OK);CHECK(requests==1&&profiles==1);
    CHECK(strstr(input,"今天")&&strstr(input,"明确下一步")&&strstr(input,"会议"));
    CHECK(result.count==2&&result.blocks[0].spans[0].end==strlen("今天下午"));
    note_t adopted={0};CHECK(note_clone(&adopted,&source));CHECK(notes_ai_apply(&adopted,&result));
    CHECK(!strcmp(adopted.blocks[0].text,"今天下午开会讨论"));CHECK(adopted.blocks[1].kind==NOTE_TASK&&adopted.blocks[1].checked);CHECK(adopted.blocks[2].kind==NOTE_AUDIO);
    note_buffer_t saved={0};CHECK(notes_ai_pack(&source,&result,AI_CLEAR,&saved));
    note_t original={0},restored={0};notes_ai_mode_t mode;
    CHECK(notes_ai_unpack(saved.data,saved.size,&original,&restored,&mode));
    CHECK(!strcmp(original.blocks[0].text,"今天开会"));CHECK(restored.blocks[0].spans[0].end==12);
    for(size_t i=0;i<saved.size;++i)CHECK(!notes_ai_unpack(saved.data,i,&original,&restored,&mode));
    saved.data[30]^=1;CHECK(!notes_ai_unpack(saved.data,saved.size,&original,&restored,&mode));
    note_buffer_clear(&saved);note_clear(&original);note_clear(&restored);note_clear(&adopted);note_clear(&result);
    const char *valid=reply;
    const char *invalid[]={"", "   ", "{\"blocks\":[]}","{\"blocks\":[],\"blocks\":[]}",
        "{\"blocks\":[{\"id\":1,\"runs\":[{\"style\":0,\"text\":\"wrong style\"}]}]}",
        "{\"blocks\":[{\"id\":99,\"runs\":[]}]}","```json\n{}\n```", "{\"blocks\":[]} trailing"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i){reply=invalid[i];CHECK(run(&source,AI_CLEAR,&result)!=EPD_AI_OK);CHECK(!result.count);}
    reply=valid;finish=EPD_AI_FINISH_REFUSAL;CHECK(run(&source,AI_CLEAR,&result)==EPD_AI_PROTOCOL);
    finish=EPD_AI_FINISH_LENGTH;CHECK(run(&source,AI_CLEAR,&result)==EPD_AI_INCOMPLETE);
    finish=EPD_AI_FINISH_STOP;complete=false;CHECK(run(&source,AI_CLEAR,&result)==EPD_AI_INCOMPLETE);complete=true;
    cancel_after=true;CHECK(run(&source,AI_CLEAR,&result)==EPD_AI_CANCELLED);cancel_after=false;
    large=true;unsigned previous=requests;CHECK(run(&source,AI_CLEAR,&result)==EPD_AI_LIMIT);CHECK(requests==previous);large=false;
    failure=EPD_AI_RATE_LIMIT;previous=requests;CHECK(run(&source,AI_CLEAR,&result)==failure);CHECK(requests==previous+1);failure=EPD_AI_OK;
    reply="{\"blocks\":[]}";CHECK(run(&source,AI_TASKS,&result)==EPD_AI_OK&&!result.count);CHECK(run(&source,AI_SUMMARY,&result)==EPD_AI_PROTOCOL);
    reply="{\"blocks\":[{\"text\":\"归纳全文\"}]}";nontxt_default=true;CHECK(run(&source,AI_SUMMARY,&result)==EPD_AI_OK);note_clear(&result);
    reply="{\"blocks\":[{\"text\":\"a\\u0000b\"}]}";CHECK(run(&source,AI_SUMMARY,&result)==EPD_AI_PROTOCOL);
    note_clear(&source);puts("notes AI: whole note, validation, formatting, snapshots, cancellation, capacity and no retry passed");return 0;
}
