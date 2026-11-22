static lv_obj_t *notebook_tab(const char *text)
{
    for(unsigned i=0;i<lv_obj_get_child_count(page->body);++i){
        lv_obj_t *container=lv_obj_get_child(page->body,i);
        for(unsigned j=0;j<lv_obj_get_child_count(container);++j){
            lv_obj_t *button=lv_obj_get_child(container,j);
            if(!lv_obj_get_child_count(button))continue;
            lv_obj_t *caption=lv_obj_get_child(button,0);
            if(lv_obj_check_type(caption,&lv_label_class)&&!strcmp(lv_label_get_text(caption),text))return button;
        }
    }
    return NULL;
}

static void select_notebook_tab(const char *text,uint32_t value)
{
    lv_obj_t *tab=notebook_tab(text);assert(tab);
    lv_obj_scroll_to_view(tab,LV_ANIM_OFF);lv_obj_update_layout(page->screen);
    lv_area_t area;lv_obj_get_coords(tab,&area);
    assert(area.x1>=120&&area.x2<W);
    page->action=0;click_at((area.x1+area.x2)/2,(area.y1+area.y2)/2);
    assert(page->action==A_BOOK_SELECT&&page->value==value);poll(NULL);lv_obj_update_layout(page->screen);
    tab=notebook_tab(text);assert(tab);
    assert(lv_color_eq(lv_obj_get_style_bg_color(tab,0),lv_color_hex(0x222222)));
    lv_obj_t *tabs=lv_obj_get_parent(tab);unsigned selected=0;
    for(unsigned i=0;i<lv_obj_get_child_count(tabs);++i)
        selected+=lv_color_eq(lv_obj_get_style_bg_color(lv_obj_get_child(tabs,i),0),lv_color_hex(0x222222));
    assert(selected==1&&notebook_tab("未分类")&&notebook_tab("全部"));
}

static void check_notebook_switching(const char *directory)
{
    assert(page->total==6);
    select_notebook_tab("未分类",UINT32_MAX);
    assert(!strcmp(page->notebook,"\1")&&page->total==1);
    capture(directory,"25-unfiled-notebook");
    assert(has_label(page->body,"值得再读的一句话")&&!has_label(page->body,"产品讨论 · 十月例会"));

    do_action(A_BOOKS,0);rebuild();do_action(A_BOOK_NEW,0);rebuild();
    lv_textarea_set_text(page->input,"新笔记本");do_action(A_SAVE,0);rebuild();
    assert(page->mode==LIST&&fixture_book_count==5&&command_kind==N_BOOK_NEW);
    page->offset=6;page->selecting=true;page->selected_count=1;page->selected[0]=6;
    page->batch_pending=6;page->batch_move=page->batch_export=true;
    select_notebook_tab("新笔记本",5);
    assert(!strcmp(page->notebook,"新笔记本")&&!page->total&&!page->offset&&!page->selecting&&!page->selected_count);
    assert(!page->batch_pending&&!page->batch_move&&!page->batch_export);
    assert(has_label(page->body,"这里还没有笔记")&&!has_label(page->body,"值得再读的一句话"));
    assert(!has_label(page->body,"产品讨论 · 十月例会"));capture(directory,"26-empty-notebook-grid");
    lv_area_t active;lv_obj_get_coords(notebook_tab("新笔记本"),&active);assert(active.x1>=120&&active.x2<W);
    page->grid=false;rebuild();assert(!page->total&&has_label(page->body,"这里还没有笔记"));capture(directory,"27-empty-notebook-list");
    do_action(A_NEW,0);assert(command_kind==N_NEW&&!strcmp(command_text,"新笔记本"));page->pending_document=0;
    do_action(A_HOME_REC,0);assert(command_kind==N_NEW&&command_arg==2&&!strcmp(command_text,"新笔记本"));page->pending_document=0;page->record_after_open=false;
    do_action(A_NEW,1);assert(command_arg==1&&!command_text[0]);page->pending_document=0;page->new_task=false;

    select_notebook_tab("工作",1);assert(page->total==2);
    assert(has_label(page->body,"产品讨论 · 十月例会")&&has_label(page->body,"采访记录 · 设计师"));
    assert(!has_label(page->body,"值得再读的一句话")&&!has_label(page->body,"这里还没有笔记"));capture(directory,"28-populated-notebook");
    do_action(A_BOOK_SELECT,999);assert(!strcmp(page->notebook,"工作")&&page->total==2);
    select_notebook_tab("未分类",UINT32_MAX);assert(page->total==1&&has_label(page->body,"值得再读的一句话"));
    do_action(A_NEW,0);assert(command_kind==N_NEW&&!command_text[0]);page->pending_document=0;
    do_action(A_BOOKS,0);rebuild();do_action(A_BOOK_SELECT,5);rebuild();assert(!page->total&&has_label(page->body,"这里还没有笔记"));
    select_notebook_tab("全部",0);assert(page->total==6);
    page->feedback[0]=0;page->feedback_until=0;notice_text("");page->edit_on_open=false;page->grid=true;rebuild();
    fprintf(stdout,"notebook creation, selection, empty lists and new-note destination checks passed\n");
}

