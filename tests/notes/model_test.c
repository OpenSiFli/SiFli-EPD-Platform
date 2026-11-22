#include "notes_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static void styles(void)
{
    note_t n; note_init(&n, 1, 123);
    note_block_t *p = note_insert(&n, 0, NOTE_TEXT, "今天讨论笔记方案"); CHECK(p);
    uint32_t id = p->id;
    CHECK(note_style(&n, id, 6, 12, NOTE_BOLD));
    CHECK(note_style(&n, id, 9, 18, NOTE_ITALIC));
    CHECK(!note_style(&n, id, 7, 12, NOTE_BOLD));
    CHECK(note_find(&n, id)->span_count == 3);
    CHECK(note_edit(&n, id, "明天讨论笔记方案"));
    CHECK(note_find(&n, id)->spans[0].start == 6);
    CHECK(note_edit(&n, id, "明天先讨论笔记方案"));
    p = note_find(&n, id);
    CHECK(p->spans[0].start == 9 && p->spans[1].style == (NOTE_BOLD | NOTE_ITALIC));
    CHECK(note_edit(&n, id, "明天先讨论方案"));
    note_buffer_t wire = {0}; CHECK(note_encode(&n, &wire));
    note_t copy = {0}; CHECK(note_decode(&copy, wire.data, wire.size));
    CHECK(!strcmp(copy.blocks[0].text, "明天先讨论方案"));
    CHECK(copy.blocks[0].span_count == n.blocks[0].span_count);
    for (size_t i = 0; i < wire.size; ++i) CHECK(!note_decode(&copy, wire.data, i));
    CHECK(!strcmp(copy.blocks[0].text, "明天先讨论方案"));
    note_buffer_clear(&wire); note_clear(&copy); note_clear(&n);
}
static void documents(void)
{
    note_t n; note_init(&n, 20, 55); CHECK(note_empty(&n));
    note_block_t *task = note_insert(&n, 0, NOTE_TASK, "检查清单"); CHECK(task);
    uint32_t task_id = task->id;
    note_block_t *audio = note_insert(&n, 1, NOTE_AUDIO, "会议录音"); CHECK(audio);
    uint32_t audio_id = audio->id; strcpy(audio->attachment, "attachments/00000002.wav");
    CHECK(note_mark(&n, audio_id, 120000, "重点一"));
    note_block_t *text = note_insert(&n, 2, NOTE_TRANSCRIPT, "修改后的转写"); CHECK(text);
    text->source = audio_id; text->original = note_string("原始转写", strlen("原始转写"));
    CHECK(note_move(&n, task_id, 1));
    CHECK(note_find(&n, audio_id)->marks[0].milliseconds == 120000);
    note_t copy = {0}; CHECK(note_clone(&copy, &n));
    CHECK(copy.blocks[2].source == audio_id && !strcmp(copy.blocks[2].original, "原始转写"));
    CHECK(note_remove(&n, task_id)); CHECK(!note_find(&n, task_id)); CHECK(note_find(&copy, task_id));
    note_clear(&n); note_clear(&copy);
}
static void input(void)
{
    CHECK(!note_utf8("\xc0\x80", 2)); CHECK(!note_utf8("\xed\xa0\x80", 3));
    CHECK(!note_utf8("a\0b", 3)); CHECK(note_utf8("中文🙂", strlen("中文🙂")));
    CHECK(!note_attachment_valid("attachments/../../config"));
    CHECK(!note_attachment_valid("/sdcard/config")); CHECK(!note_attachment_valid("attachments/a\\b"));
    CHECK(note_attachment_valid("attachments/001.wav"));
    note_t n; note_init(&n, 1, 0);
    const char *md = "## 标题\n- [x] 已完成\n> 引用\n\n正文";
    CHECK(note_import_text(&n, md, strlen(md), true));
    CHECK(n.count == 4 && n.blocks[1].checked && n.blocks[2].kind == NOTE_QUOTE);
    CHECK(!note_import_text(&n, "\xff", 1, false)); CHECK(n.count == 4);
    note_buffer_t out = {0}; CHECK(note_markdown(&n, &out, false));
    CHECK(strstr((char *)out.data, "- [x] 已完成") != NULL);
    note_buffer_clear(&out); note_clear(&n);
}
static void markdown_roundtrip(void)
{
    note_t n; note_init(&n, 5, 10); strcpy(n.title, "中文标题");
    note_block_t *b = note_insert(&n, 0, NOTE_TEXT, "甲乙丙丁 * _ [ ]"); CHECK(b);
    CHECK(note_style(&n, b->id, 0, 9, NOTE_BOLD)); CHECK(note_style(&n, b->id, 3, 12, NOTE_ITALIC));
    note_buffer_t md = {0}; CHECK(note_markdown(&n, &md, false));
    note_t copy; note_init(&copy, 6, 20); CHECK(note_import_text(&copy, (const char *)md.data, md.size, true));
    CHECK(!strcmp(n.title, copy.title)); CHECK(copy.count == 1); CHECK(!strcmp(copy.blocks[0].text, n.blocks[0].text));
    CHECK(copy.blocks[0].span_count == n.blocks[0].span_count);
    CHECK(!memcmp(copy.blocks[0].spans, n.blocks[0].spans, n.blocks[0].span_count * sizeof(note_span_t)));
    note_buffer_clear(&md); note_clear(&n); note_clear(&copy);
}
static void style_ranges(void)
{
    note_t n;note_init(&n,9,0);char text[193];unsigned expected[64]={0};
    for(unsigned i=0;i<64;++i)memcpy(text+3*i,"字",3);text[192]=0;
    note_block_t *b=note_insert(&n,0,NOTE_TEXT,text);CHECK(b);uint32_t id=b->id;
    for(unsigned i=0;i<64;i+=2){CHECK(note_style(&n,id,3*i,3*i+3,NOTE_ITALIC));expected[i]=NOTE_ITALIC;}
    CHECK(note_style(&n,id,0,192,NOTE_BOLD));for(unsigned i=0;i<64;++i)expected[i]|=NOTE_BOLD;
    uint32_t seed=87211;
    for(unsigned attempt=0;attempt<1000;++attempt){
        seed=seed*1664525u+1013904223u;unsigned start=seed%64;
        seed=seed*1664525u+1013904223u;unsigned end=start+1+seed%(64-start),flag=(seed&256)?NOTE_BOLD:NOTE_ITALIC;
        bool remove=true;for(unsigned i=start;i<end;++i)if(!(expected[i]&flag))remove=false;
        CHECK(note_style(&n,id,start*3,end*3,flag));for(unsigned i=start;i<end;++i)expected[i]=remove?expected[i]&~flag:expected[i]|flag;
        b=note_find(&n,id);
        for(unsigned i=0;i<64;++i){unsigned actual=0;for(size_t s=0;s<b->span_count;++s)if(b->spans[s].start<=i*3&&b->spans[s].end>i*3)actual=b->spans[s].style;CHECK(actual==expected[i]);}
    }
    note_clear(&n);
}
int main(void)
{ styles(); documents(); input(); markdown_roundtrip(); style_ranges(); puts("notes model: UTF-8 editing, styles, persistence and recovery passed"); return 0; }
