#define _POSIX_C_SOURCE 200809L
#include "schedule.h"
static unsigned seconds_of(const char *s) { return ((s[0]-'0')*10+s[1]-'0')*3600+((s[3]-'0')*10+s[4]-'0')*60; }
static bool match(const nf_window *w, const struct tm *t, bool slot) {
    unsigned sec=t->tm_hour*3600+t->tm_min*60+t->tm_sec;
    unsigned start=seconds_of(w->start), stop=seconds_of(w->stop);
    if (!nf_schedule_active(sec,start,stop)) return false;
    unsigned day=(t->tm_wday+6)%7;
    if (sec<start) day=(day+6)%7;
    return (w->days & (1U<<day)) && (!slot || (sec+86400-start)%86400%w->interval_s==0);
}
bool nf_schedule_now(const nf_config *c,time_t now) {
    struct tm t; localtime_r(&now,&t);
    if (!c->window_count) return nf_schedule_active(t.tm_hour*3600+t.tm_min*60+t.tm_sec,seconds_of(c->active_start),seconds_of(c->active_end));
    for (unsigned i=0;i<c->window_count;i++) if (match(&c->windows[i],&t,false)) return true;
    return false;
}
uint32_t nf_schedule_next(const nf_config *c,time_t now) {
    if (!c->window_count) {
        struct tm t; localtime_r(&now,&t);
        return nf_schedule_delay(t.tm_hour*3600+t.tm_min*60+t.tm_sec,seconds_of(c->active_start),seconds_of(c->active_end),c->update_interval_s);
    }
    /* Search real time, converting each minute: handles missing/repeated DST hours. */
    time_t next=now+60-(now%60);
    for (;next<=now+8*86400;next+=60) {
        struct tm t; localtime_r(&next,&t);
        for (unsigned i=0;i<c->window_count;i++) if (match(&c->windows[i],&t,true)) return (uint32_t)(next-now);
    }
    return 86400;
}
