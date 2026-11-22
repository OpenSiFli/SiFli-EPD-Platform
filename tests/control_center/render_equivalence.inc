/* Compare real LVGL RGB565 raster output with the original opaque containers. */
static unsigned candidate_fills, equivalence_checks;
static uint8_t reference_frame[684 * 1216 * 2];
static uint8_t closed_frame[684 * 1216 * 2];
static FILE *pixel_trace;
static bool compare_trace;

static void trace_frame(const char *phase)
{
    if(compare_trace) {
        CHECK(fread(reference_frame,1,sizeof(reference_frame),pixel_trace)==sizeof(reference_frame));
        if(memcmp(reference_frame,frame_pixels,sizeof(reference_frame))) {
            fprintf(stderr,"Baseline/current pixel mismatch: %s scale=%u mode=%u\n",phase,scale,center.notifications);
            CHECK(!"baseline/current pixel trace");
        }
    }
    else CHECK(fwrite(frame_pixels,1,sizeof(reference_frame),pixel_trace)==sizeof(reference_frame));
    ++equivalence_checks;
}

static void load_render_font(const char *path)
{
    FILE *file=fopen(path,"rb");CHECK(file);
    CHECK(fseek(file,0,SEEK_END)==0);long size=ftell(file);CHECK(size>0);
    rewind(file);ttf_size=(size_t)size;ttf_data=malloc(ttf_size);CHECK(ttf_data);
    CHECK(fread(ttf_data,1,ttf_size,file)==ttf_size);CHECK(fclose(file)==0);
    const int sizes[]={36,28,20};
    for(unsigned i=0;i<3;++i) {
        role_fonts[i]=lv_tiny_ttf_create_data_ex(ttf_data,ttf_size,sizes[i],LV_FONT_KERNING_NONE,0);
        CHECK(role_fonts[i]);
    }
}

static unsigned background_candidates(center_t *view,lv_obj_t **objects)
{
    unsigned n=0;
    objects[n++]=view->tabs[0];objects[n++]=view->tabs[1];
    if(view->notifications)objects[n++]=view->dots;
    else {
        objects[n++]=view->light;objects[n++]=view->volume;
        objects[n++]=view->wifi;objects[n++]=view->bluetooth;
        objects[n++]=view->lock;objects[n++]=view->settings;
        objects[n++]=view->media_panel;
    }
    return n;
}

static void count_candidate_fills(lv_event_t *event)
{
    if(lv_draw_task_get_type(lv_event_get_draw_task(event))==LV_DRAW_TASK_TYPE_FILL)
        ++candidate_fills;
}

static void candidate_backgrounds(lv_opa_t opacity)
{
    lv_obj_t *objects[20];unsigned count=background_candidates(selected,objects);
    for(unsigned i=0;i<count;++i)lv_obj_set_style_bg_opa(objects[i],opacity,0);
}

static void check_retained_fills(center_t *view)
{
    CHECK(lv_obj_get_style_bg_opa(view->root,0)==LV_OPA_COVER);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(view->root,0),lv_color_white()));
    CHECK(lv_obj_get_style_bg_opa(view->handle,0)==LV_OPA_COVER);
    for(unsigned i=0;i<NOTICE_ROWS;++i)
        if(view->rows[i].panel)CHECK(lv_obj_get_style_bg_opa(view->rows[i].panel,0)==LV_OPA_COVER);
    lv_obj_t *containers[]={view->light,view->volume,view->dots};
    for(unsigned i=0;i<3;++i) {
        lv_obj_t *container=containers[i];if(!container)continue;
        if(i<2)container=lv_obj_get_child(container,0);
        for(unsigned j=0;j<lv_obj_get_child_count(container);++j)
            CHECK(lv_obj_get_style_bg_opa(lv_obj_get_child(container,j),0)==LV_OPA_COVER);
    }
}

static void equal_frame(const uint8_t *expected,const char *phase)
{
    for(unsigned i=0;i<sizeof(reference_frame);++i) {
        if(expected[i]!=frame_pixels[i]) {
            fprintf(stderr,"Pixel mismatch: %s scale=%u mode=%s x=%u y=%u byte=%u old=%u new=%u\n",
                    phase,scale,center.notifications?"notices":"controls",
                    (i/2)%684,(i/2)/684,i%2,expected[i],frame_pixels[i]);
            CHECK(!"background pixel equivalence");
        }
    }
    ++equivalence_checks;
}

static void compare_backgrounds(lv_display_t *display,const char *phase)
{
    if(pixel_trace) {
        lv_refr_now(display);trace_frame(phase);return;
    }
    candidate_fills=0;render(display);
    CHECK(candidate_fills==0);
    memcpy(reference_frame,frame_pixels,sizeof(reference_frame));
    candidate_backgrounds(LV_OPA_COVER);candidate_fills=0;
    render(display);CHECK(candidate_fills>0);
    equal_frame(reference_frame,phase);
    candidate_backgrounds(LV_OPA_TRANSP);candidate_fills=0;
    render(display);CHECK(candidate_fills==0);
    equal_frame(reference_frame,phase);
    check_retained_fills(selected);
}

