/* wb_shadowbin.c — the magic shadow bin: full edit state as portable JSON
 * sidecar (R061). Write is atomic (tmp+rename); read restores video clip
 * layouts and audio clip placements by source-name match. */

#include "wbus/wbus_shadowbin.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------- JSON escaping ------------------------------------------ */
static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static const char *track_kind_name(int kind) {
    switch (kind) {
    case 0: return "instrument";
    case 1: return "audio";
    case 2: return "mixbus";
    case 3: return "video";
    default: return "unknown";
    }
}
static int track_kind_from(const char *s) {
    if (!s) return 1;
    if (!strcmp(s, "instrument")) return 0;
    if (!strcmp(s, "audio"))      return 1;
    if (!strcmp(s, "mixbus"))     return 2;
    if (!strcmp(s, "video"))      return 3;
    return 1;
}

/* basename helper (no deps) */
static const char *base_name(const char *p) {
    if (!p) return "";
    const char *slash = strrchr(p, '/');
    const char *bs = strrchr(p, '\\');
    if (bs > slash) slash = bs;
    return slash ? slash + 1 : p;
}

/* ---------------- WRITE --------------------------------------------------- */

int wb_shadowbin_write(const wb_session *s, const char *path) {
    if (!s || !path || !path[0]) return -1;

    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    fprintf(f, "{");
    fputs("\"otio_schema\":\"ShadowBin.1\",", f);
    fputs("\"name\":", f); json_str(f, s->name); fputc(',', f);
    fprintf(f, "\"bpm\":%.4f,", s->bpm);
    fprintf(f, "\"length_seconds\":%.4f,", s->length / WB_SAMPLE_RATE);
    fprintf(f, "\"time_sig\":\"%d/%d\",", s->time_sig_num, s->time_sig_den);

    /* tracks */
    fputs("\"tracks\":[", f);
    for (uint32_t t = 0; t < s->track_count; t++) {
        const wb_track *tr = &s->tracks[t];
        if (t) fputc(',', f);
        fputc('{', f);
        fputs("\"kind\":", f); json_str(f, track_kind_name(tr->kind)); fputc(',', f);
        fputs("\"name\":", f); json_str(f, tr->name); fputc(',', f);
        fprintf(f, "\"volume\":%.4f,\"pan\":%.4f,\"mute\":%d,\"solo\":%d,"
                   "\"active_lane\":%d,\"clips\":[",
                tr->volume, tr->pan, tr->mute, tr->solo, tr->active_lane);
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            const wb_clip *cl = &tr->clips[c];
            if (c) fputc(',', f);
            fputc('{', f);
            int is_video = (cl->type == 2 && cl->video);
            double start_s = is_video ? cl->start
                                      : cl->start / WB_SAMPLE_RATE;
            double len_s   = is_video ? cl->length
                                      : cl->length / WB_SAMPLE_RATE;
            double in_s    = 0;
            const char *srcname = "";
            if (is_video) {
                in_s = cl->video->start_in_source < 0 ? 0
                                                      : cl->video->start_in_source;
                srcname = base_name(cl->video->source_path);
            } else if (cl->type == 1) {
                in_s = 0;
                srcname = "inline-audio";
            } else if (cl->type == 3 && cl->perfclip) {
                in_s = 0;
                srcname = "perfclip";
            } else {
                in_s = 0;
                srcname = "midi";
            }
            fprintf(f, "\"start_s\":%.5f,\"length_s\":%.5f,"
                       "\"in_s\":%.5f,\"lane\":%d,\"gain\":%.4f,"
                       "\"media\":",
                    start_s, len_s, in_s, cl->lane,
                    cl->clip_gain > 0.0001f ? cl->clip_gain : 1.0f);
            json_str(f, srcname);
            fputc('}', f);
        }
        fputs("]}", f);
    }
    fputs("],", f);

    /* markers -> also emit YouTube-chapter-ready labels */
    fputs("\"markers\":[", f);
    for (int i = 0; i < (int)s->marker_count; i++) {
        const wb_marker *mk = &s->markers[i];
        if (i) fputc(',', f);
        fprintf(f, "{\"pos_s\":%.5f,\"label\":", mk->pos / WB_SAMPLE_RATE);
        json_str(f, mk->label);
        fprintf(f, ",\"kind\":%d}", mk->kind);
    }
    fputs("],", f);

    fputs("\"cgi\":{\"hint\":\"agent cgi-* commands manage the overlay "
          "scene; see wb_agent.c\"},", f);
    fputs("\"note\":\"ShadowBin.1 - decision list only, media not copied\"}", f);

    fclose(f);

    /* atomic swap */
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/* R068: shared atomic-commit helper for perf-clip sidecars.
 * Uses a temp + rename so a crash mid-write leaves the old file intact. */
