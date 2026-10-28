static void design_points_free(lv_event_t *e){note_free(lv_event_get_user_data(e));}
static lv_obj_t *box(lv_obj_t *p,int x,int y,int w,int h,int radius,unsigned gray,int border){
    lv_obj_t *o=lv_obj_create(p);epd_app_object_init(o);lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(gray),0);lv_obj_set_style_bg_opa(o,255,0);lv_obj_set_style_radius(o,radius,0);
    lv_obj_set_style_border_width(o,border,0);lv_obj_set_style_border_color(o,lv_color_hex(0xc8c8c8),0);return o;
}
static lv_obj_t *txt(lv_obj_t *p,const char *s,int x,int y,int w,const lv_font_t *f,unsigned color){
    lv_obj_t *o=label(p,s,x,y,w,false);lv_obj_set_style_text_font(o,f,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_obj_set_style_text_line_space(o,8,0);return o;
}
static void path(lv_obj_t *p,int x,int y,const int *coords,size_t n,int stroke,unsigned color){
    lv_point_precise_t *points=note_alloc(n*sizeof(*points));if(!points)return;for(size_t i=0;i<n;i++){points[i].x=coords[2*i];points[i].y=coords[2*i+1];}
    lv_obj_t *o=lv_line_create(p);lv_obj_add_event_cb(o,design_points_free,LV_EVENT_DELETE,points);epd_app_object_init(o);lv_line_set_points(o,points,(uint32_t)n);lv_obj_set_pos(o,x,y);lv_obj_set_style_line_width(o,stroke,0);lv_obj_set_style_line_rounded(o,true,0);lv_obj_set_style_line_color(o,lv_color_hex(color),0);
}
static void dot(lv_obj_t *p,int x,int y,int size,unsigned color){box(p,x,y,size,size,size/2,color,0);}
static void icon(lv_obj_t *p,const char *type,int x,int y,unsigned color){
    if(!strcmp(type,"flag")){int a[]={8,37,8,3,33,3,27,13,33,23,8,23};path(p,x,y,a,6,3,color);return;}
    if(!strcmp(type,"more")){for(int i=0;i<3;i++)dot(p,x+17,y+5+i*12,5,color);return;}
    if(!strcmp(type,"plus")){int a[]={20,5,20,35},b[]={5,20,35,20};path(p,x,y,a,2,3,color);path(p,x,y,b,2,3,color);return;}
    if(!strcmp(type,"back")){int a[]={27,5,12,20,27,35};path(p,x,y,a,3,3,color);return;}
    if(!strcmp(type,"close")){int a[]={8,8,32,32},b[]={32,8,8,32};path(p,x,y,a,2,3,color);path(p,x,y,b,2,3,color);return;}
    if(!strcmp(type,"down")){int a[]={7,14,20,27,33,14};path(p,x,y,a,3,3,color);return;}
    if(!strcmp(type,"right")){int a[]={13,7,26,20,13,33};path(p,x,y,a,3,3,color);return;}
    if(!strcmp(type,"check")){int a[]={7,21,16,30,34,10};path(p,x,y,a,3,3,color);return;}
    if(!strcmp(type,"task")){lv_obj_t *o=box(p,x+4,y+4,32,32,7,0xffffff,2);lv_obj_set_style_border_color(o,lv_color_hex(color),0);int a[]={6,15,12,22,25,9};path(o,0,0,a,3,3,color);return;}
    if(!strcmp(type,"folder")){int a[]={3,10,16,10,21,15,37,15,37,34,3,34,3,10};path(p,x,y,a,7,3,color);int b[]={3,10,3,5,15,5,20,10,34,10,37,15};path(p,x,y,b,6,3,color);return;}
    if(!strcmp(type,"search")){lv_obj_t *o=box(p,x+4,y+3,25,25,13,0xe8e8e8,2);lv_obj_set_style_border_color(o,lv_color_hex(color),0);int a[]={26,26,37,37};path(p,x,y,a,2,3,color);return;}
    if(!strcmp(type,"mic")){lv_obj_t *o=box(p,x+13,y+2,14,24,7,0xffffff,2);lv_obj_set_style_border_color(o,lv_color_hex(color),0);int a[]={7,18,7,23,10,29,15,33,25,33,30,29,33,23,33,18},b[]={20,33,20,40};path(p,x,y,a,8,3,color);path(p,x,y,b,2,3,color);return;}
    if(!strcmp(type,"clip")){int a[]={15,27,29,13,29,8,25,5,20,5,5,20,3,26,5,33,10,37,17,37,35,19,37,12,34,5,28,1,21,1,16,4,5,15};path(p,x,y,a,17,3,color);return;}
    if(!strcmp(type,"ai")){int a[]={20,2,25,15,38,20,25,25,20,38,15,25,2,20,15,15,20,2};path(p,x,y,a,9,2,color);return;}
    if(!strcmp(type,"text")){int a[]={4,8,36,8},b[]={20,8,20,36};path(p,x,y,a,2,3,color);path(p,x,y,b,2,3,color);return;}
    if(!strcmp(type,"undo")||!strcmp(type,"redo")){int a[]={10,10,4,18,11,25},b[]={4,18,22,18,29,21,32,28,32,36};if(!strcmp(type,"redo")){for(int i=0;i<6;i+=2)a[i]=40-a[i];for(int i=0;i<10;i+=2)b[i]=40-b[i];}path(p,x,y,a,3,3,color);path(p,x,y,b,5,3,color);return;}
    if(!strcmp(type,"share")){int a[]={6,21,6,36,34,36,34,21},b[]={20,27,20,3},c[]={11,12,20,3,29,12};path(p,x,y,a,4,3,color);path(p,x,y,b,2,3,color);path(p,x,y,c,3,3,color);return;}
    if(!strcmp(type,"pause")){box(p,x+8,y+5,7,30,2,color,0);box(p,x+25,y+5,7,30,2,color,0);return;}
    if(!strcmp(type,"play")){int a[]={10,5,33,20,10,35,10,5};path(p,x,y,a,4,3,color);return;}
    if(!strcmp(type,"stop")){box(p,x+8,y+8,24,24,4,color,0);return;}
    if(!strcmp(type,"sort")){int a[]={12,3,12,36,6,30},b[]={28,37,28,4,34,10};path(p,x,y,a,3,3,color);path(p,x,y,b,3,3,color);return;}
    if(!strcmp(type,"grid")){for(int r=0;r<2;r++)for(int c=0;c<2;c++){lv_obj_t *o=box(p,x+4+c*19,y+4+r*19,13,13,3,0xffffff,2);lv_obj_set_style_border_color(o,lv_color_hex(color),0);}return;}
    if(!strcmp(type,"trash")){int a[]={7,12,33,12},b[]={11,12,13,36,28,36,30,12},c[]={15,12,15,5,26,5,26,12};path(p,x,y,a,2,3,color);path(p,x,y,b,4,3,color);path(p,x,y,c,4,3,color);return;}
}
static const lv_font_t *body_font(void){return epd_app_font_role(EPD_FONT_BODY);}
static const lv_font_t *small_font(void){return epd_app_font_role(EPD_FONT_SMALL);}
static const lv_font_t *caption_font(void){return epd_app_font_role(EPD_FONT_CAPTION);}
static const lv_font_t *heading_font(void){return page->heading?page->heading:epd_app_font_role(EPD_FONT_TITLE);}
static void clickable(lv_obj_t *o,action_t a,uint32_t value){lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICK_FOCUSABLE);lv_obj_add_event_cb(o,event,LV_EVENT_CLICKED,bind(a,value));}
static lv_obj_t *icon_button(lv_obj_t *p,const char *name,int x,int y,action_t a,uint32_t value){
    lv_obj_t *b=epd_app_button(p,x,y,epd_ui_touch(64),epd_ui_touch(64),NULL,event,bind(a,value));
    lv_obj_set_style_border_width(b,0,0);lv_obj_set_style_bg_opa(b,0,0);icon(b,name,(epd_ui_touch(64)-40)/2,(epd_ui_touch(64)-40)/2,0x222222);return b;
}
static lv_obj_t *pill(lv_obj_t *p,const char *s,int x,int y,int w,bool chosen,action_t a,uint32_t v){
    lv_obj_t *b=button(p,s,x,y,w,epd_ui_touch(56),a,v);lv_obj_set_style_radius(b,epd_ui_px(28),0);lv_obj_set_style_bg_opa(b,255,0);lv_obj_set_style_bg_color(b,lv_color_hex(chosen?0x222222:0xffffff),0);lv_obj_set_style_border_color(b,lv_color_hex(0xc8c8c8),0);lv_obj_set_style_border_width(b,chosen?0:1,0);lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(chosen?0xffffff:0x333333),0);return b;
}
static void design_background(unsigned color){box(page->body,0,epd_ui_px(64),page->width,page->height-epd_ui_px(64),0,color,0);}
static void design_date(uint32_t stamp,char *out,size_t capacity){struct tm t;if(system_time_local_from_utc(stamp,&t))snprintf(out,capacity,tr("%d月%d日 %02d:%02d","%02d/%02d %02d:%02d"),t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min);else snprintf(out,capacity,"--/-- --:--");}
static void design_topbar(bool edit){
    icon_button(page->body,"back",10,70,A_BACK,0);
    if(edit){icon_button(page->body,"undo",page->width-251,70,A_UNDO,0);icon_button(page->body,"redo",page->width-173,70,A_REDO,0);icon_button(page->body,"check",page->width-89,70,A_SAVE,0);}
    else{icon_button(page->body,"share",page->width-164,70,A_EXPORT_MENU,0);icon_button(page->body,"more",page->width-82,70,A_MENU,1);}
}
static lv_obj_t *design_scroll(lv_obj_t *p,int x,int y,int width,int height){lv_obj_t *o=box(p,x,y,width,height,0,0xffffff,0);lv_obj_set_style_bg_opa(o,0,0);lv_obj_add_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);lv_obj_set_scroll_dir(o,LV_DIR_VER);lv_obj_set_scrollbar_mode(o,LV_SCROLLBAR_MODE_OFF);return o;}
static lv_obj_t *design_overlay(int x,int y,int w,int h){
    lv_obj_t *shade=box(page->body,0,epd_ui_px(64),page->width,page->height-epd_ui_px(64),0,0x000000,0);lv_obj_set_style_bg_opa(shade,LV_OPA_30,0);clickable(shade,A_BACK,0);
    lv_obj_t *panel=box(page->body,x,y,w,h,epd_ui_px(24),0xffffff,1);lv_obj_add_flag(panel,LV_OBJ_FLAG_CLICKABLE);return panel;
}
static void design_search(int y,bool tasks){lv_obj_t *b=box(page->body,24,y,page->width-48,66,33,0xe4e4e4,0);clickable(b,A_SEARCH,0);icon(b,"search",18,13,0x666666);txt(b,page->search[0]?page->search:tasks?tr("搜索待办","Search tasks"):tr("搜索笔记","Search notes"),72,17,page->width-150,body_font(),0x666666);}
static void design_notebook_tabs(void){
    lv_obj_t *folder=box(page->body,24,258,80,56,28,0xffffff,0);clickable(folder,A_BOOKS,0);icon(folder,"folder",20,7,0x222222);
    lv_obj_t *tabs=design_scroll(page->body,120,254,page->width-144,64),*active=NULL;
    lv_obj_set_scroll_dir(tabs,LV_DIR_HOR);
    const char *fixed[]={tr("全部","All"),tr("未分类","Unfiled")};
    int x=0;
    for(unsigned i=0;i<2;++i){
        bool chosen=i?!strcmp(page->notebook,"\1"):!page->notebook[0];
        lv_obj_t *tab=pill(tabs,fixed[i],x,4,106,chosen,A_BOOK_SELECT,i?UINT32_MAX:0);
        if(chosen)active=tab;x+=118;
    }
    char books[8][96];size_t offset=0,total;
    while((total=notes_books(books,offset,8))>offset){
        size_t count=LV_MIN(total-offset,8);
        for(size_t i=0;i<count;++i){
            lv_point_t size;lv_text_get_size(&size,books[i],small_font(),0,0,174,LV_TEXT_FLAG_NONE);
            int width=LV_CLAMP(106,size.x+36,210);bool chosen=!strcmp(page->notebook,books[i]);
            lv_obj_t *tab=pill(tabs,books[i],x,4,width,chosen,A_BOOK_SELECT,(uint32_t)(offset+i+1));
            if(chosen)active=tab;x+=width+12;
        }
        offset+=count;
    }
    lv_obj_update_layout(tabs);if(active)lv_obj_scroll_to_view(active,LV_ANIM_OFF);
}
static void design_list(void){
    design_background(0xf1f1f1);txt(page->body,page->filter==2?tr("最近删除","Recently deleted"):tr("笔记","Notes"),28,94,page->width-200,heading_font(),0x111111);
    icon_button(page->body,page->selecting?"check":"task",page->width-146,87,page->selecting?A_SELECT:A_FILTER,page->selecting?0:1);icon_button(page->body,"more",page->width-73,87,A_MENU,0);design_search(174,false);
    design_notebook_tabs();
    lv_obj_t *list=design_scroll(page->body,24,334,page->width-48,page->height-366);
    notes_row_t rows[6];size_t per=page->grid?6:5;page->total=notes_rows(rows,page->offset,per,page->search,page->notebook,page->filter,page->sort);
    if(page->offset>=page->total&&page->offset){page->offset=0;page->total=notes_rows(rows,0,per,page->search,page->notebook,page->filter,page->sort);}
    size_t count=page->total>page->offset?page->total-page->offset:0;if(count>per)count=per;
    int ys[2]={0,0},cw=page->grid?(page->width-72)/2:page->width-48;
    for(size_t i=0;i<count;i++){
        int col=page->grid?(ys[0]<=ys[1]?0:1):0;lv_point_t title_size,summary_size;
        lv_text_get_size(&title_size,rows[i].title,epd_app_font_style(EPD_FONT_BODY,1),0,8,cw-44,LV_TEXT_FLAG_NONE);
        lv_text_get_size(&summary_size,rows[i].summary,small_font(),0,8,cw-44,LV_TEXT_FLAG_NONE);
        int th=LV_MIN(title_size.y,body_font()->line_height*2+8),sh=LV_MIN(summary_size.y,small_font()->line_height*(page->grid?5:2)+32);
        int h=LV_MAX(page->grid?244:170,20+th+13+sh+64);h=LV_MIN(h,360);
        lv_obj_t *card=box(list,col*(cw+24),ys[col],cw,h,22,0xffffff,0);clickable(card,page->selecting?A_SELECT:A_OPEN,rows[i].id);
        lv_obj_t *t=txt(card,rows[i].title,22,20,cw-44,epd_app_font_style(EPD_FONT_BODY,1),0x111111);lv_obj_set_height(t,th);lv_label_set_long_mode(t,LV_LABEL_LONG_DOT);
        lv_obj_t *summary=txt(card,rows[i].summary,22,33+th,cw-44,small_font(),0x444444);lv_obj_set_height(summary,h-th-97);lv_label_set_long_mode(summary,LV_LABEL_LONG_DOT);
        char date[80];design_date(rows[i].updated,date,sizeof(date));txt(card,date,22,h-43,cw-44,caption_font(),0x666666);
        if(page->selecting){lv_obj_t *mark=box(card,cw-36,8,28,28,6,selected(rows[i].id)?0x222222:0xffffff,1);if(selected(rows[i].id))txt(mark,"✓",3,0,24,caption_font(),0xffffff);}
        ys[col]+=h+18;
    }
    int bottom=LV_MAX(ys[0],ys[1]);if(!count)txt(list,page->status.error==NOTES_NO_CARD?tr("请插入 TF 卡","Insert a TF card"):tr("这里还没有笔记","No notes yet"),16,42,page->width-100,body_font(),0x666666);
    if(page->offset)pill(list,tr("上一组","Previous"),0,bottom,180,false,A_PREV,0);
    if(page->offset+count<page->total)pill(list,tr("下一组","Next"),page->width-240,bottom,180,false,A_NEXT,0);
    box(list,0,bottom+80,1,100,0,0xf1f1f1,0);
    if(page->selecting){lv_obj_t *b=box(page->body,24,page->height-118,page->width-48,80,32,0xffffff,1);lv_obj_add_flag(b,LV_OBJ_FLAG_CLICKABLE);pill(b,tr("删除","Trash"),12,12,170,false,A_BATCH_DELETE,0);pill(b,tr("移动","Move"),194,12,170,false,A_BATCH_MOVE,0);pill(b,tr("更多","More"),376,12,170,false,A_MENU,0);}
    else{lv_obj_t *bar=box(page->body,(page->width-320)/2,page->height-118,320,80,40,0xffffff,2);lv_obj_add_flag(bar,LV_OBJ_FLAG_CLICKABLE);icon_button(bar,"clip",23,8,A_IMPORT,0);icon_button(bar,"mic",128,8,A_HOME_REC,0);icon_button(bar,"plus",233,8,A_NEW,0);}
}
static void menu_row(lv_obj_t *p,const char *name,const char *sub,const char *ico,int y,int width,action_t action,uint32_t value,bool checked){
    lv_obj_t *b=box(p,12,y,width-24,sub&&*sub?89:72,10,0xffffff,0);clickable(b,action,value);
    int x=ico?68:16;if(ico)icon(b,ico,10,16,0x222222);txt(b,name,x,sub&&*sub?8:18,width-x-70,body_font(),0x222222);
    if(sub&&*sub)txt(b,sub,x,45,width-x-70,small_font(),0x666666);
    if(checked||!(action==A_SORT||action==A_DATE_ORDER||action==A_GRID||action==A_FILTER))icon(b,checked?"check":"right",width-70,20,0x555555);
}
static void design_options(void){
    int w=LV_MIN(378,page->width-48),h=page->mode==MENU?390:page->mode==VIEW_MENU?270:page->mode==SORT_MENU?480:390;
    lv_obj_t *p=design_overlay(page->width-w-18,80,w,h);
    if(page->mode==MENU){
        if(page->selecting){txt(p,tr("导出所选笔记","Export selected"),28,24,w-56,body_font(),0x222222);
            const char *names[]={"TXT","Markdown ZIP",tr("完整笔记 ZIP","Complete note ZIP")};for(unsigned i=0;i<3;i++)menu_row(p,names[i],NULL,NULL,90+i*76,w,A_BATCH_EXPORT,i,false);return;}
        menu_row(p,tr("切换视图","View"),page->grid?tr("卡片视图","Cards"):tr("列表视图","List"),"grid",16,w,A_VIEW_MENU,0,false);
        menu_row(p,tr("排序方式","Sort"),(page->sort&3)==1?tr("创建时间","Created"):tr("更新时间","Updated"),"sort",105,w,A_SORT_MENU,0,false);
        menu_row(p,tr("筛选","Filter"),page->filter==3?tr("置顶笔记","Pinned"):tr("全部笔记","All notes"),"folder",194,w,A_FILTER_MENU,0,false);
        menu_row(p,tr("多选笔记","Select notes"),NULL,"task",283,w,A_SELECT,0,false);
    }else if(page->mode==VIEW_MENU){txt(p,tr("切换视图","View"),28,24,w-56,body_font(),0x222222);menu_row(p,tr("列表视图","List"),NULL,NULL,90,w,A_GRID,0,!page->grid);menu_row(p,tr("卡片视图","Cards"),NULL,NULL,164,w,A_GRID,1,page->grid);}
    else if(page->mode==SORT_MENU){txt(p,tr("排序方式","Sort"),28,24,w-56,body_font(),0x222222);menu_row(p,tr("更新时间","Updated"),NULL,NULL,88,w,A_SORT,0,(page->sort&3)==0);menu_row(p,tr("创建时间","Created"),NULL,NULL,160,w,A_SORT,1,(page->sort&3)==1);menu_row(p,tr("标题","Title"),NULL,NULL,232,w,A_SORT,2,(page->sort&3)==2);box(p,24,311,w-48,1,0,0xcccccc,0);menu_row(p,tr("最新的排最前","Newest first"),NULL,NULL,320,w,A_DATE_ORDER,0,!(page->sort&4));menu_row(p,tr("最旧的排最前","Oldest first"),NULL,NULL,392,w,A_DATE_ORDER,1,(page->sort&4)!=0);}
    else{txt(p,tr("筛选","Filter"),28,24,w-56,body_font(),0x222222);menu_row(p,tr("全部笔记","All notes"),NULL,NULL,90,w,A_FILTER,0,page->filter==0);menu_row(p,tr("置顶笔记","Pinned"),NULL,NULL,170,w,A_FILTER,3,page->filter==3);menu_row(p,tr("最近删除","Recently deleted"),NULL,NULL,250,w,A_FILTER,2,page->filter==2);}
}
static void design_notebooks(void){
    int w=LV_MIN(570,page->width-60);lv_obj_t *p=design_overlay(18,76,w,page->height-122);icon_button(p,"close",16,12,A_BACK,0);icon_button(p,"plus",w-83,12,A_BOOK_NEW,0);txt(p,tr("笔记本","Notebooks"),28,96,w-56,epd_app_font_role(EPD_FONT_TITLE),0x111111);
    lv_obj_t *list=design_scroll(p,18,152,w-36,page->height-392);char names[8][96];size_t total=notes_books(names,page->offset,8),count=total>page->offset?total-page->offset:0;if(count>8)count=8;page->total=total;
    for(size_t i=0;i<count+3;i++){
        const char *name=i==0?tr("全部内容","All notes"):i==1?tr("未分类","Unfiled"):i==count+2?tr("最近删除","Recently deleted"):names[i-2];
        const char *book=i==0||i==count+2?"":i==1?"\1":name;unsigned filter=i==count+2?2:0;size_t n=notes_rows(NULL,0,0,"",book,filter,0);
        lv_obj_t *row=box(list,0,(int)i*98,w-36,82,18,(!strcmp(book,page->notebook)&&page->filter==filter)?0xe8e8e8:0xffffff,0);
        clickable(row,i==count+2?A_FILTER:A_BOOK_SELECT,i==count+2?2:i==0?0:i==1?UINT32_MAX:(uint32_t)(page->offset+i-1));icon(row,i==count+2?"trash":"folder",16,18,0x222222);txt(row,name,82,22,w-200,body_font(),0x222222);
        char num[24];snprintf(num,sizeof(num),"%u",(unsigned)n);txt(row,num,w-100,24,58,small_font(),0x555555);
    }
    int y=(int)(count+3)*98;if(page->offset)pill(list,"<",0,y,100,false,A_PREV,0);if(page->offset+count<total)pill(list,">",w-160,y,100,false,A_NEXT,0);
    if(page->notebook[0]&&page->notebook[0]!='\1'){pill(list,tr("重命名","Rename"),0,y+80,210,false,A_BOOK_RENAME,0);pill(list,tr("删除笔记本","Delete notebook"),228,y+80,260,false,A_BOOK_DELETE,0);}
    pill(p,tr("刷新 TF 卡","Refresh TF card"),28,page->height-212,w-56,false,A_RESCAN,0);
}
static bool design_compose_event(notes_compose_event_t code,uint32_t id,const char *text,void *user){
    (void)user;if(!page)return false;
    if(code==COMPOSE_INSERT){page->action=A_INSERT_AT;page->value=id;return true;}
    if(code==COMPOSE_EDIT){if(!notes_edit_text(id,text,&page->note)){message(tr("文字尚未保存，请稍后重试","Text not saved; please try again"));return false;}page->block=id;return true;}
    if(code==COMPOSE_CHECK){edit_t e={.action=A_CHECK,.id=id};if(!notes_mutate(edit_note,&e,false))return false;notes_document(&page->note);return true;}
    page->block=id;page->action=code==COMPOSE_FOCUS?A_FOCUS:A_BLOCK;page->value=id;return true;
}
static void design_keyboard_layout(void){
    if(page->mode==SEARCH && page->editor_host){int kb=page->keyboard?epd_keyboard_preferred_height(page->keyboard):0;lv_obj_set_height(page->editor_host,LV_MAX(100,page->height-kb-340));return;}
    if(!page->composer||!page->toolbar)return;int kb=page->keyboard?epd_keyboard_preferred_height(page->keyboard):0;
    int bottom=page->height-(kb?kb+20:38);
    if(page->progress){int height=page->status.recording||page->status.playing?92:64;lv_obj_set_y(lv_obj_get_parent(page->progress),bottom-height);bottom-=height+8;}
    lv_obj_set_y(page->toolbar,bottom-76);int height=LV_MAX(100,bottom-100-page->composer_top);lv_obj_set_height(page->editor_host,height);notes_composer_resize(page->composer,height);
}
static void design_note(bool editing){
    design_background(0xffffff);design_topbar(editing);char title[160],date[80],meta[240];note_display_title(&page->note,title,sizeof(title));
    lv_obj_t *t=txt(page->body,title,30,164,page->width-60,heading_font(),0x111111);clickable(t,A_META,0);lv_obj_update_layout(t);
    int title_height=LV_MIN(lv_obj_get_height(t),heading_font()->line_height*2+lv_obj_get_style_text_line_space(t,0));
    lv_obj_set_height(t,title_height);lv_label_set_long_mode(t,LV_LABEL_LONG_DOT);int meta_y=164+title_height+12;
    size_t words=note_word_count(&page->note);
    design_date(page->note.updated,date,sizeof(date));snprintf(meta,sizeof(meta),tr("%s  |  %u 字%s%s","%s  |  %u characters%s%s"),date,(unsigned)words,page->note.notebook[0]?"  |  ":"",page->note.notebook);
    txt(page->body,meta,32,meta_y,page->width-64,small_font(),0x666666);page->composer_top=meta_y+74;
    int reserve=(page->status.recording||page->status.transcribing||page->status.playing)&&editing?230:150;
    int height=LV_MAX(150,page->height-page->composer_top-reserve);
    page->editor_host=box(page->body,32,page->composer_top,page->width-64,height,0,0xffffff,0);
    page->composer=notes_composer_create(page->editor_host,&page->note,editing,page->width-64,height,design_compose_event,NULL);
    if(page->composer){notes_composer_hide_completed(page->composer,page->hide_completed);notes_composer_caret(page->composer,&page->caret,true);if(editing)page->input=notes_composer_input(page->composer);}
    if(editing){page->toolbar=box(page->body,24,page->height-114,page->width-48,76,28,0xffffff,1);lv_obj_add_flag(page->toolbar,LV_OBJ_FLAG_CLICKABLE);const char *ic[]={"ai","mic","task","clip","text"};action_t act[]={A_AI_MENU,A_REC,A_INSERT,A_INSERT_MENU,A_FORMAT};
        int step=(page->width-88)/5;for(int i=0;i<5;i++)icon_button(page->toolbar,ic[i],26+i*step,6,act[i],i==2?NOTE_TASK:0);}
    else if(!page->status.recording&&!page->status.online&&!page->status.playing){lv_obj_t *bar=box(page->body,24,page->height-114,page->width-48,76,32,0xf5f5f5,1);clickable(bar,page->status.preview?A_PREVIEW:A_AI_MENU,0);icon(bar,"ai",22,17,0x222222);txt(bar,page->status.preview?tr("AI 结果待确认","Review AI result"):tr("润色、摘要、提取待办…","Polish, summarize, extract tasks…"),80,22,page->width-150,body_font(),0x555555);}
    if(page->status.recording||page->status.playing){lv_obj_t *bar=box(page->body,24,page->height-130,page->width-48,92,26,0xffffff,1);lv_obj_add_flag(bar,LV_OBJ_FLAG_CLICKABLE);
        if(page->status.recording){dot(bar,24,40,12,0x111111);char duration[80];snprintf(duration,sizeof(duration),page->status.paused?tr("已暂停  %02lu:%02lu","Paused  %02lu:%02lu"):tr("录音中  %02lu:%02lu","Recording  %02lu:%02lu"),(unsigned long)(page->status.recording_ms/60000),(unsigned long)(page->status.recording_ms/1000%60));page->progress=txt(bar,duration,52,29,284,body_font(),0x111111);icon_button(bar,page->status.paused?"play":"pause",page->width-322,14,page->status.paused?A_RESUME:A_PAUSE,0);pill(bar,tr("标记","Mark"),page->width-242,18,92,false,A_MARK,0);icon_button(bar,"stop",page->width-137,14,A_STOP,0);}
        else{char duration[32];snprintf(duration,sizeof(duration),"%02lu:%02lu",(unsigned long)(page->status.playback_ms/60000),(unsigned long)(page->status.playback_ms/1000%60));page->progress=txt(bar,duration,24,29,page->width-210,body_font(),0x222222);icon_button(bar,"pause",page->width-140,14,A_PLAY_STOP,0);}}
    else if(page->status.transcribing){lv_obj_t *bar=box(page->body,page->width-88,page->height-102,64,64,32,0xffffff,1);lv_obj_add_flag(bar,LV_OBJ_FLAG_CLICKABLE);
        page->progress=txt(bar,"",0,0,1,body_font(),0x222222);lv_obj_add_flag(page->progress,LV_OBJ_FLAG_HIDDEN);icon_button(bar,"close",0,0,A_CANCEL_TRANSCRIBE,0);}
    if(editing)design_keyboard_layout();
}
static void design_format(void){
    int h=286;lv_obj_t *p=box(page->body,0,page->height-h,page->width,h,24,0xffffff,1);lv_obj_add_flag(p,LV_OBJ_FLAG_CLICKABLE);txt(p,tr("文字格式","Text format"),28,22,page->width-110,body_font(),0x222222);icon_button(p,"close",page->width-81,6,A_BACK,0);
    icon_button(p,"more",page->width-153,6,A_BLOCK,0);
    note_block_t *b=note_find(&page->note,page->block);int w=(page->width-88)/3;note_kind_t kinds[]={NOTE_TEXT,NOTE_HEADING,NOTE_QUOTE};
    for(int i=0;i<3;i++)pill(p,kind_name(kinds[i]),28+i*(w+16),86,w,b&&b->kind==kinds[i],A_TYPE,kinds[i]);
    int cw=(page->width-104)/4;const char *names[]={"B","I",tr("撤销","Undo"),tr("重做","Redo")};action_t act[]={A_BOLD,A_ITALIC,A_UNDO,A_REDO};
    for(int i=0;i<4;i++){bool on=false;if(i<2&&b){uint32_t a=LV_MIN(page->caret.start,page->caret.end);for(size_t j=0;j<b->span_count;j++)if(b->spans[j].start<=a&&a<b->spans[j].end)on=(b->spans[j].style&(1u<<i))!=0;}
        lv_obj_t *bt=pill(p,names[i],28+i*(cw+16),168,cw,on,act[i],0);lv_obj_set_style_radius(bt,12,0);if(i<2)lv_obj_set_style_text_font(lv_obj_get_child(bt,0),epd_app_font_style(EPD_FONT_BODY,1u<<i),0);}
    if(page->toolbar){int height=LV_MAX(100,page->height-h-118-page->composer_top);lv_obj_set_y(page->toolbar,page->height-h-94);lv_obj_set_height(page->editor_host,height);notes_composer_resize(page->composer,height);}
}
static void design_ai(void){
    int h=620;lv_obj_t *p=design_overlay(14,page->height-h-28,page->width-28,h);txt(p,tr("AI 整理","AI writing"),28,24,page->width-100,epd_app_font_role(EPD_FONT_TITLE),0x111111);icon_button(p,"close",page->width-110,12,A_BACK,0);
    const char *zh[]={"清楚自然","简洁表达","正式表达","仅改错字","生成摘要","提取待办","会议纪要"};const char *en[]={"Clear and natural","Concise","Formal","Correct typos","Summary","Extract tasks","Meeting minutes"};
    int w=(page->width-100)/2;for(unsigned i=0;i<7;i++)pill(p,tr(zh[i],en[i]),24+(i%2)*(w+20),104+(i/2)*94,w,false,A_AI,i);
    lv_obj_t *grant=pill(p,page->ai_authorized?tr("已授权服务","Service authorized"):tr("授权服务","Authorize service"),24,500,w,false,A_GRANT,0);
    if(page->ai_authorized||page->grant_ticket){lv_obj_add_state(grant,LV_STATE_DISABLED);lv_obj_set_style_bg_color(grant,lv_color_hex(0xeeeeee),0);lv_obj_set_style_text_color(lv_obj_get_child(grant,0),lv_color_hex(0x888888),0);}if(page->status.preview)pill(p,tr("查看待采用结果","Review result"),44+w,500,w,false,A_PREVIEW,0);
}
static void design_preview(void){
    if(!notes_preview(&page->preview,&page->ai_mode))note_clear(&page->preview);
    if(!notes_preview_original(&page->original))note_clear(&page->original);
    page->ai_target=page->preview.count?page->preview.blocks[0].source:0;
    for(size_t i=1;i<page->preview.count;i++)if(page->preview.blocks[i].source!=page->ai_target){page->ai_target=0;break;}
    int h=LV_MIN(758,page->height-240),w=page->width-28;lv_obj_t *p=design_overlay(14,page->height-h-28,w,h);icon(p,"ai",26,26,0x222222);txt(p,tr("润色预览","AI preview"),85,24,w-180,epd_app_font_role(EPD_FONT_TITLE),0x111111);icon_button(p,"close",w-76,12,A_BACK,0);
    const char *styles[]={tr("清楚自然","Natural"),tr("简洁","Concise"),tr("正式","Formal"),tr("仅改错字","Typos")};int cw=(w-72)/4;
    for(unsigned i=0;i<4;i++)pill(p,styles[i],24+i*(cw+8),92,cw,page->ai_mode==(notes_ai_mode_t)i,A_AI,i);
    txt(p,page->show_original?tr("原文","Original"):tr("整理后的文字","Revised text"),28,180,w-56,small_font(),0x666666);
    bool paged=page->ai_mode==AI_TASKS&&!page->show_original&&page->preview.count>8;
    if(page->offset>=page->preview.count)page->offset=0;
    lv_obj_t *content=design_scroll(p,28,230,w-56,h-(paged?504:440));note_t *n=page->show_original?&page->original:&page->preview;int y=0;
    size_t begin=paged?page->offset:0,end=paged?LV_MIN(begin+8,n->count):n->count;
    for(size_t i=begin;i<end;i++){note_block_t *b=n->blocks+i;if(page->show_original&&(b->kind==NOTE_IMAGE||b->kind==NOTE_AUDIO))continue;
        int x=0;if(page->ai_mode==AI_TASKS&&!page->show_original){pill(content,b->checked?"✓":"",0,y,56,b->checked,A_TASK_SELECT,b->id);x=72;}
        lv_obj_t *t=txt(content,b->text,x,y,w-56-x,body_font(),0x222222);lv_obj_update_layout(t);y+=lv_obj_get_height(t)+28;}
    if(paged){if(begin)pill(p,tr("上一组","Previous"),28,h-270,170,false,A_PREV,0);if(end<n->count)pill(p,tr("下一组","Next"),w-198,h-270,170,false,A_NEXT,0);}
    if(!n->count)txt(content,page->ai_mode==AI_TASKS?tr("没有提取到待办","No tasks found"):tr("没有可显示的内容","No content"),0,20,w-56,body_font(),0x666666);
    menu_row(p,page->show_original?tr("查看结果","View result"):tr("查看原文","View original"),NULL,NULL,h-190,w,A_AI_ORIGINAL,0,false);box(p,28,h-129,w-56,1,0,0xcccccc,0);
    pill(p,tr("保留原文","Keep original"),24,h-94,(w-64)/2,false,A_DISCARD_AI,0);lv_obj_t *apply=pill(p,tr("采用结果","Apply result"),40+(w-64)/2,h-94,(w-64)/2,true,A_ADOPT,0);if(!page->preview.count||page->adopt_pending)lv_obj_add_state(apply,LV_STATE_DISABLED);
}
static void design_insert(void){lv_obj_t *p=design_overlay(14,page->height-360,page->width-28,332);txt(p,tr("插入内容","Insert"),28,24,page->width-120,epd_app_font_role(EPD_FONT_TITLE),0x222222);menu_row(p,tr("图片","Image"),NULL,"clip",90,page->width-28,A_ATTACH,0,false);menu_row(p,tr("正文段落","Paragraph"),NULL,"text",166,page->width-28,A_INSERT,NOTE_TEXT,false);menu_row(p,tr("分隔线","Divider"),NULL,NULL,242,page->width-28,A_INSERT,NOTE_RULE,false);}
static void design_export(void){lv_obj_t *p=design_overlay(14,page->height-370,page->width-28,342);txt(p,tr("导出笔记","Export note"),28,24,page->width-120,epd_app_font_role(EPD_FONT_TITLE),0x222222);const char *names[]={"TXT","Markdown + 附件 ZIP",tr("完整笔记 ZIP","Complete note ZIP")};for(unsigned i=0;i<3;i++)menu_row(p,names[i],NULL,NULL,90+i*76,page->width-28,A_EXPORT,i,false);}
static void design_note_menu(void){
    int w=page->width-170,h=600;lv_obj_t *p=design_overlay(page->width-w-18,80,w,h);lv_obj_t *list=design_scroll(p,0,10,w,h-20);
    const char *names[]={tr("标题和笔记本","Title & notebook"),page->note.pinned?tr("取消置顶","Unpin"):tr("置顶","Pin"),tr("添加内容","Insert content"),tr("导出","Export"),page->hide_completed?tr("显示已完成","Show completed"):tr("隐藏已完成","Hide completed"),tr("恢复 AI 修改前版本","Restore before AI"),page->note.deleted?tr("恢复笔记","Restore note"):tr("移到回收站","Move to trash")};
    action_t actions[]={A_META,A_PIN,A_INSERT_MENU,A_EXPORT_MENU,A_HIDE_COMPLETED,A_REVERT,page->note.deleted?A_RESTORE:A_DELETE};for(int i=0;i<7;i++)menu_row(list,names[i],NULL,NULL,72*i,w,actions[i],i==6?page->note.id:0,false);if(page->note.deleted)menu_row(list,tr("永久删除","Delete permanently"),NULL,NULL,504,w,A_PURGE,page->note.id,false);
}
static void design_tasks(void){
    design_background(0xf1f1f1);icon_button(page->body,"back",10,70,A_FILTER,0);txt(page->body,tr("待办","Tasks"),28,158,page->width-80,heading_font(),0x111111);design_search(238,true);
    lv_obj_t *list=design_scroll(page->body,24,342,page->width-48,page->height-390);int y=0;size_t totals[2]={notes_tasks_query(NULL,0,0,false,page->search),notes_tasks_query(NULL,0,0,true,page->search)};page->total=totals[0]>totals[1]?totals[0]:totals[1];
    for(unsigned completed=0;completed<2;completed++){
        char heading[64];snprintf(heading,sizeof(heading),completed?tr("已完成  %u","Completed  %u"):tr("待完成  %u","Pending  %u"),(unsigned)totals[completed]);lv_obj_t *head=txt(list,heading,8,y,page->width-100,small_font(),0x555555);if(completed){clickable(head,A_COMPLETED,0);icon_button(list,page->completed?"down":"right",page->width-100,y-14,A_COMPLETED,0);}y+=56;
        if(completed&&!page->completed)continue;
        notes_task_t items[12];size_t total=notes_tasks_query(items,page->offset,12,completed,page->search),count=total>page->offset?total-page->offset:0;if(count>12)count=12;
        for(size_t i=0;i<count;){size_t end=i+1;while(end<count&&items[end].note==items[i].note)++end;bool group=end-i>1;int h=group?80:0;
            lv_obj_t *card=box(list,0,y,page->width-48,100,24,0xffffff,0);
            if(group){lv_obj_t *title=txt(card,items[i].title,24,22,page->width-130,epd_app_font_style(EPD_FONT_BODY,1),0x222222);clickable(title,A_TASK_OPEN,(uint32_t)(page->offset+i)|(completed?0x80000000u:0));}
            for(size_t j=i;j<end;j++){uint32_t index=(uint32_t)(page->offset+j)|(completed?0x80000000u:0);lv_obj_t *check=box(card,24,h+22,30,30,6,completed?0x444444:0xffffff,1);clickable(check,A_TASK_CHECK,index);if(completed)txt(check,"✓",3,0,28,caption_font(),0xffffff);
                lv_obj_t *t=txt(card,items[j].text,72,h+18,page->width-150,body_font(),completed?0x777777:0x222222);clickable(t,A_TASK_OPEN,index);lv_obj_update_layout(t);h+=LV_MAX(62,lv_obj_get_height(t)+30);}
            if(!group){txt(card,items[i].title,72,h,page->width-150,caption_font(),0x666666);h+=38;}lv_obj_set_height(card,h+20);y+=h+40;i=end;
        }
    }
    if(page->offset)pill(list,tr("上一组","Previous"),0,y,180,false,A_PREV,0);if(page->offset+12<page->total)pill(list,tr("下一组","Next"),page->width-240,y,180,false,A_NEXT,0);
    lv_obj_t *add=box(page->body,page->width-118,page->height-134,86,86,43,0x222222,0);clickable(add,A_NEW,1);icon(add,"plus",23,23,0xffffff);
}
static void design_confirm(void){
    if(!page->confirm_action)return;int w=page->width-48,h=page->confirm_action==A_BOOK_DELETE?360:280;lv_obj_t *p=design_overlay(24,(page->height-h)/2,w,h);
    txt(p,page->confirm_action==A_BOOK_DELETE?tr("删除笔记本，如何处理其中的笔记？","Delete notebook: where should its notes go?"):page->confirm_action==A_PURGE?tr("永久删除这篇笔记及附件？","Permanently delete this note and attachments?"):tr("移到回收站？","Move to trash?"),24,28,w-48,body_font(),0x222222);
    if(page->confirm_action==A_BOOK_DELETE){pill(p,tr("移到未分类","Move to unfiled"),24,160,w-48,false,A_BOOK_DELETE_CONFIRM,0);pill(p,tr("移到回收站","Move to trash"),24,236,w-48,false,A_BOOK_DELETE_CONFIRM,1);}
    else{pill(p,tr("取消","Cancel"),24,184,(w-72)/2,false,A_CONFIRM,0);pill(p,tr("确认","Confirm"),48+(w-72)/2,184,(w-72)/2,true,A_CONFIRM,1);}
}

