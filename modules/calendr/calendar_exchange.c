#include "calendar_exchange.h"
#include "storage_file.h"
#include "system_time.h"
#include <dfs_posix.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#define BACKUP_MAGIC 0x43424b31u
static bool suffix(const char *p,const char *ending){size_t n=strlen(p),k=strlen(ending);if(n<k)return false;for(size_t i=0;i<k;++i)if(tolower((unsigned char)p[n-k+i])!=ending[i])return false;return true;}
static bool append(char **s,size_t *used,size_t *capacity,const char *text)
{
    size_t len=strlen(text);if(*used+len+1>2u*1024u*1024u)return false;
    if(*used+len+1>*capacity){size_t n=(*used+len+1)*2;char *p=cal_alloc(n);if(!p)return false;if(*used)memcpy(p,*s,*used);cal_free(*s);*s=p;*capacity=n;}
    memcpy(*s+*used,text,len+1);*used+=len;return true;
}
static bool field(char **s,size_t *used,size_t *cap,const char *key,const char *value)
{
    if(!append(s,used,cap,key)||!append(s,used,cap,":"))return false;
    unsigned col=(unsigned)strlen(key)+1;
    for(const unsigned char *p=(const unsigned char *)(value?value:"");*p;) {
        char text[8]={0};unsigned n=1;
        if(*p=='\n')strcpy(text,"\\n");else if(*p=='\r'){++p;continue;}
        else if(*p=='\\'||*p==','||*p==';'){text[0]='\\';text[1]=(char)*p;}
        else {if(*p>=0xf0)n=4;else if(*p>=0xe0)n=3;else if(*p>=0xc0)n=2;memcpy(text,p,n);}
        if(col+strlen(text)>72){if(!append(s,used,cap,"\r\n "))return false;col=1;}
        if(!append(s,used,cap,text))return false;col+=(unsigned)strlen(text);p+=n;
    }
    return append(s,used,cap,"\r\n");
}
static void date_text(char out[32],int64_t utc,bool date_only)
{
    int32_t day=epd_date_utc_day(utc,0);epd_date_t d=epd_date_from_day(day);int minute=(int)((utc-(int64_t)day*86400)/60);
    if(date_only)snprintf(out,32,"%04d%02u%02u",d.year,d.month,d.day);
    else snprintf(out,32,"%04d%02u%02uT%02d%02d00Z",d.year,d.month,d.day,minute/60,minute%60);
}
bool cal_ics_supported(const cal_book_t *b)
{
    for(size_t i=0;i<b->count;++i){const cal_event_t *e=b->events[i];
        if(e->rule.lunar_month||e->rule.month_end||(e->rule.repeat&&!e->all_day&&e->rule.timezone_minutes))return false;}
    return true;
}
bool cal_export_file(const cal_book_t *b,const char *path)
{
    if(!b||!path)return false;
    char directory[512];if(strlen(path)>=sizeof(directory))return false;strcpy(directory,path);char *slash=strrchr(directory,'/');if(!slash)return false;*slash=0;if(!storage_mkdirs(directory))return false;
    if(suffix(path,".calbak")){void *bytes=NULL;size_t n;bool ok=cal_encode(b,&bytes,&n);if(ok)ok=storage_record_save(path,BACKUP_MAGIC,1,bytes,n);cal_free(bytes);return ok;}
    if(!suffix(path,".ics"))return false;
    char *text=NULL;size_t used=0,cap=0;bool ok=append(&text,&used,&cap,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//SiFli EPD//Calendar//ZH\r\n");
    for(size_t i=0;ok&&i<b->count;++i) {
        const cal_event_t *e=b->events[i];if(e->rule.lunar_month||e->rule.month_end||(e->rule.repeat&&!e->all_day&&e->rule.timezone_minutes)){ok=false;break;}
        char value[256],date[32];snprintf(value,sizeof(value),"%s",e->uid);
        date_text(date,system_time_get_utc(),false);
        ok=append(&text,&used,&cap,"BEGIN:VEVENT\r\n")&&field(&text,&used,&cap,"DTSTAMP",date)&&field(&text,&used,&cap,"UID",value)&&field(&text,&used,&cap,"SUMMARY",e->title)&&field(&text,&used,&cap,"LOCATION",e->place)&&field(&text,&used,&cap,"DESCRIPTION",e->notes);
        int64_t t=(int64_t)e->rule.start_day*86400+(e->all_day?0:(e->rule.minute-e->rule.timezone_minutes)*60);
        date_text(date,t,e->all_day);ok=ok&&field(&text,&used,&cap,e->all_day?"DTSTART;VALUE=DATE":"DTSTART",date);
        date_text(date,t+e->duration_minutes*INT64_C(60),e->all_day);ok=ok&&field(&text,&used,&cap,e->all_day?"DTEND;VALUE=DATE":"DTEND",date);
        if(e->rule.repeat) {
            static const char *freq[]={"","DAILY","WEEKLY","MONTHLY","YEARLY"};
            snprintf(value,sizeof(value),"RRULE:FREQ=%s",freq[e->rule.repeat]);
            if(e->rule.repeat==EPD_REPEAT_WEEKLY){strcat(value,";BYDAY=");static const char *w[]={"SU","MO","TU","WE","TH","FR","SA"};bool comma=false;for(unsigned j=0;j<7;++j)if(e->rule.weekdays&(1u<<j)){if(comma)strcat(value,",");strcat(value,w[j]);comma=true;}}
            if(e->rule.until_day<epd_date_day((epd_date_t){2099,12,31})){date_text(date,(int64_t)e->rule.until_day*86400+(e->all_day?0:86399-e->rule.timezone_minutes*60),e->all_day);strcat(value,";UNTIL=");strcat(value,date);}
            ok=ok&&append(&text,&used,&cap,value)&&append(&text,&used,&cap,"\r\n");
        }
        for(unsigned j=0;ok&&j<e->rule.excluded_count;++j){date_text(date,(int64_t)e->rule.excluded[j]*86400+(e->all_day?0:(e->rule.minute-e->rule.timezone_minutes)*60),e->all_day);ok=field(&text,&used,&cap,e->all_day?"EXDATE;VALUE=DATE":"EXDATE",date);}
        if(e->reminder_minutes>=0){snprintf(value,sizeof(value),"BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT%ldM\r\nDESCRIPTION:Reminder\r\nEND:VALARM\r\n",(long)e->reminder_minutes);ok=ok&&append(&text,&used,&cap,value);}
        ok=ok&&append(&text,&used,&cap,"END:VEVENT\r\n");
    }
    ok=ok&&append(&text,&used,&cap,"END:VCALENDAR\r\n");
    if(ok)ok=storage_file_replace(path,text,used);cal_free(text);return ok;
}
static bool unescape(char *s)
{
    char *out=s;for(char *p=s;*p;++p){if(*p=='\\'){++p;if(!*p)return false;if(*p=='n'||*p=='N')*out++='\n';else if(*p=='\\'||*p==','||*p==';')*out++=*p;else return false;}else *out++=*p;}*out=0;return true;
}
static bool read_date(const char *s,bool date_only,int64_t *value)
{
    size_t n=strlen(s);if(n!=(date_only?8u:16u))return false;
    for(size_t i=0;i<n;++i)if(i==8&&!date_only){if(s[i]!='T')return false;}else if(i==15&&!date_only){if(s[i]!='Z')return false;}else if(s[i]<'0'||s[i]>'9')return false;
    int y,m,d,h=0,minute=0,sec=0; if(sscanf(s,"%4d%2d%2d",&y,&m,&d)!=3)return false;
    if(!date_only&&sscanf(s+9,"%2d%2d%2d",&h,&minute,&sec)!=3)return false;
    epd_date_t date={(int16_t)y,(uint8_t)m,(uint8_t)d};if(!epd_date_valid(date)||h>23||minute>59||sec>59)return false;
    *value=(int64_t)epd_date_day(date)*86400+h*3600+minute*60+sec;return true;
}
static bool read_repeat(epd_calendar_rule_t *r,char *value,int64_t *until)
{
    bool frequency=false,byday=false;char *next=value;
    while(next&&*next){char *part=next;next=strchr(part,';');if(next)*next++=0;char *eq=strchr(part,'=');if(!eq)return false;*eq++=0;
        if(!strcmp(part,"FREQ")){if(frequency)return false;const char *freq[]={"NONE","DAILY","WEEKLY","MONTHLY","YEARLY"};for(int i=1;i<=4;++i)if(!strcmp(eq,freq[i])){r->repeat=(uint8_t)i;frequency=true;}if(!frequency)return false;}
        else if(!strcmp(part,"INTERVAL")){if(strcmp(eq,"1"))return false;}
        else if(!strcmp(part,"UNTIL")){int64_t t;if(!read_date(eq,strlen(eq)==8,&t))return false;*until=t;r->until_day=epd_date_utc_day(t,0);if(strlen(eq)==8)*until+=86399;}
        else if(!strcmp(part,"BYDAY")){byday=true;static const char *days[]={"SU","MO","TU","WE","TH","FR","SA"};r->weekdays=0;while(*eq){bool found=false;for(int i=0;i<7;++i)if(!strncmp(eq,days[i],2)){r->weekdays|=1u<<i;found=true;break;}if(!found)return false;eq+=2;if(*eq==',')++eq;else if(*eq)return false;}}
        else if(!strcmp(part,"WKST")){if(strcmp(eq,"MO")&&strcmp(eq,"SU"))return false;}
        else return false;
    }
    return frequency&&(!byday||r->repeat==EPD_REPEAT_WEEKLY);
}
static bool parse_ics(cal_book_t *b,char *s)
{
    /* Unfold into the same buffer before tokenizing. */
    char *out=s;for(char *p=s;*p;){if(*p=='\r'&&p[1]=='\n')++p;if(*p=='\n'&&(p[1]==' '||p[1]=='\t')){p+=2;continue;}*out++=*p++;}*out=0;
    cal_event_t *e=cal_alloc(sizeof(*e));if(!e)return false;bool active=false,alarm_block=false,calendar=false,ended=false,ok=true;int64_t start=0,end=0,until=INT64_MAX;bool have_start=false,have_end=false;unsigned total=0;char uid[128]={0};
    char *line=s;
    while(line&&*line&&ok){char *next=strchr(line,'\n');if(next)*next++=0;char *colon=strchr(line,':');if(!colon){if(*line)ok=false;line=next;continue;}*colon++=0;
        if(!strcmp(line,"BEGIN")&&!strcmp(colon,"VCALENDAR")){if(calendar)ok=false;calendar=true;}
        else if(!strcmp(line,"END")&&!strcmp(colon,"VCALENDAR")){ended=true;if(active)ok=false;}
        else if(!strcmp(line,"BEGIN")&&!strcmp(colon,"VEVENT")){if(active||!calendar||ended){ok=false;break;}active=true;cal_event_clear(e);e->reminder_minutes=-1;e->rule.until_day=epd_date_day((epd_date_t){2099,12,31});have_start=have_end=false;until=INT64_MAX;uid[0]=0;}
        else if(!strcmp(line,"END")&&!strcmp(colon,"VEVENT")){
            if(!active||alarm_block||!have_start||!have_end||end<=start||start%60||end%60||!uid[0]){ok=false;break;}
            if(until!=INT64_MAX&&until-(int64_t)e->rule.until_day*86400<e->rule.minute*60)--e->rule.until_day;
            e->duration_minutes=(uint32_t)((end-start)/60);if(!e->rule.weekdays)e->rule.weekdays=1u<<epd_date_weekday(e->rule.start_day);
            e->id=0;strcpy(e->uid,uid);
            for(size_t i=0;i<b->count;++i)if(!strcmp(b->events[i]->uid,uid)){e->id=b->events[i]->id;break;}
            ok=cal_put(b,e);active=false;++total;
        }
        else if(active){
            if(!strcmp(line,"BEGIN")&&!strcmp(colon,"VALARM"))alarm_block=true;
            else if(!strcmp(line,"END")&&!strcmp(colon,"VALARM"))alarm_block=false;
            else if(alarm_block){if(!strcmp(line,"TRIGGER")){long minutes;int n=0;if(sscanf(colon,"-PT%ldM%n",&minutes,&n)!=1||colon[n]||minutes<0||minutes>525600)ok=false;else e->reminder_minutes=(int32_t)minutes;}else if(!strcmp(line,"ACTION")&&strcmp(colon,"DISPLAY"))ok=false;}
            else if(!strcmp(line,"UID")){if(!unescape(colon)){ok=false;break;}if(strlen(colon)>=sizeof(uid))ok=false;else strcpy(uid,colon);}
            else if(!strcmp(line,"SUMMARY")||!strcmp(line,"LOCATION")||!strcmp(line,"DESCRIPTION")){
                ok=unescape(colon);size_t len=strlen(colon);
                if(!strcmp(line,"SUMMARY")){if(len>=sizeof(e->title))ok=false;else strcpy(e->title,colon);}
                else if(!strcmp(line,"LOCATION")){if(len>=sizeof(e->place))ok=false;else strcpy(e->place,colon);}
                else {if(len>CAL_TEXT_BYTES)ok=false;else {cal_free(e->notes);e->notes=cal_alloc(len+1);if(e->notes)strcpy(e->notes,colon);else ok=false;}}
            }
            else if(!strcmp(line,"DTSTART")||!strcmp(line,"DTSTART;VALUE=DATE")){e->all_day=strchr(line,';')!=NULL;ok=read_date(colon,e->all_day,&start);have_start=ok;e->rule.start_day=epd_date_utc_day(start,0);e->rule.minute=(uint16_t)((start-(int64_t)e->rule.start_day*86400)/60);}
            else if(!strcmp(line,"DTEND")||!strcmp(line,"DTEND;VALUE=DATE")){if(!have_start||e->all_day!=(strchr(line,';')!=NULL)){ok=false;break;}ok=read_date(colon,strchr(line,';')!=NULL,&end);have_end=ok;}
            else if(!strcmp(line,"RRULE"))ok=read_repeat(&e->rule,colon,&until);
            else if(!strcmp(line,"EXDATE")||!strcmp(line,"EXDATE;VALUE=DATE")){if(!have_start||e->all_day!=(strchr(line,';')!=NULL)){ok=false;break;}char *v=colon;while(v&&*v&&ok){char *comma=strchr(v,',');if(comma)*comma++=0;int64_t t;ok=read_date(v,strchr(line,';')!=NULL,&t)&&e->rule.excluded_count<EPD_CALENDAR_EXCEPTIONS;if(ok)e->rule.excluded[e->rule.excluded_count++]=epd_date_utc_day(t,0);v=comma;}}
            else if(!strcmp(line,"DTSTAMP")||!strcmp(line,"CREATED")||!strcmp(line,"LAST-MODIFIED")||!strcmp(line,"SEQUENCE")||!strcmp(line,"TRANSP")||!strcmp(line,"CLASS")){}
            else ok=false;
        }
        else if(!strcmp(line,"BEGIN"))ok=false;
        line=next;
    }
    cal_event_clear(e);cal_free(e);return ok&&calendar&&ended&&!active&&total>0;
}
bool cal_merge(cal_book_t *b,const cal_book_t *imported,bool replace_existing)
{
    uint32_t *ids=cal_alloc(imported->count*sizeof(*ids)+1);bool *existing=cal_alloc(imported->count+1);bool ok=ids&&existing;
    for(size_t i=0;ok&&i<imported->count;++i){const cal_event_t *e=imported->events[i];
        for(size_t j=0;j<b->count;++j)if(*e->uid&&!strcmp(b->events[j]->uid,e->uid)){ids[i]=b->events[j]->id;existing[i]=true;break;}
        if(!ids[i]){if(b->next_id==UINT32_MAX){ok=false;break;}ids[i]=b->next_id++;}}
    for(size_t i=0;ok&&i<imported->count;++i){cal_event_t e=*imported->events[i];if(existing[i]&&!replace_existing)continue;e.id=ids[i];
        if(e.parent){bool found=false;for(size_t j=0;j<imported->count;++j)if(imported->events[j]->id==e.parent){e.parent=ids[j];found=true;break;}if(!found){ok=false;break;}}
        ok=cal_put(b,&e);}
    cal_free(ids);cal_free(existing);return ok;
}
bool cal_import_file(cal_book_t *b,const char *path)
{
    if(!b||!path)return false;
    bool backup=suffix(path,".calbak"),ics=suffix(path,".ics");
    if(!backup&&!ics)return false;
    storage_lock();struct stat st;
    if(stat(path,&st)||st.st_size<=0||st.st_size>2*1024*1024+16){storage_unlock();return false;}
    size_t n=(size_t)st.st_size;bool loaded=false;void *data=cal_alloc(n+1);
    if(data&&backup)loaded=n>16&&storage_record_load(path,BACKUP_MAGIC,1,data,n-16);
    else if(data){int fd=open(path,O_RDONLY,0);if(fd>=0){size_t done=0;while(done<n){int r=read(fd,(uint8_t *)data+done,n-done);if(r<=0)break;done+=(size_t)r;}loaded=close(fd)==0&&done==n;}}
    storage_unlock();
    bool ok=false;
    if(loaded&&backup){cal_book_t imported;cal_book_init(&imported);if(cal_decode(&imported,data,n-16))ok=cal_merge(b,&imported,true);cal_book_clear(&imported);}
    else if(loaded&&!memchr(data,0,n))ok=parse_ics(b,data);
    cal_free(data);return ok;
}
