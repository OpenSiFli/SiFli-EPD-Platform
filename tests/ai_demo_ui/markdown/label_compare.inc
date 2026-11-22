static lv_obj_t *find_label(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_label_class)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *label = find_label(lv_obj_get_child(o, i));
        if (label) return label;
    }
    return NULL;
}
static void compare_one(const char *text, int width, unsigned id)
{
    chat_markdown_t *v = chat_markdown_create(screen);
    CHECK(update(v, text, width));
    CHECK(!strcmp(chat_markdown_source(v), text));
    if (AI_MARKDOWN_PLAIN_LABEL) {
        lv_obj_t *label = find_label(v->root);
        CHECK(label && !group(v->root));
        const char *rendered = lv_label_get_text(label);
        size_t available = strlen(rendered), n = 0;
        const md_node_t *block = &v->document.nodes[1];
        for (uint32_t i = 0; i < block->run_count; ++i) {
            const md_run_t *run = &v->document.runs[block->first_run+i];
            CHECK(run->text.length <= available - n);
            CHECK(!memcmp(rendered + n, v->document.text + run->text.offset, run->text.length));
            n += run->text.length;
        }
        CHECK(available == n);
    } else paragraph_bytes(v);
    int height = lv_obj_get_height(v->root);
    printf("GEOMETRY %u %d %d\n", id, width, height);
    int positions[] = {0,height/2,height-800};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_y(v->root,-positions[i]);
        printf("PIXELS %u %u %llu\n",id,i,(unsigned long long)pixel_hash());
    }
    size_t before = glyph_calls;
    uint64_t hash = pixel_hash();
    printf("WARM %u %zu %llu\n",id,glyph_calls-before,(unsigned long long)hash);
    destroy(v);
}
static void label_compare(const char *path)
{
    CHECK(LV_LABEL_LONG_TXT_HINT == 1);
    char *fixture = load(path);
    compare_one(fixture,627,0);
    compare_one(fixture,381,1);
    free(fixture);
    char plain[8192] = "";
    for (unsigned i=0;i<80;++i)
        strcat(plain,"Mixed English words wrap around 中文，标点。 ASCII abc123! Another sentence.\n");
    compare_one(plain,627,2);
    compare_one(plain,381,3);
    if (AI_MARKDOWN_PLAIN_LABEL) {
        chat_markdown_t *v = chat_markdown_create(screen);
        const char *styles[]={" **bold**"," [link](https://one.test)"," `code`",
            "\n\n| a | b |\n| --- | --- |\n| one | **two** |"};
        char mixed[9000];
        for (unsigned i=0;i<4;++i) {
            snprintf(mixed,sizeof(mixed),"%s%s",plain,styles[i]);
            CHECK(update(v,mixed,627) && group(v->root));
            if (i<3) CHECK(!find_label(v->root));
        }
        destroy(v);
        puts("PASS bold, link, code and table span path");
#if AI_MARKDOWN_PLAIN_LABEL
        v=chat_markdown_create(screen);
        md_doc_t doc={0};
        CHECK(md_document_parse(plain, strlen(plain), &allocator, NULL, &doc) == MD_OK);
        size_t retained=live[1];
        char *owned = NULL;
        app_fail=1;
        lv_obj_t *box = build_block(v->root,&doc,1,627,&owned);
        CHECK(box && group(box) && !find_label(box) && !owned);
        CHECK(!app_fail && live[1]==retained);
        md_document_free(&doc);
        destroy(v);
        puts("PASS label PSRAM allocation failure leaves no allocation behind");
        v = chat_markdown_create(screen);
        CHECK(update(v,plain,627) && find_label(v->root));
        const lv_obj_t *prior = chat_markdown_block(v,0);
        snprintf(mixed,sizeof(mixed),"%s\n\n**second paragraph**",plain);
        CHECK(update(v,mixed,627) && chat_markdown_block(v,0)==prior);
        snprintf(mixed,sizeof(mixed),"%s suffix\n\n**changed**",plain);
        retained=live[1];
        font_fail=2;
        CHECK(!update(v,mixed,627) && !font_fail);
        CHECK(live[1]==retained && chat_markdown_block(v,0)==prior);
        CHECK(update(v,mixed,627) && chat_markdown_block(v,0)!=prior);
        destroy(v);
        puts("PASS reuse, rollback after staged label, replacement and destruction");

#endif

    }
}