static void design_soft_shadow(lv_obj_t *object){
    lv_obj_set_style_shadow_color(object,lv_color_hex(0x000000),0);lv_obj_set_style_shadow_width(object,12,0);
    lv_obj_set_style_shadow_opa(object,LV_OPA_10,0);lv_obj_set_style_shadow_ofs_y(object,3,0);
}
static void design_form_header(const char *title){
    design_background(0xf1f1f1);icon_button(page->body,"back",12,76,A_BACK,0);
    txt(page->body,title,30,164,page->width-60,heading_font(),0x111111);
}
static void design_meta(void){
    design_form_header(tr("编辑标题","Edit title"));icon_button(page->body,"check",page->width-80,76,A_SAVE,0);
    lv_obj_t *card=box(page->body,24,250,page->width-48,168,24,0xffffff,0);design_soft_shadow(card);
    txt(card,tr("标题","Title"),24,20,page->width-96,small_font(),0x666666);
    page->input=textarea(page->note.title,48,312,page->width-96,80,true);
    lv_textarea_set_placeholder_text(page->input,tr("未命名笔记","Untitled note"));lv_obj_set_style_border_width(page->input,0,0);
    lv_obj_set_style_text_font(page->input,epd_app_font_role(EPD_FONT_TITLE),0);
    txt(page->body,tr("所在笔记本","Notebook"),32,460,page->width-64,small_font(),0x666666);
    lv_obj_t *book=box(page->body,24,510,page->width-48,100,24,0xffffff,0);design_soft_shadow(book);clickable(book,A_PICK_BOOK,0);
    icon(book,"folder",24,28,0x333333);txt(book,page->note.notebook[0]?page->note.notebook:tr("未分类","Unfiled"),88,34,page->width-200,body_font(),0x222222);icon(book,"right",page->width-120,30,0x777777);
}
static void design_book_picker(void){
    design_form_header(page->batch_move?tr("移动到笔记本","Move to notebook"):tr("选择笔记本","Choose notebook"));
    icon_button(page->body,"plus",page->width-80,76,A_BOOK_NEW,0);
    lv_obj_t *list=design_scroll(page->body,24,258,page->width-48,page->height-316);
    char names[8][96];size_t total=notes_books(names,page->offset,8),count=total>page->offset?total-page->offset:0;if(count>8)count=8;page->total=total;
    for(size_t i=0;i<count+1;++i){const char *name=i?names[i-1]:"";bool chosen=!page->batch_move&&!strcmp(name,page->note.notebook);
        lv_obj_t *row=box(list,0,(int)i*112,page->width-48,96,24,0xffffff,chosen?2:0);design_soft_shadow(row);clickable(row,A_CHOOSE_BOOK,i?(uint32_t)(page->offset+i):0);
        icon(row,"folder",24,26,0x333333);txt(row,*name?name:tr("未分类","Unfiled"),88,18,page->width-210,body_font(),0x222222);
        char detail[48];size_t n=notes_rows(NULL,0,0,"",*name?name:"\1",0,0);snprintf(detail,sizeof(detail),tr("%u 篇笔记","%u notes"),(unsigned)n);txt(row,detail,88,57,page->width-210,caption_font(),0x777777);
        if(chosen)icon(row,"check",page->width-124,28,0x222222);
    }
    int y=(int)(count+1)*112;if(page->offset)pill(list,tr("上一组","Previous"),8,y,180,false,A_PREV,0);
    if(page->offset+count<total)pill(list,tr("下一组","Next"),page->width-250,y,180,false,A_NEXT,0);
}
static void design_book_edit(const char *draft){
    design_form_header(page->book_edit?tr("重命名笔记本","Rename notebook"):tr("新建笔记本","New notebook"));
    icon_button(page->body,"check",page->width-80,76,A_SAVE,0);
    lv_obj_t *card=box(page->body,24,260,page->width-48,176,24,0xffffff,0);design_soft_shadow(card);icon(card,"folder",24,24,0x333333);
    txt(card,tr("笔记本名称","Notebook name"),86,30,page->width-160,small_font(),0x666666);
    page->input=textarea(draft?draft:page->book_edit?page->notebook:"",48,344,page->width-96,70,true);
    lv_obj_set_style_border_width(page->input,0,0);lv_textarea_set_placeholder_text(page->input,tr("输入名称","Enter a name"));
}
static void design_search_page(const char *draft){
    design_form_header(tr("搜索笔记","Search notes"));
    lv_obj_t *bar=box(page->body,24,244,page->width-48,80,30,0xffffff,0);design_soft_shadow(bar);
    icon(bar,"search",20,20,0x555555);
    page->input=textarea(draft?draft:page->search,92,253,page->width-238,62,true);
    lv_obj_set_style_border_width(page->input,0,0);lv_textarea_set_placeholder_text(page->input,tr("标题、正文或转写","Title, text or transcript"));
    icon_button(bar,"close",page->width-190,8,A_CLEAR_SEARCH,0);icon_button(bar,"right",page->width-120,8,A_SAVE,0);
    page->editor_host=design_scroll(page->body,24,348,page->width-48,page->height-392);
    lv_obj_t *list=page->editor_host;
    if(!page->search[0]){icon(list,"search",(page->width-88)/2,64,0x777777);page->total=0;return;}
    notes_row_t rows[8];size_t total=notes_rows(rows,page->offset,8,page->search,"",0,page->sort),count=total>page->offset?total-page->offset:0;if(count>8)count=8;page->total=total;
    if(!total&&!page->search_waiting){icon(list,"search",(page->width-88)/2,64,0x777777);txt(list,tr("没有找到相关笔记","No matching notes"),28,140,page->width-104,body_font(),0x444444);txt(list,tr("试试其他关键词","Try another keyword"),28,188,page->width-104,small_font(),0x777777);return;}
    for(size_t i=0;i<count;++i){lv_obj_t *card=box(list,0,(int)i*190,page->width-48,174,24,0xffffff,0);design_soft_shadow(card);clickable(card,A_OPEN,rows[i].id);
        lv_obj_t *title=txt(card,rows[i].title,22,20,page->width-110,body_font(),0x111111);lv_obj_set_height(title,38);lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
        lv_obj_t *summary=txt(card,rows[i].summary,22,66,page->width-110,small_font(),0x555555);lv_obj_set_height(summary,34);lv_label_set_long_mode(summary,LV_LABEL_LONG_DOT);
        char date[64];design_date(rows[i].updated,date,sizeof(date));txt(card,date,22,124,page->width-110,caption_font(),0x777777);
    }
    int y=(int)count*190;if(page->offset)pill(list,tr("上一组","Previous"),8,y,180,false,A_PREV,0);
    if(page->offset+count<total)pill(list,tr("下一组","Next"),page->width-250,y,180,false,A_NEXT,0);
}

