/* Caelum extension channel: wire format shared by Caelum (pso_recomp/runtime/caelum_ext.h) and Teaserv
 * (src/CaelumExt.h is a byte-for-byte copy; keep them the same). Plain C99, also valid C++; no allocation.
 *
 * Command F2, flags 0x20-0x2F (flags 1-5 are the modpack handshake, 0x10-0x13 voice chat):
 *   0x20 HELLO     client -> server, once, right after the client's F2 flag 2 ("mods ready")
 *   0x21 WELCOME   server -> client, the answer to HELLO
 *   0x22 SNAPSHOT  server -> client: every extension object and the music of the client's current room
 *   0x23           reserved (changes; today every change is a new SNAPSHOT)
 *   0x24 EVENT     client -> server: small reports (an asset loaded or failed)
 * The server sends 0x21-0x23 only to a client that sent HELLO: a Caelum from before this channel ends the session
 * on an unknown F2 flag, and a Teaserv from before it ignores unknown client flags.
 *
 * Bodies are text, lines end with '\n', zero padded to 4 bytes like every game packet.
 *   HELLO / WELCOME:  "v=1\ncaps=<name>,<name>\n"; a client's HELLO adds "objects=<n>\n", the most objects per room it
 *                     can show (CEXT_LEGACY_OBJECTS when the line is missing: clients from before it hold 16). The
 *                     server never sends a client more objects than that.
 *   SNAPSHOT:         "v=1\nroom=<L or G><lobby id>\nnow=<server Unix ms>\n" then zero or more lines
 *                     "obj <id> model=<file> [part=<name>] [tex=<file>] [motion=<name>] x= y= z= ry=<degrees> scale=
 *                      start=<Unix ms> loop=<0|1> show=<0|1>"   (one line each, fields separated by one space)
 *                     model is a BML archive; part names the model inside it (default: its first model) and motion
 *                     the motion inside it (default: none). tex is reserved (a BML carries its textures).
 *                     Optional on any object: rx= rz= (degrees, turned after ry). Attached objects (sent only to a
 *                     client whose HELLO named "scene.attach"): attach=<lobby slot 0-11 of the player> and
 *                     bone=<skeleton node 0-63, -1 = the player's root; 58 is the head>; x/y/z/ry/rx/rz are then in
 *                     that node's frame (root: the player's position and facing); with upright=1 the node only gives
 *                     the place and x/y/z/ry are in the player's frame (a hat that stays level). follow=1 (root only): the object
 *                     trails the player, turning the way it moves (a pet); move=<motion> is its motion while it moves.
 *                     "music <file> start=<Unix ms> loop=<0|1>"   (at most one)
 *   EVENT:            "v=1\nroom=<room>\nasset <id> ok=<0|1>\n"
 * <id> is "<package>:<key>". File names are base names: 1-47 characters of A-Z a-z 0-9 _ . - (no folders). Unknown
 * fields on an "obj" or "music" line and unknown lines are skipped, so later versions can add to them. */
#ifndef CAELUM_EXT_SHARED_H
#define CAELUM_EXT_SHARED_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define CEXT_FLAG_HELLO 0x20
#define CEXT_FLAG_WELCOME 0x21
#define CEXT_FLAG_SNAPSHOT 0x22
#define CEXT_FLAG_RESERVED 0x23
#define CEXT_FLAG_EVENT 0x24
#define CEXT_FLAG_FIRST 0x20
#define CEXT_FLAG_LAST 0x2F

#define CEXT_VERSION 1
#define CEXT_MAX_OBJECTS 64      /* per room. 64 of the longest possible "obj" lines (about 440 bytes each), the header
                                    and a music line fit in CEXT_MAX_SNAPSHOT (checked by tools/caelum_ext_check.c) */