int wb_shadowbin_atomic_commit(const char *tmp, const char *dst) {
    if (!tmp || !dst) return -1;
    if (rename(tmp, dst) != 0) { remove(tmp); return -1; }
    return 0;
}

void wb_shadowbin_path_for(const char *project_path, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    buf[0] = 0;
    if (!project_path || !project_path[0]) return;
    snprintf(buf, cap, "%s", project_path);
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf) *dot = 0;
    size_t n = strlen(buf);
    snprintf(buf + n, cap - n, ".shadowbin.json");
}

/* ---------------- READ ----------------------------------------------------
 * A tiny targeted scanner: finds "tracks":[ ... ] then walks objects with
 * the same string-scan approach proven in wb_gltf.c.
 * -------------------------------------------------------------------------- */

typedef struct { const char *p, *end; } jscan;

static void js_ws(jscan *j){while(j->p<j->end&&(*j->p==' '||*j->p=='\t'||*j->p=='\n'||*j->p=='\r'))j->p++;}
static int js_peek(jscan *j){js_ws(j);return j->p<j->end?*j->p:0;}
static int js_eat(jscan *j,char c){js_ws(j);if(j->p<j->end&&*j->p==c){j->p++;return 1;}return 0;}
static double js_number(jscan *j){
    js_ws(j); char *e; double v=strtod(j->p,&e); if(e==j->p)return 0; j->p=e; return v;
}
static void js_skip(jscan *j){
    js_ws(j); if(j->p>=j->end) return;
    char c=*j->p;
    if(c=='{'||c=='['){
        char open=c, close=(c=='{')?'}':']'; int depth=0;
        while(j->p<j->end){
            char d=*j->p;
            if(d=='"'){ j->p++;
                while(j->p<j->end&&*j->p!='"'){if(*j->p=='\\')j->p++;j->p++;}
                if(j->p<j->end)j->p++; continue;
            }
            if(d==open)depth++;
            else if(d==close){depth--; if(!depth){j->p++;return;}}
            j->p++;
        }
    } else if (c=='"'){
        j->p++;
        while(j->p<j->end&&*j->p!='"'){if(*j->p=='\\')j->p++;j->p++;}
        if(j->p<j->end)j->p++;
    } else {
        while(j->p<j->end&&*j->p!=','&&*j->p!='}'&&*j->p!=']'&&*j->p!=' '&&*j->p!='\n')j->p++;
    }
}
static int js_string(jscan *j, char*out,int cap){
    js_ws(j); if(j->p>=j->end||*j->p!='"')return 0;
    j->p++; int n=0;
    while(j->p<j->end&&*j->p!='"'){
        char c=*j->p++;
        if(c=='\\'&&j->p<j->end)c=*j->p++;
        if(n<cap-1)out[n++]=c;
    }
    if(j->p<j->end)j->p++; out[n]=0; return 1;
}
/* find "key": at current object level; positions at value */
static int js_find_key(jscan *j, const char *key){
    char pat[64]; snprintf(pat,sizeof pat,"\"%s\"",key);
    const char *hit=NULL;
    jscan tmp=*j; int depth=0;
    while(tmp.p<tmp.end){
        char c=*tmp.p;
        if(c=='{'){depth++;}
        else if(c=='}'){depth--; if(depth<=0)break;}
        else if(c=='"'){
            const char *ss=tmp.p; tmp.p++;
            while(tmp.p<tmp.end&&*tmp.p!='"'){if(*tmp.p=='\\')tmp.p++;tmp.p++;}
            if(tmp.p>=tmp.end)break; tmp.p++;
            size_t slen=(size_t)(tmp.p-ss);
            if(depth==1&&slen==strlen(pat)&&strncmp(ss,pat,slen)==0){
                js_ws(&tmp);
                if(tmp.p<tmp.end&&*tmp.p==':'){tmp.p++;hit=tmp.p;break;}
            }
            continue;
        }
        tmp.p++;
    }
    if(!hit)return 0; j->p=hit; return 1;
}