static uint32_t audio_position(void){
    return page->status.playback_block==page->block&&page->status.playback_note==page->note.id?page->status.playback_ms:0;
}
static void audio_duration(uint32_t milliseconds,char *out,size_t size){
    unsigned seconds=milliseconds/1000;snprintf(out,size,"%02u:%02u:%02u",seconds/3600,seconds/60%60,seconds%60);
}
static void audio_seek(lv_event_t *e){
    if(!page)return;note_block_t *b=note_find(&page->note,page->block);lv_indev_t *device=lv_event_get_indev(e);if(!b||!device||!b->milliseconds)return;
    lv_point_t point;lv_area_t area;lv_indev_get_point(device,&point);lv_obj_get_coords(lv_event_get_target_obj(e),&area);
    int x=LV_CLAMP(0,point.x-area.x1,area.x2-area.x1);uint32_t ms=(uint32_t)((uint64_t)b->milliseconds*x/(area.x2-area.x1));
    if(ms>=b->milliseconds)ms=b->milliseconds>1000?b->milliseconds-1000:0;
    page->action=A_SEEK;page->value=ms;
}
static void design_audio_update(void){
    note_block_t *b=note_find(&page->note,page->block);if(!b||!page->audio_time)return;
    uint32_t ms=audio_position();bool playing=page->status.playing&&page->status.playback_block==page->block&&page->status.playback_note==page->note.id;
    if(playing)ms=ms/5000*5000;
    char time[32];audio_duration(ms,time,sizeof(time));epd_app_text(page->audio_time,time);
    int width=b->milliseconds?(int)((uint64_t)LV_MIN(ms,b->milliseconds)*(page->width-96)/b->milliseconds):0;
    if(page->audio_fill)lv_obj_set_width(page->audio_fill,LV_MAX(1,width));
}
static void design_audio(void){
    design_background(0xffffff);icon_button(page->body,"back",10,70,A_BACK,0);icon_button(page->body,"more",page->width-82,70,A_AUDIO_MENU,0);
    note_block_t *b=note_find(&page->note,page->block);if(!b)return;
    lv_obj_t *name=txt(page->body,b->text[0]?b->text:tr("录音","Recording"),32,164,page->width-64,heading_font(),0x111111);
    lv_obj_set_height(name,heading_font()->line_height*2+8);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);clickable(name,A_EDIT,b->id);
    char title[160];note_display_title(&page->note,title,sizeof(title));txt(page->body,title,34,278,page->width-68,small_font(),0x777777);
    page->audio_time=txt(page->body,"00:00:00",24,388,page->width-48,page->clock_font?page->clock_font:heading_font(),0x111111);lv_obj_set_style_text_align(page->audio_time,LV_TEXT_ALIGN_CENTER,0);
    char total[32];audio_duration(b->milliseconds,total,sizeof(total));
    lv_obj_t *duration=txt(page->body,total,24,502,page->width-48,small_font(),0x777777);lv_obj_set_style_text_align(duration,LV_TEXT_ALIGN_CENTER,0);
    int w=page->width-96;lv_obj_t *seek=box(page->body,48,570,w,56,0,0xffffff,0);
    lv_obj_add_flag(seek,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(seek,audio_seek,LV_EVENT_CLICKED,NULL);
    box(seek,0,25,w,6,3,0xe4e4e4,0);page->audio_fill=box(seek,0,25,1,6,3,0x222222,0);
    txt(page->body,"00:00:00",48,630,w/2,caption_font(),0x777777);lv_obj_t *end=txt(page->body,total,page->width/2,630,w/2,caption_font(),0x777777);lv_obj_set_style_text_align(end,LV_TEXT_ALIGN_RIGHT,0);
    bool playing=page->status.playing&&page->status.playback_block==b->id&&page->status.playback_note==page->note.id;
    lv_obj_t *controls=box(page->body,40,714,page->width-80,142,44,0xf8f8f8,0);design_soft_shadow(controls);
    int third=(page->width-80)/3;
    for(int i=0;i<3;i++){
        lv_obj_t *control=box(controls,i*third,0,third,142,40,i==1?0xeeeeee:0xf8f8f8,0);
        clickable(control,i==1?(playing?A_PLAY_STOP:A_PLAY):A_SKIP,i==2?1:0);
        icon(control,i==1?(playing?"pause":"play"):i==0?"undo":"redo",(third-40)/2,27,0x222222);
        lv_obj_t *text=txt(control,i==1?(playing?tr("暂停","Pause"):tr("播放","Play")):i==0?tr("后退 10 秒","Back 10 s"):tr("前进 10 秒","Forward 10 s"),4,88,third-8,small_font(),0x444444);lv_obj_set_style_text_align(text,LV_TEXT_ALIGN_CENTER,0);
    }
    bool wav=strlen(b->attachment)>4&&!strcmp(b->attachment+strlen(b->attachment)-4,".wav");int cw=wav?(page->width-104)/2:page->width-80;
    for(int i=0;i<(wav?2:1);i++){
        bool transcription=wav&&!i;lv_obj_t *action=box(page->body,40+i*(cw+24),914,cw,128,32,0xf8f8f8,0);design_soft_shadow(action);
        clickable(action,transcription?A_TRANSCRIBE:A_MARKS,0);icon(action,transcription?"text":"flag",(cw-40)/2,20,0x333333);
        lv_obj_t *caption=txt(action,transcription?tr("转写","Transcribe"):tr("录音标记","Markers"),8,78,cw-16,small_font(),0x333333);lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);
    }
    design_audio_update();
}
static void design_audio_menu(void){
    int w=page->width-142;lv_obj_t *p=design_overlay(page->width-w-18,82,w,460);
    note_block_t *b=note_find(&page->note,page->block);if(!b)return;size_t index=(size_t)(b-page->note.blocks);
    const char *names[]={tr("重命名录音","Rename recording"),tr("在录音前添加文字","Add text before"),tr("在录音后添加文字","Add text after"),tr("上移","Move up"),tr("下移","Move down"),tr("删除录音","Remove recording")};
    action_t actions[]={A_EDIT,A_INSERT_AT,A_INSERT_AT,A_UP,A_DOWN,A_REMOVE};uint32_t values[]={b->id,(uint32_t)index,(uint32_t)index+1,0,0,0};
    for(unsigned i=0;i<6;i++)menu_row(p,names[i],NULL,NULL,12+i*72,w,actions[i],values[i],false);
}
static void design_audio_name(const char *draft){
    bool mark=page->mode==MARK_EDIT;note_block_t *b=note_find(&page->note,page->block);
    design_form_header(mark?tr("编辑标记","Edit marker"):tr("编辑附件名称","Edit attachment name"));icon_button(page->body,"check",page->width-80,76,mark?A_MARK_SAVE:A_SAVE,0);
    lv_obj_t *card=box(page->body,24,258,page->width-48,176,24,0xffffff,0);design_soft_shadow(card);icon(card,mark?"flag":"mic",24,26,0x333333);
    txt(card,tr("名称","Name"),86,32,page->width-160,small_font(),0x666666);
    const char *text=b?(mark?(page->mark_index<b->mark_count?b->marks[page->mark_index].title:""):b->text):"";
    page->input=textarea(draft?draft:text,48,344,page->width-96,70,true);lv_obj_set_style_border_width(page->input,0,0);
}
static void design_marks(void){
    design_form_header(tr("录音标记","Recording markers"));note_block_t *b=note_find(&page->note,page->block);page->total=b?b->mark_count:0;
    lv_obj_t *list=design_scroll(page->body,24,260,page->width-48,page->height-310);
    if(!page->total){icon(list,"flag",(page->width-88)/2,100,0x777777);lv_obj_t *empty=txt(list,tr("这段录音还没有标记","No markers in this recording"),24,172,page->width-96,body_font(),0x777777);lv_obj_set_style_text_align(empty,LV_TEXT_ALIGN_CENTER,0);}
    int y=0;for(size_t i=page->offset;i<page->total&&i<page->offset+5;i++,y+=140){
        lv_obj_t *row=box(list,0,y,page->width-48,120,28,0xffffff,0);design_soft_shadow(row);clickable(row,A_PLAY,b->marks[i].milliseconds?b->marks[i].milliseconds:1);
        icon(row,"play",20,38,0x222222);txt(row,b->marks[i].title,82,20,page->width-238,body_font(),0x222222);
        char time[32];audio_duration(b->marks[i].milliseconds,time,sizeof(time));txt(row,time,82,67,page->width-238,small_font(),0x777777);icon_button(row,"text",page->width-130,28,A_MARK_EDIT,(uint32_t)i);
    }
    if(page->offset)pill(list,tr("上一组","Previous"),0,y,180,false,A_PREV,0);if(page->offset+5<page->total)pill(list,tr("下一组","Next"),page->width-240,y,180,false,A_NEXT,0);
}