#define CEXT_LEGACY_OBJECTS 16   /* per room, for a client whose HELLO has no "objects=" line */
#define CEXT_MAX_ID 100          /* "<package>:<key>", with the terminator */
#define CEXT_MAX_FILE 48         /* with the terminator */
#define CEXT_MAX_ROOM 24
#define CEXT_MAX_SNAPSHOT 0x7000 /* body bytes, server -> client */
#define CEXT_MAX_CLIENT_BODY 0x400 /* body bytes, client -> server */

/* capability bits (HELLO "caps=" names) */
#define CEXT_CAP_MODEL 1u  /* "scene.model": draws models and motions that are not placed by the lobby's SET file */
#define CEXT_CAP_MUSIC 2u  /* "audio.music": plays and seeks room music */
#define CEXT_CAP_ATTACH 4u /* "scene.attach": draws objects attached to a player (attach=, bone=, follow=) */
#define CEXT_MAX_BONE 63
#define CEXT_HEAD_BONE 58  /* the head node of the 64-node player skeleton (the game draws head parts there) */

typedef struct {
    char id[CEXT_MAX_ID];
    char model[CEXT_MAX_FILE], part[CEXT_MAX_FILE], tex[CEXT_MAX_FILE], motion[CEXT_MAX_FILE];
    float x, y, z, ry, scale;
    long long start_ms;
    int loop, show;
    float rx, rz;                 /* degrees, after ry */
    int attach;                   /* lobby slot of the player it is attached to, -1 = placed in the room */
    int bone;                     /* attached: skeleton node, -1 = the player's root */
    int follow;                   /* attached to the root: trails the player */
    int upright;                  /* attached to a bone: the bone's place, the player's facing */
    char move[CEXT_MAX_FILE];     /* follow: the motion while moving ("" = motion) */
} CextObject;

typedef struct {
    int version;
    char room[CEXT_MAX_ROOM];
    long long now_ms;
    unsigned count;
    CextObject objects[CEXT_MAX_OBJECTS];
    int has_music;
    char music[CEXT_MAX_FILE];
    long long music_start_ms;
    int music_loop;
} CextSnapshot;

static inline int cext_valid_file(const char* s) {
    size_t n = strlen(s);
    if (n < 1 || n >= CEXT_MAX_FILE || s[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return 0;
    }
    return 1;
}

/* an id or room: printable ASCII without spaces or '=' */
static inline int cext_valid_token(const char* s, size_t cap) {
    size_t n = strlen(s);
    if (n < 1 || n >= cap) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] <= ' ' || s[i] >= 0x7F || s[i] == '=') return 0;
    return 1;
}

/* Copies the next token of [*p, end) (up to `stop` or a space) into out; 0 when it does not fit. */
static inline int cext_token(const char** p, const char* end, char stop, char* out, size_t cap) {
    size_t n = 0;
    while (*p < end && **p != ' ' && **p != stop) {
        if (n + 1 >= cap) return 0;
        out[n++] = *(*p)++;
    }
    out[n] = 0;
    return 1;
}

static inline int cext_number(const char* s, long long* out) {
    char* e = NULL;
    if (!*s) return 0;
    *out = strtoll(s, &e, 10);
    return e && !*e;
}

static inline int cext_float(const char* s, float* out) {
    char* e = NULL;
    if (!*s) return 0;
    const double v = strtod(s, &e);
    if (!e || *e || !(v > -1.0e7 && v < 1.0e7)) return 0;   /* also refuses NaN */
    *out = (float)v;
    return 1;
}