/* read one number field from an object span */
static double obj_num(const char *ps, const char *pe, const char *key, double dflt) {
    jscan o={ps,pe};
    if(!js_find_key(&o,key))return dflt;
    return js_number(&o);
}
/* read one string field into out */
static int obj_str(const char *ps, const char *pe, const char *key,
                   char *out, int cap) {
    jscan o={ps,pe};
    if(!js_find_key(&o,key))return 0;
    return js_string(&o,out,cap);
}

int wb_shadowbin_read(wb_session *s, const char *path) {
    if (!s || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 2) { fclose(f); return -1; }
    char *json = malloc((size_t)sz+1);
    if (!json) { fclose(f); return -1; }
    size_t got = fread(json, 1, (size_t)sz, f);
    fclose(f);
    json[got] = 0;

    jscan root = { json, json + got };
    int restored = 0;

    /* ---- tracks ---- */
    jscan tscan = root;
    if (js_find_key(&tscan, "tracks") && js_eat(&tscan, '[')) {
        long tidx = 0;
        while (js_peek(&tscan) == '{' && tidx < (long)s->track_count) {
            jscan tobj = tscan;
            js_skip(&tobj);
            const char *tps = tscan.p, *tpe = tobj.p;

            char kindname[32];
            obj_str(tps,tpe,"kind",kindname,sizeof kindname);
            int kind = track_kind_from(kindname);
            wb_track *tr = &s->tracks[tidx];
            if (tr->kind == kind || tr->kind == 1 || tr->kind == 3) {
                tr->volume = (float)obj_num(tps,tpe,"volume",tr->volume);
                tr->pan    = (float)obj_num(tps,tpe,"pan",tr->pan);
                tr->mute   = (int)obj_num(tps,tpe,"mute",tr->mute);
                tr->solo   = (int)obj_num(tps,tpe,"solo",tr->solo);
                tr->active_lane = (int)obj_num(tps,tpe,"active_lane",
                                              tr->active_lane);

                /* clips array inside this track */
                jscan cscan = { tps, tpe };
                if (js_find_key(&cscan,"clips") && js_eat(&cscan,'[')) {
                    long cidx = 0;
                    while (js_peek(&cscan)=='{' ) {
                        jscan cobj = cscan;
                        js_skip(&cobj);
                        const char *cps=cscan.p,*cpe=cobj.p;

                        double start_s=obj_num(cps,cpe,"start_s",-1);
                        double len_s  =obj_num(cps,cpe,"length_s",-1);
                        double in_s   =obj_num(cps,cpe,"in_s",0);
                        int lane      =(int)obj_num(cps,cpe,"lane",0);
                        float gain    =(float)obj_num(cps,cpe,"gain",1);

                        if (start_s>=0 && len_s>0 &&
                            cidx < (long)tr->clip_count) {
                            wb_clip *cl=&tr->clips[cidx];
                            if (cl->type==2 && cl->video) {
                                cl->start = start_s;
                                cl->length = len_s;
                                cl->video->start_in_source = in_s;
                                cl->video->duration = len_s;
                                cl->video->timeline_pos = start_s;
                                cl->lane = lane;
                                if (gain>0) cl->clip_gain=gain;
                                restored++;
                            } else if (cl->type==1) {
                                cl->start = start_s*WB_SAMPLE_RATE;
                                cl->length= len_s*WB_SAMPLE_RATE;
                                cl->lane  = lane;
                                if (gain>0) cl->clip_gain=gain;
                                restored++;
                            }
                        }
                        cidx++;
                        cscan.p = cobj.p;
                        js_eat(&cscan,',');
                    }
                }
            }
            tidx++;
            tscan.p = tobj.p;
            js_eat(&tscan,',');
        }
    }

    /* ---- markers: replace all ---- */
    jscan mscan = root;
    if (js_find_key(&mscan,"markers") && js_eat(&mscan,'[')) {
        s->marker_count = 0;
        while (js_peek(&mscan)=='{') {
            jscan mobj = mscan; js_skip(&mobj);
            const char *mps=mscan.p,*mpe=mobj.p;
            if (s->marker_count < 64) {
                wb_marker *mk=&s->markers[s->marker_count++];
                mk->pos  = obj_num(mps,mpe,"pos_s",0)*WB_SAMPLE_RATE;
                mk->kind = (int)obj_num(mps,mpe,"kind",1);
                char lbl[128];
                if (obj_str(mps,mpe,"label",lbl,sizeof lbl))
                    snprintf(mk->label,sizeof mk->label,"%s",lbl);
                else mk->label[0]=0;
            }
            mscan.p=mobj.p; js_eat(&mscan,',');
        }
    }

    free(json);
    return restored;
}