static void check_notebook_background_updates(void)
{
    fixture_book_count=4;rebuild();assert(!notebook_tab("新笔记本"));
    page->preferences_loaded=true;page->revision=page->status.revision;
    reported_status=page->status;report_status=true;
    fixture_book_count=5;++reported_status.revision;++reported_status.catalog_revision;
    assert(!reported_status.busy&&!page->status.busy);
    poll(NULL);lv_obj_update_layout(page->screen);
    assert(notebook_tab("新笔记本"));
    select_notebook_tab("新笔记本",5);assert(page->total==0);
    fixture_moved_note=true;++reported_status.revision;++reported_status.catalog_revision;
    poll(NULL);lv_obj_update_layout(page->screen);
    assert(page->total==1&&has_label(page->body,"值得再读的一句话"));
    fixture_moved_note=false;++reported_status.revision;++reported_status.catalog_revision;
    poll(NULL);lv_obj_update_layout(page->screen);
    assert(!page->total&&has_label(page->body,"这里还没有笔记"));
    unsigned calls=rows_calls;++reported_status.revision;reported_status.recording_ms+=5000;
    poll(NULL);assert(rows_calls==calls);
    do_action(A_BOOK_RENAME,0);rebuild();lv_textarea_set_text(page->input,"改名后的笔记本");do_action(A_SAVE,0);
    assert(page->book_rename_pending&&!strcmp(page->notebook,"新笔记本"));
    strcpy(fixture_custom_name,"改名后的笔记本");++reported_status.revision;++reported_status.catalog_revision;
    poll(NULL);lv_obj_update_layout(page->screen);
    assert(!page->book_rename_pending&&!strcmp(page->notebook,fixture_custom_name)&&notebook_tab(fixture_custom_name));
    do_action(A_BOOK_RENAME,0);rebuild();lv_textarea_set_text(page->input,"保存失败的名称");do_action(A_SAVE,0);
    reported_status.error=NOTES_IO;++reported_status.revision;poll(NULL);
    assert(!page->book_rename_pending&&!strcmp(page->notebook,fixture_custom_name));
    reported_status.error=NOTES_OK;++reported_status.revision;poll(NULL);
    do_action(A_BOOK_RENAME,0);rebuild();lv_textarea_set_text(page->input,"再次改名");do_action(A_SAVE,0);rebuild();
    assert(page->book_rename_pending);
    strcpy(fixture_custom_name,"再次改名");++reported_status.revision;++reported_status.catalog_revision;
    select_notebook_tab("全部",0);
    assert(!page->book_rename_pending&&!page->notebook[0]);
    strcpy(fixture_custom_name,"新笔记本");
    report_status=false;select_notebook_tab("全部",0);
    fprintf(stdout,"background notebook changes refresh without an observed busy transition\n");
}