/* "caps=a,b" names -> bits; unknown names are ignored */
static inline unsigned cext_parse_caps(const char* text, size_t size) {
    unsigned caps = 0, version = 0;
    const char* p = text;
    const char* const end = text + size;
    while (p < end && *p) {
        const char* line = p;
        while (p < end && *p && *p != '\n') p++;
        const size_t n = (size_t)(p - line);
        if (p < end && *p == '\n') p++;
        if (n == 3 && !memcmp(line, "v=1", 3)) version = 1;
        if (n > 5 && !memcmp(line, "caps=", 5)) {
            const char* q = line + 5;
            const char* const le = line + n;
            while (q < le) {
                const char* name = q;
                while (q < le && *q != ',') q++;
                const size_t m = (size_t)(q - name);
                if (m == 11 && !memcmp(name, "scene.model", 11)) caps |= CEXT_CAP_MODEL;
                if (m == 11 && !memcmp(name, "audio.music", 11)) caps |= CEXT_CAP_MUSIC;
                if (m == 12 && !memcmp(name, "scene.attach", 12)) caps |= CEXT_CAP_ATTACH;
                if (q < le) q++;
            }
        }
    }
    return version ? caps : 0;
}

/* HELLO "objects=<n>" -> the most objects per room the client can show (1..CEXT_MAX_OBJECTS; more is read as the
 * maximum), or CEXT_LEGACY_OBJECTS when the line is missing or not a number. */
static inline unsigned cext_parse_objects(const char* text, size_t size) {
    const char* p = text;
    const char* const end = text + size;
    while (p < end && *p) {
        const char* line = p;
        while (p < end && *p && *p != '\n') p++;
        const size_t n = (size_t)(p - line);
        if (p < end && *p == '\n') p++;
        if (n > 8 && n < 20 && !memcmp(line, "objects=", 8)) {
            char num[16];
            long long value = 0;
            memcpy(num, line + 8, n - 8);
            num[n - 8] = 0;
            if (!cext_number(num, &value) || value < 1) return CEXT_LEGACY_OBJECTS;
            return value > CEXT_MAX_OBJECTS ? CEXT_MAX_OBJECTS : (unsigned)value;
        }
    }
    return CEXT_LEGACY_OBJECTS;
}

/* Parses a SNAPSHOT body. Returns 1 and fills *out, or 0 (nothing in *out may be used) when the text is not a valid
 * version 1 snapshot. A bad "obj" or "music" line makes the whole snapshot invalid: a half-applied room is worse than
 * keeping the previous one. */
