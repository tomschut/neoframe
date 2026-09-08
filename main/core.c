#include "core.h"
#include <string.h>
#include <ctype.h>
bool nf_pixels_valid(const uint8_t *data, size_t size) {
    if (!data || size != NF_FRAME_SIZE) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned hi = data[i] >> 4, lo = data[i] & 15;
        if (hi > 6 || hi == 4 || lo > 6 || lo == 4) return false;
    }
    return true;
}
size_t nf_row_offset(unsigned controller, unsigned row) {
    return (controller < 2 && row < NF_ROWS) ? row * NF_ROW_BYTES + controller * 300 : SIZE_MAX;
}
bool nf_url_valid(const char *url, bool optional) {
    if (!url || !*url) return optional && url;
    size_t prefix = !strncmp(url, "http://", 7) ? 7 : !strncmp(url, "https://", 8) ? 8 : 0;
    if (!prefix || !url[prefix] || url[prefix] == '/') return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 32 || *p >= 127 || *p == '@' || *p == '#' || *p == '\\') return false;
    return true;
}
bool nf_time_valid(const char *t) {
    return t && strlen(t) == 5 && t[2] == ':' && isdigit((unsigned char)t[0]) &&
        isdigit((unsigned char)t[1]) && isdigit((unsigned char)t[3]) && isdigit((unsigned char)t[4]) &&
        (t[0]-'0')*10 + t[1]-'0' < 24 && (t[3]-'0')*10 + t[4]-'0' < 60;
}

bool nf_schedule_active(unsigned t, unsigned start, unsigned stop) {
    if (start==stop) return true;
    return start<stop ? t>=start && t<stop : t>=start || t<stop;
}
uint32_t nf_schedule_delay(unsigned t, unsigned start, unsigned stop, uint32_t interval) {
    if (!interval) return NF_MIN_INTERVAL;
    unsigned elapsed=(t+NF_DAY-start)%NF_DAY;
    unsigned length=start==stop ? NF_DAY : (stop+NF_DAY-start)%NF_DAY;
    if (elapsed>=length) return (start+NF_DAY-t)%NF_DAY;
    unsigned next=(elapsed/interval+1)*interval;
    return next<length ? next-elapsed : NF_DAY-elapsed;
}