static void background_equivalence_test(lv_display_t *display)
{
    locked=false;storage_busy=false;idle=true;ui_control_center_close();
    pixel_bytes=2;
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_pixels,NULL,684*40*2,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_obj_t *screen=lv_screen_active();
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x194080),0);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
    lv_obj_t *under=lv_label_create(screen);
    lv_label_set_text(under,"Underlying page must return after close");
    lv_obj_set_pos(under,20,200);
    for(unsigned v=0;v<2;++v) {
        lv_obj_t *objects[20];unsigned count=background_candidates(&views[v],objects);
        for(unsigned i=0;i<count;++i) {
            if(!pixel_trace)CHECK(lv_obj_get_style_bg_opa(objects[i],0)==LV_OPA_TRANSP);
            lv_obj_add_flag(objects[i],LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
            lv_obj_add_event_cb(objects[i],count_candidate_fills,LV_EVENT_DRAW_TASK_ADDED,NULL);
        }
        check_retained_fills(&views[v]);
    }
    unsigned cases=0;
    for(unsigned zoom=0;zoom<2;++zoom) {
        scale=zoom?120:100;++scale_revision;
        lv_font_t *old_roles[3];memcpy(old_roles,role_fonts,sizeof(old_roles));
        if(ttf_data) {
            const int sizes[]={36,28,20};
            for(unsigned i=0;i<3;++i) {
                role_fonts[i]=lv_tiny_ttf_create_data_ex(ttf_data,ttf_size,epd_ui_px(sizes[i]),LV_FONT_KERNING_NONE,0);
                CHECK(role_fonts[i]);
            }
        }
        ui_control_center_process();
        if(ttf_data)for(unsigned i=0;i<3;++i)lv_tiny_ttf_destroy(old_roles[i]);
        for(unsigned mode=0;mode<2;++mode)
        for(unsigned populated=0;populated<2;++populated)
        for(unsigned media=0;media<2;++media)
        for(unsigned disabled=0;disabled<2;++disabled) {
            populate(populated?12:0);
            if(populated)for(unsigned i=0;i<notice_count;++i) {
                strcpy(catalog[i].name,"Long application name for clipped title");
                strcpy(notices[i].text,"Notification text crossing the row width and wrapping or clipping at the edge.");
            }
            media_present=media!=0;++media_generation;
            memset(&published_media,0,sizeof(published_media));
            strcpy(published_media.app_id,"app0");
            strcpy(published_media.artist,"Artist with a long display name");
            strcpy(published_media.title,"Track title reaching the panel boundary");
            published_media.playing=!disabled;
            published_media.capabilities=disabled?0:EPD_MEDIA_CAN_PLAY|EPD_MEDIA_CAN_PAUSE|EPD_MEDIA_CAN_PREVIOUS|EPD_MEDIA_CAN_NEXT;
            sound_ready=light_ready=radios_enabled=!disabled;
            sound_max=16;sound_percent=disabled?0:75;light_percent=disabled?0:63;
            sound_save=disabled?EPD_SOUND_SAVE_FAILED:EPD_SOUND_SAVE_OK;
            render(display);memcpy(closed_frame,frame_pixels,sizeof(closed_frame));
            ui_control_center_open(mode!=0);ui_control_center_process();
            if(!mode)CHECK(lv_obj_has_state(center.volume,LV_STATE_DISABLED)==(disabled!=0));
            compare_backgrounds(display,"open/state transition");
            if(populated) {
                lv_obj_remove_flag(center.rows[0].remove,LV_OBJ_FLAG_HIDDEN);
                compare_backgrounds(display,"visible dismiss button");
                if(mode) {
                    center.page_delta=1;ui_control_center_process();CHECK(center.page==1);
                    compare_backgrounds(display,"next notification page");
                }
            }
            ui_control_center_close();lv_refr_now(display);
            if(pixel_trace)trace_frame("close");else equal_frame(closed_frame,"close");
            ui_control_center_open(mode!=0);ui_control_center_process();
            compare_backgrounds(display,"reopen");
            ui_control_center_close();lv_refr_now(display);
            if(pixel_trace)trace_frame("close after reopen");else equal_frame(closed_frame,"close after reopen");
            ++cases;
        }
    }
    CHECK(!filesystem_opens);
    printf("PASS backgrounds: %u cases, %u RGB565 frames, 100/120%%, both panels, empty/populated notices, media/no media, enabled/disabled, paging/dismiss, close/reopen; %s; %s\n",
           cases,equivalence_checks,pixel_trace?(compare_trace?"baseline/current byte-identical":"baseline trace recorded"):"opaque/transparent byte-identical, candidate fill tasks removed",
           ttf_data?"MiSans TTF":"built-in font");
}
