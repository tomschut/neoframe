#include "formutil.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
void nf_html_escape(const char *in, char *out, size_t cap) {
    size_t o=0;
    for (size_t i=0; in[i] && o+6<cap; ++i) {
        if (in[i]=='"') { memcpy(out+o,"&quot;",6); o+=6; }
        else if (in[i]=='<') { memcpy(out+o,"&lt;",4); o+=4; }
        else if (in[i]=='&') { memcpy(out+o,"&amp;",5); o+=5; }
        else out[o++]=in[i];
    }
    out[o]=0;
}
void nf_url_decode(const char *in, size_t n, char *out, size_t cap) {
    size_t o=0;
    for (size_t i=0; i<n && o+1<cap; ++i) {
        if (in[i]=='+') out[o++]=' ';
        else if (in[i]=='%' && i+2<n && isxdigit((unsigned char)in[i+1]) && isxdigit((unsigned char)in[i+2])) {
            char hex[3]={in[i+1],in[i+2],0}; out[o++]=(char)strtol(hex,NULL,16); i+=2;
        } else out[o++]=in[i];
    }
    out[o]=0;
}
void nf_form_field(const char *body, const char *key, char *out, size_t cap) {
    *out=0;
    size_t klen=strlen(key);
    for (const char *p=body; *p; ) {
        const char *amp=strchr(p,'&');
        size_t seglen=amp?(size_t)(amp-p):strlen(p);
        if (seglen>klen && p[klen]=='=' && !strncmp(p,key,klen))
            nf_url_decode(p+klen+1,seglen-klen-1,out,cap);
        p+=seglen; if (*p=='&') ++p; else break;
    }
}
void nf_json_escape(const char *in, char *out, size_t cap) {
    size_t o=0;
    for (size_t i=0; in[i] && o+2<cap; ++i) {
        unsigned char c=(unsigned char)in[i];
        if (c=='"' || c=='\\') { out[o++]='\\'; out[o++]=(char)c; }
        else if (c>=0x20) out[o++]=(char)c;
    }
    out[o]=0;
}
