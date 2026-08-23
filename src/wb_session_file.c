/* wb_session_file.c — save/load sessions in the .wbus text format.
 * Grammar (matches wb_session_save output):
 *
 *   wbus_session 1.0
 *   bpm 120.000
 *   time_sig 4 4
 *   length 352800.0
 *   track "Lead" kind 0 volume 0.80000 pan 0.00000
 *     insert 0 "comp"
 *     clip 0 start 0.000 length 352800.000
 *       note 60 0.000 44100.000 96
 *     end_clips
 *   end_track
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* G10/G70: access/F_OK for offline-file checks */

#include "wbus.h"

/* G04: read the remainder of the current line (after tokens already consumed)
 * into `out`, stripping the trailing newline. Returns the length, -1 on EOF. */
static int read_line_rest(FILE *f, char *out, size_t sz) {
    int c, i = 0;
    /* skip leading whitespace up to the first non-space on this line */
    do { c = fgetc(f); } while (c == ' ' || c == '\t');
    while (c != EOF && c != '\n' && c != '\r' && i < (int)sz - 1) {
        out[i++] = (char)c; c = fgetc(f);
    }
    out[i] = '\0';
    if (c == '\r') { c = fgetc(f); if (c != '\n') ungetc(c, f); }
    return i > 0 ? i : (c == EOF ? -1 : 0);
}


/* ---- token stream ------------------------------------------------------- */
typedef struct { FILE *f; } tok_s;

static char *next_tok(tok_s *ts) {
    static char buf[512];
    int c;
    do { c = fgetc(ts->f); } while (c==' '||c=='\t'||c=='\n'||c=='\r');
    if (c == EOF) return NULL;
    if (c == '#') { while ((c=fgetc(ts->f))!='\n' && c!=EOF) { /* skip comment */ } return next_tok(ts); }
    if (c == '"') {
        int i = 0;
        while ((c=fgetc(ts->f))!=EOF && c!='"' && i<511) buf[i++] = (char)c;
        buf[i] = '\0';
        return buf;
    }
    int i = 0;
    buf[i++] = (char)c;
    while (i < 511) {
        c = fgetc(ts->f);
        if (c==EOF || c==' '||c=='\t'||c=='\n'||c=='\r') break;
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return buf;
}

/* ---- writer ------------------------------------------------------------- */
int wb_session_save(const wb_session *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "wbus_session 1.0\n");
    fprintf(f, "bpm %.3f\n", s->bpm);
    fprintf(f, "time_sig %d %d\n", s->time_sig_num, s->time_sig_den);
    fprintf(f, "length %.3f\n", s->length);
    for (uint32_t t = 0; t < s->track_count; t++) {
        const wb_track *tk = &s->tracks[t];
        fprintf(f, "track \"%s\" kind %d volume %.5f pan %.5f mute %d solo %d route %d lane %d rec %d\n",
                tk->name, tk->kind, tk->volume, tk->pan, tk->mute, tk->solo, tk->route,
                tk->active_lane, tk->rec_armed);
        for (int i = 0; i < WB_MAX_INSERT_SLOTS; i++)
            if (tk->inserts[i].id[0])
                fprintf(f, "  insert %d \"%s\"\n", i, tk->inserts[i].id);
        for (int i = 0; i < WB_MAX_INSERT_SLOTS; i++)
            if (tk->inserts[i].id[0] && (tk->inserts[i].bypass || tk->inserts[i].wet != 1.0f))
                fprintf(f, "  slot_state %d bypass %d wet %.4f\n", i, tk->inserts[i].bypass, tk->inserts[i].wet);
        /* sidechain routing: only emit when a key source is set */
        for (int i = 0; i < WB_MAX_INSERT_SLOTS; i++)
            if (tk->sidechain[i] >= 0)
                fprintf(f, "  sidechain %d %d\n", i, tk->sidechain[i]);
        /* aux send levels: only emit non-zero sends (sparser than emit-all) */
        int any_send = 0;
        for (uint32_t d = 0; d < WB_MAX_TRACKS; d++)
            if (tk->send[d] > 0.0f) { any_send = 1; break; }
        if (any_send) {
            for (uint32_t d = 0; d < WB_MAX_TRACKS; d++)
                if (tk->send[d] > 0.0f)
                    fprintf(f, "  send %u %.4f\n", d, tk->send[d]);
        }
        for (uint32_t c = 0; c < tk->clip_count; c++) {
            const wb_clip *cl = &tk->clips[c];
            fprintf(f, "  clip %u start %.3f length %.3f gain %.4f lane %d\n", c, cl->start, cl->length, cl->clip_gain, cl->lane);
            for (uint32_t n = 0; n < cl->note_count; n++)
                fprintf(f, "    note %d %.3f %.3f %d\n",
                        cl->notes[n].pitch, cl->notes[n].start, cl->notes[n].dur, cl->notes[n].vel);
            fprintf(f, "  end_clips\n");
        }
        fprintf(f, "end_track\n");
    }
    /* automation lanes */
    for (uint32_t a = 0; a < s->automation_count; a++) {
        const wb_automation_lane *al = s->automation[a];
        fprintf(f, "automation target %d param \"%s\"\n", al->target, al->param);
        for (uint32_t p = 0; p < al->point_count; p++)
            fprintf(f, "  point %.3f %.6f %d\n",
                    al->points[p].time, al->points[p].value, al->points[p].curve);
        fprintf(f, "end_automation\n");
    }
    /* R022: arrangement markers */
    for (uint32_t m = 0; m < s->marker_count; m++)
        fprintf(f, "marker %.3f %d %s\n", s->markers[m].pos, s->markers[m].kind, s->markers[m].label);
    /* G04: media bin (persistent, re-placable) */
    for (uint32_t b = 0; b < s->bin_count; b++)
        fprintf(f, "bin %d %.6f %s\n", s->bin_entries[b].kind, s->bin_entries[b].duration,
                s->bin_entries[b].path);
    fclose(f);
    return 0;
}

