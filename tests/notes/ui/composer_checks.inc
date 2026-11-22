static size_t body_glyph_queries;
static bool (*body_glyph_descriptor)(const lv_font_t *,lv_font_glyph_dsc_t *,uint32_t,uint32_t);
static bool counted_body_glyph(const lv_font_t *font,lv_font_glyph_dsc_t *d,uint32_t c,uint32_t next)
{++body_glyph_queries;return body_glyph_descriptor(font,d,c,next);}

static bool edit_fixture(notes_compose_event_t event, uint32_t id, const char *value, void *context)
{
    if(event != COMPOSE_EDIT) return false;
    note_t copy = {0}, *note = context;
    if(!note_clone(&copy, note)) return false;
    if(!note_edit(&copy, id, value)) { note_clear(&copy); return false; }
    note_clear(note);
    *note = copy;
    return true;
}

static void check_full_layout(notes_composer_t *editor)
{
    size_t count = editor->count;
    text_line_t *lines = malloc(count * sizeof(*lines));
    memcpy(lines, editor->lines, count * sizeof(*lines));
    int height = lv_obj_get_height(editor->canvas);
    layout(editor);
    assert(editor->count == count);
    for(size_t i = 0; i < count; ++i) {
        text_line_t *a = lines + i, *b = editor->lines + i;
        assert(a->block == b->block && a->begin == b->begin && a->end == b->end);
        assert(a->x == b->x && a->y == b->y && a->width == b->width);
        assert(a->height == b->height && a->insertion == b->insertion);
    }
    assert(lv_obj_get_height(editor->canvas) == height);
    free(lines);
}

static void check_incremental_pixels(lv_obj_t *parent)
{
    lv_obj_update_layout(parent);lv_refr_now(NULL);
    uint8_t *saved=malloc(sizeof(frame));memcpy(saved,frame,sizeof(frame));
    lv_obj_invalidate(parent);lv_refr_now(NULL);
    static unsigned step;int left=W,right=-1,top=H,bottom=-1;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(memcmp(saved+(y*W+x)*4,frame+(y*W+x)*4,3)){
        left=LV_MIN(left,x);right=LV_MAX(right,x);top=LV_MIN(top,y);bottom=LV_MAX(bottom,y);
    }
    if(right>=0)fprintf(stderr,"incremental pixel mismatch step=%u bounds=%d,%d-%d,%d\n",step,left,top,right,bottom);
    ++step;assert(right<0);free(saved);
}

static void check_composer_edits(lv_display_t *display,void *buffer)
{
    body_glyph_descriptor=fonts[0]->get_glyph_dsc;fonts[0]->get_glyph_dsc=counted_body_glyph;
    lv_display_set_buffers(display,buffer,NULL,W*40*4,LV_DISPLAY_RENDER_MODE_PARTIAL);
    epd_obj_init(lv_screen_active());lv_obj_set_style_bg_opa(lv_screen_active(),LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(lv_screen_active(),lv_color_white(),0);
    note_t note = {0}; note_init(&note, 17, 0);
    note_insert(&note, 0, NOTE_HEADING, "输入响应测试");
    char *body = calloc(24001, 1);
    for(unsigned i = 0; i < 300; ++i) strcat(body, "中文段落 with styled text and wrapping across the editor.\n");
    uint32_t id = note_insert(&note, 1, NOTE_TEXT, body)->id;
    note_style(&note, id, 0, 12, NOTE_BOLD);
    note_insert(&note, 2, NOTE_QUOTE, "后续段落的位置应该保持正确。");
    note_insert(&note, 3, NOTE_IMAGE, "图片");
    note_insert(&note, 4, NOTE_TASK, "待办事项");
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    epd_obj_init(parent); lv_obj_set_size(parent, W, H);
    lv_obj_set_style_bg_opa(parent,LV_OPA_COVER,0);lv_obj_set_style_bg_color(parent,lv_color_white(),0);
    notes_composer_t *editor = notes_composer_create(parent, &note, true, 500, 440, edit_fixture, &note);
    lv_obj_update_layout(parent);
    notes_caret_t caret = {.block = id, .cursor = (uint32_t)strlen(body), .scroll = -1};
    caret.start = caret.end = caret.cursor;
    notes_composer_caret(editor, &caret, true);
    check_incremental_pixels(parent);
    composer_font_queries = 0;
    body_glyph_queries=0;
    lv_textarea_add_text(editor->input, "\n");
    printf("composer append newline: %zu font queries for %zu bytes\n", composer_font_queries, strlen(body));
    printf("composer append newline: %zu body glyph queries including input buffer\n",body_glyph_queries);
    assert(composer_font_queries<100);
    assert(body_glyph_queries<100);
    check_incremental_pixels(parent);
    check_full_layout(editor);

    const char *inserts[] = {"新行\n", "abc", "\n\n", "。"};
    for(unsigned i = 0; i < 24; ++i) {
        note_block_t *b = note_find(&note, id);
        size_t at = (i * 937u) % (strlen(b->text) + 1);
        while(at && ((unsigned char)b->text[at] & 0xc0) == 0x80) --at;
        caret = (notes_caret_t){.block = id, .cursor = (uint32_t)at, .start = (uint32_t)at, .end = (uint32_t)at, .scroll = -1};
        notes_composer_caret(editor, &caret, true);
        check_incremental_pixels(parent);
        lv_textarea_add_text(editor->input, inserts[i % 4]);
        check_incremental_pixels(parent);
        check_full_layout(editor);
        lv_textarea_delete_char(editor->input);
        check_incremental_pixels(parent);
        check_full_layout(editor);
    }
    caret = (notes_caret_t){.block = id, .cursor = 12, .start = 0, .end = 12, .scroll = -1};
    notes_composer_caret(editor, &caret, true);
    lv_textarea_add_text(editor->input, "替换\n文字");
    check_incremental_pixels(parent);
    check_full_layout(editor);
    caret=(notes_caret_t){.block=id,.cursor=15,.start=15,.end=15,.scroll=-1};
    notes_composer_caret(editor,&caret,true);lv_obj_invalidate(parent);lv_refr_now(NULL);
    lv_textarea_set_text(editor->input,note_find(&note,id)->text);
    check_incremental_pixels(parent);check_full_layout(editor);
    lv_textarea_set_text(editor->input,"第一行\n第二行\n第三行");check_incremental_pixels(parent);check_full_layout(editor);
    caret=(notes_caret_t){.block=id,.cursor=3,.start=3,.end=3,.scroll=-1};
    notes_composer_caret(editor,&caret,true);lv_obj_invalidate(parent);lv_refr_now(NULL);
    lv_textarea_set_text(editor->input,note_find(&note,id)->text);
    check_incremental_pixels(parent);check_full_layout(editor);
    lv_obj_delete(parent); note_clear(&note); free(body);
    lv_display_set_buffers(display,buffer,NULL,W*H*4,LV_DISPLAY_RENDER_MODE_FULL);
    fonts[0]->get_glyph_dsc=body_glyph_descriptor;
    puts("composer edit layout checks passed");
}
