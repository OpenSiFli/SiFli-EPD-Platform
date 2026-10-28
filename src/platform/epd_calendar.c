#include "epd_calendar.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "epd_calendar_table.inc"

static bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
int epd_date_month_days(int y, int m)
{
    static const uint8_t n[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return y >= 1901 && y <= 2099 && m >= 1 && m <= 12 ? n[m-1] + (m == 2 && leap(y)) : 0;
}
bool epd_date_valid(epd_date_t d) { return d.day && d.day <= epd_date_month_days(d.year, d.month); }
int32_t epd_date_day(epd_date_t d)
{
    if (!epd_date_valid(d)) return INT32_MIN;
    int32_t days = 0;
    if (d.year >= 1970) for (int y=1970; y<d.year; ++y) days += 365 + leap(y);
    else for (int y=d.year; y<1970; ++y) days -= 365 + leap(y);
    for (int m=1; m<d.month; ++m) days += epd_date_month_days(d.year,m);
    return days + d.day - 1;
}
epd_date_t epd_date_from_day(int32_t day)
{
    epd_date_t d = {1901,1,1};
    if (day < epd_date_day(d) || day > epd_date_day((epd_date_t){2099,12,31})) return (epd_date_t){0};
    int32_t n = day - epd_date_day(d);
    while (n >= 365 + leap(d.year)) { n -= 365 + leap(d.year); ++d.year; }
    while (n >= epd_date_month_days(d.year,d.month)) { n -= epd_date_month_days(d.year,d.month); ++d.month; }
    d.day = (uint8_t)(n+1); return d;
}
int epd_date_weekday(int32_t day) { int w = (day+4)%7; return w < 0 ? w+7 : w; }
int32_t epd_date_utc_day(int64_t utc, int offset)
{
    int64_t shift=offset*INT64_C(60);if((shift>0&&utc>INT64_MAX-shift)||(shift<0&&utc<INT64_MIN-shift))return INT32_MIN;
    int64_t n = utc + shift, day = n / 86400;
    if (n < 0 && n % 86400) --day;
    return day < INT32_MIN || day > INT32_MAX ? INT32_MIN : (int32_t)day;
}
static int32_t spring(int y)
{
    if (y < 1901 || y > 2099) return INT32_MIN;
    uint32_t v = calendar_chinese_table[y-1901];
    return epd_date_day((epd_date_t){(int16_t)y,(uint8_t)((v>>5)&3),(uint8_t)(v&31)});
}
bool epd_date_to_lunar(epd_date_t d, epd_lunar_t *out)
{
    int32_t day = epd_date_day(d);
    if (!out || day == INT32_MIN) return false;
    int y=d.year; if (day < spring(y)) --y;
    if (y < 1901) return false;
    uint32_t v=calendar_chinese_table[y-1901]; int offset=day-spring(y), lm=(v>>20)&15;
    int m=1; bool extra=false;
    for (int i=0; i<12+(lm!=0); ++i) {
        int days=29+((v&(0x80000u>>i))!=0);
        if (offset<days) { *out=(epd_lunar_t){{(int16_t)y,(uint8_t)m,(uint8_t)(offset+1)},extra}; return true; }
        offset-=days;
        if (m==lm && !extra) extra=true; else { extra=false; ++m; }
    }
    return false;
}
bool epd_date_from_lunar(epd_lunar_t l, bool month_end, epd_date_t *out)
{
    int y=l.date.year,m=l.date.month,d=l.date.day;
    if (!out || y<1901 || y>2099 || m<1 || m>12 || d<1 || d>30) return false;
    uint32_t v=calendar_chinese_table[y-1901]; int lm=(v>>20)&15;
    if (l.leap && lm!=m) return false;
    int index=m-1+(lm && (m>lm || (m==lm && l.leap))), offset=0;
    int days=29+((v&(0x80000u>>index))!=0);
    if (d>days) { if (!month_end) return false; d=days; }
    for (int i=0;i<index;++i) offset+=29+((v&(0x80000u>>i))!=0);
    *out=epd_date_from_day(spring(y)+offset+d-1); return epd_date_valid(*out);
}
bool epd_calendar_rule_valid(const epd_calendar_rule_t *r)
{
    if (!r || !epd_date_valid(epd_date_from_day(r->start_day)) || r->until_day<r->start_day ||
        !epd_date_valid(epd_date_from_day(r->until_day)) || r->weekdays>127 || r->minute>=1440 || r->timezone_minutes < -720 || r->timezone_minutes > 840 ||
        r->repeat>EPD_REPEAT_YEARLY || r->excluded_count>EPD_CALENDAR_EXCEPTIONS ||
        (r->repeat==EPD_REPEAT_WEEKLY && !(r->weekdays&127))) return false;
    return !r->lunar_month || (r->lunar_month<=12 && r->lunar_day>=1 && r->lunar_day<=30 && r->repeat==EPD_REPEAT_YEARLY);
}
static bool accepted(const epd_calendar_rule_t *r, int32_t d, int64_t lower, int64_t *utc, int32_t *day)
{
    if (d<r->start_day || d>r->until_day || !epd_date_valid(epd_date_from_day(d))) return false;
    for (unsigned i=0;i<r->excluded_count;++i) if (r->excluded[i]==d) return false;
    int64_t t=(int64_t)d*86400+(r->minute-r->timezone_minutes)*INT64_C(60);
    if (t<lower) return false;
    *utc=t; if (day) *day=d; return true;
}
bool epd_calendar_next(const epd_calendar_rule_t *r, int64_t lower, int64_t *utc, int32_t *day)
{
    if (!utc || !epd_calendar_rule_valid(r)) return false;
    if (r->repeat==EPD_REPEAT_NONE) return accepted(r,r->start_day,lower,utc,day);
    int32_t from=epd_date_utc_day(lower,r->timezone_minutes);
    if (from<r->start_day) from=r->start_day;
    epd_date_t begin=epd_date_from_day(r->start_day), at=epd_date_from_day(from);
    if (!epd_date_valid(at) || from>r->until_day) return false;
    if (r->repeat==EPD_REPEAT_DAILY || r->repeat==EPD_REPEAT_WEEKLY) {
        for (int32_t d=from;d<=r->until_day && d<=from+7*(EPD_CALENDAR_EXCEPTIONS+1);++d)
            if ((r->repeat!=EPD_REPEAT_WEEKLY || (r->weekdays&(1u<<epd_date_weekday(d)))) && accepted(r,d,lower,utc,day)) return true;
    } else if (r->repeat==EPD_REPEAT_MONTHLY) {
        for (int y=at.year,m=at.month;y<=2099;) {
            int n=epd_date_month_days(y,m),d=begin.day;
            if (d>n && r->month_end) d=n;
            if (d<=n) { int32_t candidate=epd_date_day((epd_date_t){(int16_t)y,(uint8_t)m,(uint8_t)d});
                if(candidate>r->until_day)break;
                if(accepted(r,candidate,lower,utc,day))return true; }
            if (++m==13) {m=1;++y;}
        }
    } else {
        for(int y=at.year-(r->lunar_month!=0);y<=2099;++y) {
            epd_date_t candidate={0};
            if(r->lunar_month) {
                epd_lunar_t l={{(int16_t)y,r->lunar_month,r->lunar_day},r->lunar_leap};
                if(!epd_date_from_lunar(l,true,&candidate)) {l.leap=false;if(!epd_date_from_lunar(l,true,&candidate))continue;}
            } else {int d=begin.day,n=epd_date_month_days(y,begin.month);if(d>n&&r->month_end)d=n;
                candidate=(epd_date_t){(int16_t)y,begin.month,(uint8_t)d};}
            int32_t d=epd_date_day(candidate);if(d==INT32_MIN)continue;if(d>r->until_day)break;
            if(accepted(r,d,lower,utc,day))return true;
        }
    }
    return false;
}
bool epd_calendar_last(const epd_calendar_rule_t *r,int64_t lower,int64_t upper,int64_t *utc,int32_t *day)
{
    if(!utc||!epd_calendar_rule_valid(r)||lower>upper)return false;
    int64_t a=(int64_t)r->start_day*86400-86400,b=(int64_t)r->until_day*86400+172800;
    if(lower<a)lower=a;if(upper>b)upper=b;bool found=false;
    while(lower<=upper){int64_t mid=lower+(upper-lower)/2,t;int32_t d;
        if(epd_calendar_next(r,mid,&t,&d)&&t<=upper){*utc=t;if(day)*day=d;found=true;lower=t+1;}
        else upper=mid-1;
    }
    return found;
}
static const char *months[]={"正月","二月","三月","四月","五月","六月","七月","八月","九月","十月","冬月","腊月"};
static const char *days[]={"初一","初二","初三","初四","初五","初六","初七","初八","初九","初十","十一","十二","十三","十四","十五","十六","十七","十八","十九","二十","廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"};
const char *epd_date_label(epd_date_t d, bool festivals, bool lunar)
{
    epd_lunar_t l;bool ok=epd_date_to_lunar(d,&l);
    if(festivals) {
        if(d.month==1&&d.day==1)return "元旦";
        if(d.month==5&&d.day==1)return "劳动节";
        if(d.month==10&&d.day==1)return "国庆节";
        if(ok&&!l.leap) {
            if(l.date.month==1&&l.date.day==1)return "春节";
            if(l.date.month==1&&l.date.day==15)return "元宵";
            if(l.date.month==5&&l.date.day==5)return "端午";
            if(l.date.month==7&&l.date.day==7)return "七夕";
            if(l.date.month==8&&l.date.day==15)return "中秋";
            if(l.date.month==9&&l.date.day==9)return "重阳";
            epd_lunar_t next;
            if(l.date.month==12&&epd_date_to_lunar(epd_date_from_day(epd_date_day(d)+1),&next)&&next.date.month==1&&next.date.day==1)return "除夕";
        }
        const char *term=epd_calendar_term(d); if(term)return term;
    }
    return lunar&&ok ? (l.date.day==1?months[l.date.month-1]:days[l.date.day-1]) : "";
}
bool epd_date_lunar_text(epd_date_t d,char *out,size_t size)
{
    epd_lunar_t l;if(!out||!size)return false;out[0]=0;if(!epd_date_to_lunar(d,&l))return false;
    int n=snprintf(out,size,"%s%s%s",l.leap?"闰":"",months[l.date.month-1],days[l.date.day-1]);return n>=0&&(size_t)n<size;
}
#ifndef EPD_CALENDAR_HOST
#include <rtm.h>
RTM_EXPORT(epd_date_valid); RTM_EXPORT(epd_date_month_days); RTM_EXPORT(epd_date_day);
RTM_EXPORT(epd_date_from_day); RTM_EXPORT(epd_date_weekday); RTM_EXPORT(epd_date_utc_day);
RTM_EXPORT(epd_date_to_lunar); RTM_EXPORT(epd_date_from_lunar); RTM_EXPORT(epd_calendar_rule_valid);
RTM_EXPORT(epd_calendar_next); RTM_EXPORT(epd_calendar_last); RTM_EXPORT(epd_date_label); RTM_EXPORT(epd_date_lunar_text);
#endif