/* ---- reader ------------------------------------------------------------- */
/* Grammar addition:
 *   automation target <n> param "<name>"
 *     point <time> <value> <curve>
 *   end_automation
 */
wb_session *wb_session_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    tok_s ts = { .f = f };

    wb_session *s = calloc(1, sizeof(*s));
    if (!s) { fclose(f); return NULL; }
    const char *base = strrchr(path, '/'); base = base ? base+1 : path;
    snprintf(s->name, sizeof(s->name), "%s", base);
    s->bpm = 120.0; s->time_sig_num = 4; s->time_sig_den = 4;
    s->tracks = calloc(WB_MAX_TRACKS, sizeof(wb_track));
    if (!s->tracks) { fclose(f); free(s); return NULL; }

    char *tok;
    while ((tok = next_tok(&ts)) != NULL) {
        if (strcmp(tok,"wbus_session")==0) { next_tok(&ts); continue; }
        if (strcmp(tok,"bpm")==0)          { tok=next_tok(&ts); if(tok) s->bpm=atof(tok); continue; }
        if (strcmp(tok,"time_sig")==0)     { tok=next_tok(&ts); s->time_sig_num=tok?atoi(tok):4;
                                             tok=next_tok(&ts); s->time_sig_den=tok?atoi(tok):4; continue; }
        if (strcmp(tok,"length")==0)       { tok=next_tok(&ts); if(tok) s->length=atof(tok); continue; }

        if (strcmp(tok,"track")==0) {;
            if (s->track_count >= WB_MAX_TRACKS) { fclose(f); return s; }
            wb_track *tk = &s->tracks[s->track_count];
            memset(tk, 0, sizeof(*tk));
            tk->volume = 1.0f;
            tk->route = -1;   /* default: route to master, not a bus */
            tok = next_tok(&ts); /* name */
            if (tok) strncpy(tk->name, tok, sizeof(tk->name)-1);

            /* parse track attributes / inserts / clips until end_track */
            while ((tok = next_tok(&ts)) != NULL) {
                if (strcmp(tok,"end_track")==0) break;
                else if (strcmp(tok,"kind")==0)    { tok=next_tok(&ts); if(tok) tk->kind=atoi(tok); }
                else if (strcmp(tok,"volume")==0)  { tok=next_tok(&ts); if(tok) tk->volume=(float)atof(tok); }
                else if (strcmp(tok,"pan")==0)     { tok=next_tok(&ts); if(tok) tk->pan=(float)atof(tok); }
                else if (strcmp(tok,"mute")==0)    { tok=next_tok(&ts); if(tok) tk->mute=atoi(tok); }
                else if (strcmp(tok,"solo")==0)    { tok=next_tok(&ts); if(tok) tk->solo=atoi(tok); }
                else if (strcmp(tok,"route")==0)   { tok=next_tok(&ts); if(tok) tk->route=atoi(tok); }
                else if (strcmp(tok,"lane")==0)    { tok=next_tok(&ts); if(tok) tk->active_lane=atoi(tok); }
                else if (strcmp(tok,"rec")==0)     { tok=next_tok(&ts); if(tok) tk->rec_armed=atoi(tok); }  /* G09 */
                else if (strcmp(tok,"insert")==0) {
                    tok=next_tok(&ts); int slot = tok?atoi(tok):0;
                    tok=next_tok(&ts); if (tok && slot>=0 && slot<WB_MAX_INSERT_SLOTS)
                        strncpy(tk->inserts[slot].id, tok, 63);
                }
                else if (strcmp(tok,"slot_state")==0) {
                    tok=next_tok(&ts); int slot = tok?atoi(tok):0;
                    if (slot>=0 && slot<WB_MAX_INSERT_SLOTS) {
                        tok=next_tok(&ts); if(tok) tk->inserts[slot].bypass = atoi(tok);
                        tok=next_tok(&ts); if(tok) tk->inserts[slot].wet = (float)atof(tok);
                    }
                }
                else if (strcmp(tok,"send")==0) {
                    tok=next_tok(&ts); uint32_t dst = tok?atoi(tok):0;
                    tok=next_tok(&ts); if(tok && dst<WB_MAX_TRACKS) tk->send[dst] = (float)atof(tok);
                }
                else if (strcmp(tok,"sidechain")==0) {
                    tok=next_tok(&ts); int slot = tok?atoi(tok):0;
                    tok=next_tok(&ts); if (tok && slot>=0 && slot<WB_MAX_INSERT_SLOTS)
                        tk->sidechain[slot] = atoi(tok);
                }
                else if (strcmp(tok,"clip")==0) {;
                    next_tok(&ts); /* clip index */
                    tok=next_tok(&ts); /* "start" */
                    double start = 0, length = 0;
                    if (tok && strcmp(tok,"start")==0) {
                        tok=next_tok(&ts); start = tok?atof(tok):0;
                        tok=next_tok(&ts); /* "length" */
                        tok=next_tok(&ts); length = tok?atof(tok):0;
                    } else if (tok) { length = atof(tok); }
                    tk->clip_count++;
                    tk->clips = realloc(tk->clips, tk->clip_count * sizeof(wb_clip));
                    wb_clip *cl = &tk->clips[tk->clip_count-1];
                    memset(cl, 0, sizeof(*cl));
                    cl->type = 0; cl->start = start; cl->length = length;
                    /* parse notes until end_clips */
                    while ((tok = next_tok(&ts)) != NULL) {
                        if (strcmp(tok,"end_clips")==0) break;
                        if (strcmp(tok,"gain")==0) {
                            tok=next_tok(&ts); if(tok) cl->clip_gain=(float)atof(tok);
                        }
                        else if (strcmp(tok,"lane")==0) {
                            tok=next_tok(&ts); if(tok) cl->lane=atoi(tok);
                        }
                        else if (strcmp(tok,"note")==0) {
                            wb_note no = {0,0,0,100};
                            tok=next_tok(&ts); no.pitch = tok?(uint8_t)atoi(tok):0;
                            tok=next_tok(&ts); no.start = tok?atof(tok):0;
                            tok=next_tok(&ts); no.dur  = tok?atof(tok):0;
                            tok=next_tok(&ts); no.vel  = tok?(uint8_t)atoi(tok):0;;
                            cl->note_count++;
                            cl->notes = realloc(cl->notes, cl->note_count*sizeof(wb_note));
                            cl->notes[cl->note_count-1] = no;
                        }
                    }
                }
                else { /* unknown/blank token — ignore one */ }
            }
            s->track_count++;
            continue;
        }
        if (strcmp(tok,"automation")==0) {
            int target = -1;
            char param[64] = {0};
            tok=next_tok(&ts);                        /* keyword "target" */
            tok=next_tok(&ts); if(tok) target=atoi(tok);
            tok=next_tok(&ts);                        /* keyword "param" */
            tok=next_tok(&ts); if(tok) strncpy(param, tok, sizeof(param)-1);
            wb_automation_lane *al = wb_session_add_automation(s, param, target);
            if (!al) continue;
            while ((tok = next_tok(&ts)) != NULL) {
                if (strcmp(tok,"end_automation")==0) break;
                if (strcmp(tok,"point")==0) {
                    tok=next_tok(&ts); double t = tok?atof(tok):0;
                    tok=next_tok(&ts); double v = tok?atof(tok):0;
                    tok=next_tok(&ts); int c = tok?atoi(tok):0;
                    wb_automation_add_point(al, t, v, c);
                }
            }
            continue;
        }
        if (strcmp(tok,"marker")==0) {
            tok=next_tok(&ts); double mpos = tok?atof(tok):0;
            tok=next_tok(&ts); int mkind = tok?atoi(tok):0;
            tok=next_tok(&ts); /* label (may contain spaces) */
            char mlabel[32] = {0};
            if (tok) strncpy(mlabel, tok, sizeof(mlabel)-1);
            wb_session_add_marker(s, mpos, mlabel, mkind);
            continue;
        }
        /* G04: media bin entries ("bin <kind> <dur> <path>"). The path is the
         * rest of the line so it may contain spaces. The bin is also populated
         * implicitly by imports; on load we still parse persisted entries. */
        if (strcmp(tok,"bin")==0) {
            tok=next_tok(&ts); int bkind = tok?atoi(tok):0;
            tok=next_tok(&ts); double bdur = tok?atof(tok):0.0;
            char bpath[1024] = {0};
            if (read_line_rest(ts.f, bpath, sizeof(bpath)) <= 0) {
                /* line had no path remainder: ignore silently */
            } else {
                char nm[256] = {0};
                const char *sp = strrchr(bpath, '/');
                const char *base = sp ? sp+1 : bpath;
                snprintf(nm, sizeof(nm), "%s", base);
                if (s->bin_count < WB_MAX_BIN) {
                    wb_bin_entry *e = &s->bin_entries[s->bin_count++];
                    snprintf(e->path, sizeof(e->path), "%s", bpath);
                    snprintf(e->name, sizeof(e->name), "%s", nm);
                    e->kind = (bkind != 0) ? 1 : 0;
                    e->duration = bdur;
                    e->offline = (access(bpath, F_OK) != 0) ? 1 : 0;  /* G70 */
                }
            }
            continue;
        }
        /* unknown top-level token: skip its value if it follows a key */
    }
    /* G70: once fully loaded, recompute offline state from disk reality. */
    wb_session_update_offline(s);
    fclose(f);
    return s;
}