static inline int cext_parse_snapshot(const char* text, size_t size, CextSnapshot* out) {
    const char* p = text;
    const char* const end = text + size;
    int have_room = 0, have_now = 0;
    memset(out, 0, sizeof *out);
    if (size > CEXT_MAX_SNAPSHOT) return 0;
    while (p < end && *p) {
        const char* line = p;
        while (p < end && *p && *p != '\n') p++;
        const char* const le = p;
        if (p < end && *p == '\n') p++;
        const size_t n = (size_t)(le - line);
        if (!n) continue;
        if (n == 3 && !memcmp(line, "v=1", 3)) {
            out->version = 1;
        } else if (n > 5 && !memcmp(line, "room=", 5)) {
            const char* q = line + 5;
            if (!cext_token(&q, le, 0, out->room, sizeof out->room) || q != le || !cext_valid_token(out->room, sizeof out->room)) return 0;
            have_room = 1;
        } else if (n > 4 && !memcmp(line, "now=", 4)) {
            char num[24];
            const char* q = line + 4;
            if (!cext_token(&q, le, 0, num, sizeof num) || q != le || !cext_number(num, &out->now_ms)) return 0;
            have_now = 1;
        } else if (n > 4 && !memcmp(line, "obj ", 4)) {
            CextObject o;
            const char* q = line + 4;
            int have_model = 0;
            if (out->count >= CEXT_MAX_OBJECTS) return 0;
            memset(&o, 0, sizeof o);
            o.scale = 1.0f;
            o.loop = 1;
            o.show = 1;
            o.attach = -1;
            o.bone = -1;
            if (!cext_token(&q, le, 0, o.id, sizeof o.id) || !cext_valid_token(o.id, sizeof o.id)) return 0;
            for (unsigned i = 0; i < out->count; i++)
                if (!strcmp(out->objects[i].id, o.id)) return 0;
            while (q < le) {
                char key[16], value[CEXT_MAX_FILE];
                long long number = 0;
                if (*q != ' ') return 0;
                q++;
                if (!cext_token(&q, le, '=', key, sizeof key) || q >= le || *q != '=') return 0;
                q++;
                if (!cext_token(&q, le, 0, value, sizeof value)) return 0;
                if (!strcmp(key, "model")) { if (!cext_valid_file(value)) return 0; strcpy(o.model, value); have_model = 1; }
                else if (!strcmp(key, "part")) { if (!cext_valid_file(value)) return 0; strcpy(o.part, value); }
                else if (!strcmp(key, "tex")) { if (!cext_valid_file(value)) return 0; strcpy(o.tex, value); }
                else if (!strcmp(key, "motion")) { if (!cext_valid_file(value)) return 0; strcpy(o.motion, value); }
                else if (!strcmp(key, "x")) { if (!cext_float(value, &o.x)) return 0; }
                else if (!strcmp(key, "y")) { if (!cext_float(value, &o.y)) return 0; }
                else if (!strcmp(key, "z")) { if (!cext_float(value, &o.z)) return 0; }
                else if (!strcmp(key, "ry")) { if (!cext_float(value, &o.ry)) return 0; }
                else if (!strcmp(key, "scale")) { if (!cext_float(value, &o.scale) || !(o.scale > 0.0f)) return 0; }
                else if (!strcmp(key, "start")) { if (!cext_number(value, &o.start_ms)) return 0; }
                else if (!strcmp(key, "loop")) { if (!cext_number(value, &number)) return 0; o.loop = number != 0; }
                else if (!strcmp(key, "show")) { if (!cext_number(value, &number)) return 0; o.show = number != 0; }
                else if (!strcmp(key, "rx")) { if (!cext_float(value, &o.rx)) return 0; }
                else if (!strcmp(key, "rz")) { if (!cext_float(value, &o.rz)) return 0; }
                else if (!strcmp(key, "attach")) { if (!cext_number(value, &number) || number < 0 || number > 11) return 0; o.attach = (int)number; }
                else if (!strcmp(key, "bone")) { if (!cext_number(value, &number) || number < -1 || number > CEXT_MAX_BONE) return 0; o.bone = (int)number; }
                else if (!strcmp(key, "follow")) { if (!cext_number(value, &number)) return 0; o.follow = number != 0; }
                else if (!strcmp(key, "upright")) { if (!cext_number(value, &number)) return 0; o.upright = number != 0; }
                else if (!strcmp(key, "move")) { if (!cext_valid_file(value)) return 0; strcpy(o.move, value); }
                /* any other field belongs to a later version */
            }
            if (!have_model || (o.follow && (o.attach < 0 || o.bone >= 0))) return 0;   /* follow: attached to a root */
            out->objects[out->count++] = o;
        } else if (n > 6 && !memcmp(line, "music ", 6)) {
            const char* q = line + 6;
            if (out->has_music) return 0;
            if (!cext_token(&q, le, 0, out->music, sizeof out->music) || !cext_valid_file(out->music)) return 0;
            out->music_loop = 1;
            while (q < le) {
                char key[16], value[24];
                long long number = 0;
                if (*q != ' ') return 0;
                q++;
                if (!cext_token(&q, le, '=', key, sizeof key) || q >= le || *q != '=') return 0;
                q++;
                if (!cext_token(&q, le, 0, value, sizeof value)) return 0;
                if (!strcmp(key, "start")) { if (!cext_number(value, &out->music_start_ms)) return 0; }
                else if (!strcmp(key, "loop")) { if (!cext_number(value, &number)) return 0; out->music_loop = number != 0; }
            }
            out->has_music = 1;
        }
        /* any other line belongs to a later version */
    }
    return out->version == 1 && have_room && have_now;
}

#endif
