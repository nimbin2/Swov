/* swov — a fast window/workspace overview for Sway (SDL3)
 *
 * Build:
 *   cc -std=c11 -O2 -Wall -Wextra -o swov swov.c \
 *      $(pkg-config --cflags --libs sdl3 sdl3-image sdl3-ttf)
 *
 * Runtime deps: a running Sway session ($SWAYSOCK). No jq, no shell-outs:
 * the program talks to the Sway IPC socket directly. fontconfig (fc-match)
 * is used if present to pick the desktop font, otherwise a few well known
 * font paths are tried.
 *
 * See README.md for configuration and key bindings.
 */

#define _POSIX_C_SOURCE 200809L

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sw_theme.h"

#define APP_ID          "swov"
#define SWOV_VERSION    "1.2"
#ifndef SWOV_BUILD                 /* set by the Makefile: md5 of this file */
#define SWOV_BUILD "unknown"
#endif
#define MAX_WINDOWS     512
#define MAX_WORKSPACES  64
#define MAX_DESKTOPS    4096

/* ------------------------------------------------------------------ util */

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("swov: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s ? s : "", n);
    return p;
}

static char *fmt_alloc(const char *fmt, ...)
{
    if (!fmt) return xstrdup("");
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return xstrdup("");
    char *buf = (char *)xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

/* snprintf(dst, sizeof dst, "%s", src) makes newer gcc warn: it cannot prove
 * src is shorter than dst, since a char field inside a struct array could in
 * principle run to the end of that array. The precision settles it. */
static void str_set(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    snprintf(dst, cap, "%.*s", (int)cap - 1, src ? src : "");
}

static bool file_readable(const char *p) { return p && access(p, R_OK) == 0; }

static bool is_dir(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *str_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n] = 0;
    return s;
}

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* case-insensitive substring search (used by the filter) */
static bool ci_contains(const char *hay, const char *needle)
{
    if (!needle || !*needle) return true;
    if (!hay) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

static bool str_all_digits(const char *s)
{
    if (!s || !*s) return false;
    for (const char *p = s; *p; ++p) if (!isdigit((unsigned char)*p)) return false;
    return true;
}

/* ---------------------------------------------------------------- colors */

static SDL_FColor rgba(uint32_t v)
{
    return (SDL_FColor){ ((v >> 24) & 0xff) / 255.0f,
                         ((v >> 16) & 0xff) / 255.0f,
                         ((v >>  8) & 0xff) / 255.0f,
                         ((v      ) & 0xff) / 255.0f };
}

static bool parse_color(const char *s, SDL_FColor *out)
{
    if (!s) return false;
    while (*s == '#' || *s == ' ') s++;
    size_t n = strlen(s);
    if (n != 6 && n != 8) return false;
    char buf[9];
    memcpy(buf, s, n);
    buf[n] = 0;
    for (size_t i = 0; i < n; ++i) if (!isxdigit((unsigned char)buf[i])) return false;

    unsigned long v = strtoul(buf, NULL, 16);
    if (n == 6) v = (v << 8) | 0xff;
    *out = rgba((uint32_t)v);
    return true;
}

/* c blended towards `to` by t (0..1); alpha kept from c */
static SDL_FColor mix(SDL_FColor c, SDL_FColor to, float t)
{
    return (SDL_FColor){ c.r + (to.r - c.r) * t,
                         c.g + (to.g - c.g) * t,
                         c.b + (to.b - c.b) * t,
                         c.a + (to.a - c.a) * t };
}

static SDL_FColor with_alpha(SDL_FColor c, float a) { c.a = a; return c; }

/* ---------------------------------------------------------------- config */

typedef struct {
    /* rendering */
    int   ssaa;              /* 1..4 full scene supersampling */
    int   icons;             /* show .desktop icons */
    int   icon_px;           /* icon edge length (logical px, before ui_scale) */
    int   shadow;
    int   vsync;

    /* text */
    float ui_scale;
    int   ws_px;             /* workspace number badge */
    int   label_px;          /* app name on a window card */
    int   title_px;          /* window title (the long text) */
    int   hint_px;           /* footer / header text */
    char  font[512];
    char  font_bold[512];

    /* layout */
    int   cols, rows;        /* 0 = automatic */
    float margin, gap, pad, radius, border;
    float win_gap;           /* space between two window cards            */
    float badge_top;         /* space above the workspace number          */
    float float_alpha;       /* opacity of floating / fullscreen cards    */
    float screen_pad;        /* border between mini screen and the cards  */
    int   shadow_layers;
    int   usage_dots;        /* dot scale down the left edge of a tile     */
    int   track;             /* record how long each workspace is used     */
    int   back_scope;        /* -b: 0 global, 1 this monitor, 2 sway's own */
    int   blur;              /* --backdrop only: 0 off, 1..3 how soft       */
    int   drop_ghosts;       /* --backdrop: offer the free numbers as tiles */
    int   drop_outputs;      /* mid-drag, show every screen's workspaces    */
    int   over_fullscreen;   /* un-fullscreen what is in the way, and put
                                it back on the way out                      */
    int   cpu;               /* the load dots on a tile, measured by swbr   */
    float cpu_idle;          /* under this, nothing is happening at all     */
    float cpu_min, cpu_full; /* the scale, in cores                         */
    int   dot_count;         /* how many dots the scale has                */
    float dot_px;            /* dot diameter                               */
    float anim_ms;           /* tile glide duration, 0 disables            */
    int   start_selection;   /* 0 none, 1 workspace, 2 first window       */
    char  header_pos[16];    /* none|top-left|top-center|...|bottom-right */
    char  hints_pos[16];
    int   show_empty;
    int   all_outputs;
    int   outputs_map;       /* the little map of the monitors, bottom left  */
    int   map_dwell_ms;      /* hold an app over one this long to step onto it */
    float outputs_map_w;     /* how wide, as a share of the tile area        */
    char  output[64];        /* start looking at this screen, not the one
                                sway is on                                   */
    char  launcher[192];     /* what `d` opens, and swov steps aside for     */
    char  tab[16];           /* what tab walks: workspaces, or last used     */
    int   show_header;
    int   show_hints;
    int   quit_on_focus_loss;
    int   quit_after_action;

    /* colors */
    SDL_FColor bg, tile, tile_sel, tile_hover, mini_bg;
    SDL_FColor card, card_hover, card_focus;
    SDL_FColor hl, text, subtext, dim, accent, hltext, shadow_col, outline, urgent, hint;
    SDL_FColor current;      /* what sway is showing right now             */
    SDL_FColor match;        /* windows the search found                   */
} Cfg;

static Cfg cfg_defaults(void)
{
    Cfg c;
    memset(&c, 0, sizeof(c));

    c.ssaa       = 2;
    c.icons      = 1;
    c.icon_px    = 40;
    c.shadow     = 1;
    c.vsync      = 1;

    c.ui_scale   = 1.0f;
    c.ws_px      = 26;
    c.label_px   = 16;
    c.title_px   = 13;
    c.hint_px    = 14;

    c.cols = c.rows = 0;
    c.margin  = 26.0f;
    c.win_gap    = 5.0f;
    c.badge_top  = 7.0f;
    c.float_alpha = 0.62f;
    c.screen_pad = 6.0f;
    c.shadow_layers = 3;
    c.usage_dots = 1;
    c.track      = 1;
    c.back_scope = 0;        /* global: go back to where I just was */
    c.blur       = 2;
    c.cpu        = 1;
    c.drop_ghosts = 1;
    c.drop_outputs = 1;
    c.over_fullscreen = 1;
    c.cpu_idle   = 0.01f;    /* cores; under this nothing is happening */
    c.cpu_min    = 0.25f;    /* cores, not a share of the machine */
    c.cpu_full   = 4.0f;
    c.dot_count  = 14;
    c.dot_px     = 5.0f;
    c.anim_ms = 160.0f;
    c.start_selection = 1;
    str_set(c.header_pos, sizeof(c.header_pos), "top-right");
    str_set(c.hints_pos, sizeof(c.hints_pos), "bottom-center");
    c.gap     = 14.0f;
    c.pad     = 10.0f;
    c.radius  = 14.0f;
    c.border  = 3.0f;

    c.show_empty         = 1;
    c.all_outputs        = 0;
    c.outputs_map        = 1;
    c.outputs_map_w      = 0.18f;
    c.map_dwell_ms       = 0;
    str_set(c.launcher, sizeof(c.launcher), "swas --replace overview=1");
    str_set(c.tab, sizeof(c.tab), "recent");
    c.show_header        = 1;
    c.show_hints         = 1;
    c.quit_on_focus_loss = 1;
    c.quit_after_action  = 0;

    /* palette: same family as the swas config */
    c.bg         = rgba(0x0d1117cc);  /* dims whatever is behind the overlay*/
    c.tile       = rgba(0x1e2733f2);  /* workspace tile                     */
    c.tile_sel   = rgba(0x26313ff2);  /* selected workspace tile            */
    c.tile_hover = rgba(0x2b3644f2);  /* hovered workspace tile             */
    c.mini_bg    = rgba(0x11171f9e);  /* inset "screen" inside a tile       */
    c.card       = rgba(0x33404ff7);  /* window card                        */
    c.card_hover = rgba(0x46566af7);  /* hovered window card                */
    c.card_focus = rgba(0x3b4a5bf7);  /* card of the window sway focuses    */
    c.hl         = rgba(0xcb9b00ff);  /* the orange                         */
    c.text       = rgba(0xe8e8e8ff);
    c.subtext    = rgba(0xb3c0cdff);
    c.dim        = rgba(0x5a6b7aff);
    c.accent     = rgba(0x89afc4ff);
    c.hltext     = rgba(0x141414ff);
    c.shadow_col = rgba(0x00000073);
    c.outline    = rgba(0x0a0e1499);
    c.urgent     = rgba(0xe0533cff);
    c.hint       = rgba(0xa7b5c4ff);  /* header line and the key hints       */
    c.current    = rgba(0x4fb3a5ff);  /* the live workspace / focused window */
    c.match      = rgba(0xb58ae0ff);  /* search hits                         */
    return c;
}

static bool key_is(const char *k, const char *a) { return ci_cmp(k, a) == 0; }

static void cfg_set(Cfg *c, const char *k, const char *v)
{
    /* rendering */
    if (key_is(k,"ssaa") || key_is(k,"aa")) {
        c->ssaa = atoi(v); if (c->ssaa < 1) c->ssaa = 1; if (c->ssaa > 4) c->ssaa = 4;
    }
    else if (key_is(k,"icons"))        c->icons = atoi(v) != 0;
    else if (key_is(k,"icon_px"))      c->icon_px = atoi(v);
    else if (key_is(k,"shadow"))       c->shadow = atoi(v) != 0;
    else if (key_is(k,"vsync"))        c->vsync = atoi(v) != 0;

    /* text */
    else if (key_is(k,"ui_scale") || key_is(k,"font_scale") || key_is(k,"text_scale"))
        c->ui_scale = (float)atof(v);
    else if (key_is(k,"ws_px") || key_is(k,"workspace_px")) c->ws_px = atoi(v);
    else if (key_is(k,"label_px") || key_is(k,"app_px"))    c->label_px = atoi(v);
    else if (key_is(k,"title_px"))                          c->title_px = atoi(v);
    else if (key_is(k,"hint_px") || key_is(k,"count_px"))   c->hint_px = atoi(v);
    else if (key_is(k,"font"))       str_set(c->font, sizeof(c->font), v);
    else if (key_is(k,"font_bold"))  str_set(c->font_bold, sizeof(c->font_bold), v);

    /* layout */
    else if (key_is(k,"cols"))          c->cols = atoi(v);
    else if (key_is(k,"rows"))          c->rows = atoi(v);
    else if (key_is(k,"margin"))        c->margin = (float)atof(v);
    else if (key_is(k,"gap"))           c->gap = (float)atof(v);
    else if (key_is(k,"pad"))           c->pad = (float)atof(v);
    else if (key_is(k,"radius") || key_is(k,"corner")) c->radius = (float)atof(v);
    else if (key_is(k,"border"))        c->border = (float)atof(v);
    else if (key_is(k,"win_gap") || key_is(k,"window_gap")) c->win_gap = (float)atof(v);
    else if (key_is(k,"screen_pad"))    c->screen_pad = (float)atof(v);
    else if (key_is(k,"badge_top") || key_is(k,"badge_scale")) c->badge_top = (float)atof(v);
    else if (key_is(k,"float_alpha") || key_is(k,"floating_alpha"))
        c->float_alpha = (float)atof(v);
    else if (key_is(k,"shadow_layers")) c->shadow_layers = atoi(v);
    else if (key_is(k,"usage_dots") || key_is(k,"usage_bar"))
        c->usage_dots = atoi(v) != 0;
    else if (key_is(k,"dot_count"))     c->dot_count = atoi(v);
    else if (key_is(k,"dot_px"))        c->dot_px = (float)atof(v);
    else if (key_is(k,"track"))         c->track = atoi(v) != 0;
    else if (key_is(k,"cpu"))       c->cpu = atoi(v) != 0;
    else if (key_is(k,"drop_ghosts")) c->drop_ghosts = atoi(v) != 0;
    else if (key_is(k,"drop_outputs")) c->drop_outputs = atoi(v) != 0;
    else if (key_is(k,"over_fullscreen")) c->over_fullscreen = atoi(v) != 0;
    else if (key_is(k,"cpu_idle"))  c->cpu_idle = (float)atof(v);
    else if (key_is(k,"cpu_min"))   c->cpu_min = (float)atof(v);
    else if (key_is(k,"cpu_full"))  c->cpu_full = (float)atof(v);
    else if (key_is(k,"blur")) {
        c->blur = atoi(v); if (c->blur < 0) c->blur = 0; if (c->blur > 3) c->blur = 3;
    }
    else if (key_is(k,"back") || key_is(k,"back_scope"))
        c->back_scope = key_is(v,"output") ? 1 : key_is(v,"sway") ? 2 : 0;
    else if (key_is(k,"anim_ms") || key_is(k,"animation")) c->anim_ms = (float)atof(v);
    else if (key_is(k,"start_selection")) {
        c->start_selection = key_is(v,"none") ? 0 : key_is(v,"window") ? 2 : 1;
    }
    else if (key_is(k,"header_pos")) str_set(c->header_pos, sizeof(c->header_pos), v);
    else if (key_is(k,"hints_pos"))  str_set(c->hints_pos, sizeof(c->hints_pos), v);
    else if (key_is(k,"show_empty"))    c->show_empty = atoi(v) != 0;
    else if (key_is(k,"all_outputs"))   c->all_outputs = atoi(v) != 0;
    else if (key_is(k,"outputs_map"))   c->outputs_map = atoi(v) != 0;
    else if (key_is(k,"outputs_map_w")) c->outputs_map_w = (float)atof(v);
    else if (key_is(k,"map_dwell_ms"))  c->map_dwell_ms = atoi(v);
    else if (key_is(k,"output"))        str_set(c->output, sizeof(c->output), v);
    else if (key_is(k,"launcher"))      str_set(c->launcher, sizeof(c->launcher), v);
    else if (key_is(k,"tab"))           str_set(c->tab, sizeof(c->tab), v);
    else if (key_is(k,"show_header"))   c->show_header = atoi(v) != 0;
    else if (key_is(k,"show_hints"))    c->show_hints = atoi(v) != 0;
    else if (key_is(k,"quit_on_focus_loss")) c->quit_on_focus_loss = atoi(v) != 0;
    else if (key_is(k,"quit_after_action"))  c->quit_after_action = atoi(v) != 0;

    /* colors */
    else if (key_is(k,"bg"))          parse_color(v, &c->bg);
    else if (key_is(k,"tile") || key_is(k,"ring"))       parse_color(v, &c->tile);
    else if (key_is(k,"tile_sel") || key_is(k,"ring2"))  parse_color(v, &c->tile_sel);
    else if (key_is(k,"tile_hover"))  parse_color(v, &c->tile_hover);
    else if (key_is(k,"mini_bg") || key_is(k,"center"))  parse_color(v, &c->mini_bg);
    else if (key_is(k,"card"))        parse_color(v, &c->card);
    else if (key_is(k,"card_hover") || key_is(k,"hover")) parse_color(v, &c->card_hover);
    else if (key_is(k,"card_focus"))  parse_color(v, &c->card_focus);
    else if (key_is(k,"hl"))          parse_color(v, &c->hl);
    else if (key_is(k,"text"))        parse_color(v, &c->text);
    else if (key_is(k,"subtext"))     parse_color(v, &c->subtext);
    else if (key_is(k,"dim"))         parse_color(v, &c->dim);
    else if (key_is(k,"accent"))      parse_color(v, &c->accent);
    else if (key_is(k,"hltext"))      parse_color(v, &c->hltext);
    else if (key_is(k,"shadow_color")) parse_color(v, &c->shadow_col);
    else if (key_is(k,"outline"))     parse_color(v, &c->outline);
    else if (key_is(k,"urgent"))      parse_color(v, &c->urgent);
    else if (key_is(k,"hint"))        parse_color(v, &c->hint);
    else if (key_is(k,"current"))     parse_color(v, &c->current);
    else if (key_is(k,"match"))       parse_color(v, &c->match);
    else fprintf(stderr, "swov: unknown config key '%s' (ignored)\n", k);
}

/* the shared ~/.config/sw/config, translated into swov's own keys */
static void cfg_set_shared(void *ud, const char *k, const char *v)
{
    cfg_set((Cfg *)ud, k, v);
}

static char *expand_tilde(const char *p)
{
    if (p && p[0] == '~' && (p[1] == '/' || p[1] == 0)) {
        const char *home = getenv("HOME");
        if (home) return fmt_alloc("%s%s", home, p + 1);
    }
    return xstrdup(p ? p : "");
}

static bool cfg_load_file(Cfg *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *s = str_trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = str_trim(s);
        char *v = str_trim(eq + 1);
        char *hash = strchr(v, '#');           /* trailing comment */
        if (hash && hash > v && hash[-1] == ' ') { *hash = 0; v = str_trim(v); }
        if (*k) cfg_set(c, k, v);
    }
    fclose(f);
    return true;
}

static char  CFG_PATH[512];
static bool  CFG_LOADED;
static bool  SHARED_LOADED;

static char *default_config_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fmt_alloc("%s/swov/config", xdg);
    const char *home = getenv("HOME");
    if (home) return fmt_alloc("%s/.config/swov/config", home);
    return NULL;
}

/* ----------------------------------------------------------- json parser
 * Just enough JSON to read sway's IPC replies: no streaming, no comments,
 * numbers as double, strings unescaped to UTF-8.
 */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JV JV;
struct JV {
    JType type;
    bool   b;
    double num;
    char  *str;              /* J_STR */
    JV   **items;            /* J_ARR / J_OBJ values */
    char **keys;             /* J_OBJ keys */
    int    count, cap;
};

static void jfree(JV *v)
{
    if (!v) return;
    switch (v->type) {
    case J_STR: free(v->str); break;
    case J_ARR:
        for (int i = 0; i < v->count; ++i) jfree(v->items[i]);
        free(v->items);
        break;
    case J_OBJ:
        for (int i = 0; i < v->count; ++i) { free(v->keys[i]); jfree(v->items[i]); }
        free(v->items);
        free(v->keys);
        break;
    default: break;
    }
    free(v);
}

static JV *jnew(JType t)
{
    JV *v = (JV *)xmalloc(sizeof(JV));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static void jpush(JV *v, char *key, JV *val)
{
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = (JV **)xrealloc(v->items, (size_t)v->cap * sizeof(JV *));
        if (v->type == J_OBJ)
            v->keys = (char **)xrealloc(v->keys, (size_t)v->cap * sizeof(char *));
    }
    if (v->type == J_OBJ) v->keys[v->count] = key;
    v->items[v->count++] = val;
}

static void utf8_append(char **dst, size_t *len, size_t *cap, uint32_t cp)
{
    char tmp[4];
    int n = 0;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        tmp[0] = (char)(0xc0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xe0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        tmp[0] = (char)(0xf0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    if (*len + (size_t)n + 1 > *cap) {
        *cap = (*cap ? *cap * 2 : 32) + (size_t)n;
        *dst = (char *)xrealloc(*dst, *cap);
    }
    memcpy(*dst + *len, tmp, (size_t)n);
    *len += (size_t)n;
    (*dst)[*len] = 0;
}

static const char *jskip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *jparse_string(const char *p, char **out)
{
    /* p points at the opening quote */
    size_t len = 0, cap = 32;
    char *s = (char *)xmalloc(cap);
    s[0] = 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            uint32_t cp = 0;
            switch (*p) {
            case 'n': cp = '\n'; p++; break;
            case 't': cp = '\t'; p++; break;
            case 'r': cp = '\r'; p++; break;
            case 'b': cp = '\b'; p++; break;
            case 'f': cp = '\f'; p++; break;
            case '/': cp = '/';  p++; break;
            case '"': cp = '"';  p++; break;
            case '\\': cp = '\\'; p++; break;
            case 'u': {
                p++;
                char hex[5] = {0};
                for (int i = 0; i < 4 && p[i]; ++i) hex[i] = p[i];
                cp = (uint32_t)strtoul(hex, NULL, 16);
                p += 4;
                if (cp >= 0xd800 && cp <= 0xdbff && p[0] == '\\' && p[1] == 'u') {
                    char hex2[5] = {0};
                    for (int i = 0; i < 4 && p[2 + i]; ++i) hex2[i] = p[2 + i];
                    uint32_t lo = (uint32_t)strtoul(hex2, NULL, 16);
                    if (lo >= 0xdc00 && lo <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    }
                }
                break;
            }
            default:
                if (!*p) { free(s); *out = NULL; return NULL; }
                cp = (unsigned char)*p++;
                break;
            }
            utf8_append(&s, &len, &cap, cp);
        } else {
            if (len + 2 > cap) { cap *= 2; s = (char *)xrealloc(s, cap); }
            s[len++] = *p++;
            s[len] = 0;
        }
    }
    if (*p != '"') { free(s); *out = NULL; return NULL; }
    *out = s;
    return p + 1;
}

static const char *jparse_value(const char *p, JV **out);

static const char *jparse_container(const char *p, JV **out, bool is_obj)
{
    JV *v = jnew(is_obj ? J_OBJ : J_ARR);
    p = jskip_ws(p + 1);
    char close = is_obj ? '}' : ']';
    if (*p == close) { *out = v; return p + 1; }

    for (;;) {
        char *key = NULL;
        if (is_obj) {
            p = jskip_ws(p);
            if (*p != '"') { jfree(v); return NULL; }
            p = jparse_string(p, &key);
            if (!p) { jfree(v); return NULL; }
            p = jskip_ws(p);
            if (*p != ':') { free(key); jfree(v); return NULL; }
            p++;
        }
        JV *child = NULL;
        p = jparse_value(jskip_ws(p), &child);
        if (!p) { free(key); jfree(v); return NULL; }
        jpush(v, key, child);

        p = jskip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == close) { p++; break; }
        jfree(v);
        return NULL;
    }
    *out = v;
    return p;
}

static const char *jparse_value(const char *p, JV **out)
{
    p = jskip_ws(p);
    switch (*p) {
    case '{': return jparse_container(p, out, true);
    case '[': return jparse_container(p, out, false);
    case '"': {
        JV *v = jnew(J_STR);
        p = jparse_string(p, &v->str);
        if (!p) { jfree(v); return NULL; }
        *out = v;
        return p;
    }
    case 't':
        if (strncmp(p, "true", 4)) return NULL;
        *out = jnew(J_BOOL); (*out)->b = true; return p + 4;
    case 'f':
        if (strncmp(p, "false", 5)) return NULL;
        *out = jnew(J_BOOL); (*out)->b = false; return p + 5;
    case 'n':
        if (strncmp(p, "null", 4)) return NULL;
        *out = jnew(J_NULL); return p + 4;
    default: {
        char *end = NULL;
        double d = strtod(p, &end);
        if (end == p) return NULL;
        JV *v = jnew(J_NUM);
        v->num = d;
        *out = v;
        return end;
    }
    }
}

static JV *jparse(const char *text)
{
    JV *v = NULL;
    const char *p = jparse_value(text, &v);
    if (!p) { jfree(v); return NULL; }
    return v;
}

static JV *jget(const JV *o, const char *key)
{
    if (!o || o->type != J_OBJ) return NULL;
    for (int i = 0; i < o->count; ++i)
        if (o->keys[i] && strcmp(o->keys[i], key) == 0) return o->items[i];
    return NULL;
}

static const char *jstr(const JV *o, const char *key, const char *def)
{
    JV *v = jget(o, key);
    return (v && v->type == J_STR) ? v->str : def;
}

static int jint(const JV *o, const char *key, int def)
{
    JV *v = jget(o, key);
    return (v && v->type == J_NUM) ? (int)v->num : def;
}

static bool jbool(const JV *o, const char *key, bool def)
{
    JV *v = jget(o, key);
    return (v && v->type == J_BOOL) ? v->b : def;
}

/* -------------------------------------------------------------- sway ipc */

enum {
    IPC_GET_OUTPUTS    = 3,
    IPC_RUN_COMMAND    = 0,
    IPC_GET_WORKSPACES = 1,
    IPC_GET_TREE       = 4,
    IPC_SUBSCRIBE      = 2
};

static int sway_fd = -1;      /* request socket  */
static int sway_evt_fd = -1;  /* event socket    */

static int sway_connect(void)
{
    const char *path = getenv("SWAYSOCK");
    if (!path || !*path) path = getenv("I3SOCK");
    if (!path || !*path) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    str_set(addr.sun_path, sizeof(addr.sun_path), path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

/* Returns a malloc'ed, NUL-terminated payload (caller frees), or NULL. */
static char *sway_request(uint32_t type, const char *payload)
{
    if (sway_fd < 0) return NULL;

    size_t plen = payload ? strlen(payload) : 0;
    char hdr[14];
    memcpy(hdr, "i3-ipc", 6);
    uint32_t l = (uint32_t)plen, t = type;
    memcpy(hdr + 6, &l, 4);
    memcpy(hdr + 10, &t, 4);

    if (!write_all(sway_fd, hdr, sizeof(hdr))) return NULL;
    if (plen && !write_all(sway_fd, payload, plen)) return NULL;

    char rhdr[14];
    if (!read_all(sway_fd, rhdr, sizeof(rhdr))) return NULL;
    if (memcmp(rhdr, "i3-ipc", 6) != 0) return NULL;

    uint32_t rlen = 0;
    memcpy(&rlen, rhdr + 6, 4);
    if (rlen > (64u << 20)) return NULL;

    char *body = (char *)xmalloc(rlen + 1);
    if (rlen && !read_all(sway_fd, body, rlen)) { free(body); return NULL; }
    body[rlen] = 0;
    return body;
}

static JV *sway_query(uint32_t type)
{
    char *body = sway_request(type, NULL);
    if (!body) return NULL;
    JV *v = jparse(body);
    free(body);
    return v;
}

static bool sway_cmd(const char *fmt, ...)
{
    if (!fmt) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return false;

    char *cmd = (char *)xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(cmd, (size_t)n + 1, fmt, ap);
    va_end(ap);

    char *reply = sway_request(IPC_RUN_COMMAND, cmd);
    free(cmd);
    if (!reply) return false;

    bool ok = true;
    JV *v = jparse(reply);
    if (v && v->type == J_ARR) {
        for (int i = 0; i < v->count; ++i)
            if (!jbool(v->items[i], "success", true)) ok = false;
    }
    jfree(v);
    free(reply);
    return ok;
}

/* Sway answers a command as soon as it is queued, but the tree only carries
 * the new geometry once the transaction has committed. Rather than sleeping
 * and hoping, we subscribe to window and workspace events on a second socket
 * and reload when sway tells us it is done. That also keeps the overview
 * correct while it is open. */
static bool sway_subscribe_events(void)
{
    sway_evt_fd = sway_connect();
    if (sway_evt_fd < 0) return false;

    const char *payload = "[\"window\",\"workspace\"]";
    char hdr[14];
    memcpy(hdr, "i3-ipc", 6);
    uint32_t l = (uint32_t)strlen(payload), t = IPC_SUBSCRIBE;
    memcpy(hdr + 6, &l, 4);
    memcpy(hdr + 10, &t, 4);

    if (!write_all(sway_evt_fd, hdr, sizeof(hdr)) ||
        !write_all(sway_evt_fd, payload, l)) {
        close(sway_evt_fd);
        sway_evt_fd = -1;
        return false;
    }

    char rhdr[14];
    uint32_t rlen = 0;
    if (!read_all(sway_evt_fd, rhdr, sizeof(rhdr))) { close(sway_evt_fd); sway_evt_fd = -1; return false; }
    memcpy(&rlen, rhdr + 6, 4);
    if (rlen && rlen < (1u << 20)) {
        char *body = (char *)xmalloc(rlen + 1);
        read_all(sway_evt_fd, body, rlen);
        free(body);
    }

    int flags = fcntl(sway_evt_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(sway_evt_fd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

/* true when sway reported at least one change since the last call */
static bool sway_events_pending(void)
{
    if (sway_evt_fd < 0) return false;
    bool any = false;

    for (;;) {
        char hdr[14];
        ssize_t r = recv(sway_evt_fd, hdr, sizeof(hdr), MSG_DONTWAIT);
        if (r <= 0) break;
        if ((size_t)r < sizeof(hdr)) {              /* rest of the header */
            if (!read_all(sway_evt_fd, hdr + r, sizeof(hdr) - (size_t)r)) break;
        }
        uint32_t rlen = 0;
        memcpy(&rlen, hdr + 6, 4);
        if (rlen > (16u << 20)) break;
        if (rlen) {
            char *body = (char *)xmalloc(rlen + 1);
            bool ok = read_all(sway_evt_fd, body, rlen);
            free(body);
            if (!ok) break;
        }
        any = true;
    }
    return any;
}

/* ------------------------------------------------------------ usage store
 * How long the user has had each workspace in front of them. Every workspace
 * switch goes through swov, so no daemon is needed: on each switch we credit
 * the workspace we are leaving with the time since the last switch, and write
 * down where we are going and when. A workspace that falls empty loses its
 * count — the number describes the workspace as it is now.
 *
 * File: one "> <epoch> <name>" line saying where we are, then "<seconds>
 * <name>" per workspace.
 */

typedef struct {
    char   name[64];
    char   output[64];    /* which monitor it lives on */
    int    num;           /* its number, or -1 for a named workspace */
    double secs;
    double last;          /* when we were last there (epoch) */
} Usage;

static Usage  USAGE[128];
static int    NUSAGE;
static double USAGE_MAX = 0.0;    /* the busiest workspace, for the scale */
static char   USAGE_CUR[64];      /* the workspace we are on              */
static char   USAGE_CUR_OUT[64];  /* ... on this output                   */
static double USAGE_SINCE;        /* ... since this moment (epoch seconds) */

static double now_secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static char *usage_path(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return fmt_alloc("%s/swov/usage", xdg);
    const char *home = getenv("HOME");
    return home ? fmt_alloc("%s/.cache/swov/usage", home) : NULL;
}

static Usage *usage_find(const char *name, bool create)
{
    for (int i = 0; i < NUSAGE; ++i)
        if (strcmp(USAGE[i].name, name) == 0) return &USAGE[i];
    if (!create || NUSAGE >= (int)SDL_arraysize(USAGE)) return NULL;

    Usage *u = &USAGE[NUSAGE++];
    memset(u, 0, sizeof(*u));
    u->num = -1;
    str_set(u->name, sizeof(u->name), name);
    return u;
}

static void usage_load(void)
{
    NUSAGE = 0;
    char *path = usage_path();
    if (!path) return;

    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return;

    USAGE_CUR[0] = 0;
    USAGE_CUR_OUT[0] = 0;
    USAGE_SINCE = 0.0;

    char line[320];
    while (fgets(line, sizeof(line), f) && NUSAGE < (int)SDL_arraysize(USAGE)) {
        double v = 0.0, last = 0.0;
        char name[64] = {0}, out[64] = {0};

        if (line[0] == '>') {
            if (sscanf(line + 1, "%lf %63s %63[^\n]", &v, out, name) == 3) {
                USAGE_SINCE = v;
                str_set(USAGE_CUR, sizeof(USAGE_CUR), name);
                str_set(USAGE_CUR_OUT, sizeof(USAGE_CUR_OUT), out);
            } else if (sscanf(line + 1, "%lf %63[^\n]", &v, name) == 2) {
                USAGE_SINCE = v;                     /* a file from before outputs */
                str_set(USAGE_CUR, sizeof(USAGE_CUR), name);
            }
            continue;
        }

        int num = -1;
        if (sscanf(line, "%lf %lf %d %63s %63[^\n]", &v, &last, &num, out, name) == 5 && name[0]) {
            Usage *u = usage_find(name, true);
            if (u) {
                u->secs = v;
                u->last = last;
                u->num  = num;
                if (strcmp(out, "-") != 0) str_set(u->output, sizeof(u->output), out);
            }
        } else if (sscanf(line, "%lf %lf %63s %63[^\n]", &v, &last, out, name) == 4 && name[0]) {
            Usage *u = usage_find(name, true);   /* a file from before numbers */
            if (u) {
                u->secs = v;
                u->last = last;
                if (strcmp(out, "-") != 0) str_set(u->output, sizeof(u->output), out);
            }
        } else if (sscanf(line, "%lf %63[^\n]", &v, name) == 2 && name[0]) {
            Usage *u = usage_find(name, true);
            if (u) u->secs = v;
        }
    }
    fclose(f);
}

/* time spent on the workspace we are on but not yet written down */
static double usage_pending(void)
{
    if (!USAGE_CUR[0] || USAGE_SINCE <= 0.0) return 0.0;
    double d = now_secs() - USAGE_SINCE;
    if (d < 0.0 || d > 12.0 * 3600.0) return 0.0;   /* clock jump, or a nap */
    return d;
}

static void usage_save(void)
{
    char *path = usage_path();
    if (!path) return;

    char *slash = strrchr(path, '/');
    if (slash) {                       /* mkdir -p on the parent, one level */
        *slash = 0;
        char *up = strrchr(path, '/');
        if (up) { *up = 0; mkdir(path, 0755); *up = '/'; }
        mkdir(path, 0755);
        *slash = '/';
    }

    char *tmp = fmt_alloc("%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        if (USAGE_CUR[0])
            fprintf(f, "> %.0f %s %s\n", USAGE_SINCE,
                    USAGE_CUR_OUT[0] ? USAGE_CUR_OUT : "-", USAGE_CUR);
        for (int i = 0; i < NUSAGE; ++i)
            if (USAGE[i].secs > 0.0 || USAGE[i].last > 0.0)
                fprintf(f, "%.0f %.3f %d %s %s\n", USAGE[i].secs, USAGE[i].last, USAGE[i].num,
                        USAGE[i].output[0] ? USAGE[i].output : "-", USAGE[i].name);
        fclose(f);
        rename(tmp, path);
    }
    free(tmp);
    free(path);
}

/* Credit the workspace we are leaving, then note where we are going. This is
 * the whole of the time tracking: swov performs every switch itself. */
static void usage_switch(const char *to, const char *out, int num)
{
    if (!to || !*to) return;
    usage_load();

    double now = now_secs();
    double pending = usage_pending();
    if (USAGE_CUR[0]) {
        Usage *u = usage_find(USAGE_CUR, true);
        if (u) {
            if (pending > 0.0) u->secs += pending;
            u->last = now;                          /* we were just there */
            if (USAGE_CUR_OUT[0]) str_set(u->output, sizeof(u->output), USAGE_CUR_OUT);
        }
    }

    Usage *dst = usage_find(to, true);
    if (dst) {
        if (out && *out) str_set(dst->output, sizeof(dst->output), out);
        dst->num  = num;
        dst->last = now;      /* arriving counts as a visit, so the entry (and
                               * with it the number) survives the save filter */
    }

    str_set(USAGE_CUR, sizeof(USAGE_CUR), to);
    str_set(USAGE_CUR_OUT, sizeof(USAGE_CUR_OUT), out ? out : "");
    USAGE_SINCE = now;
    usage_save();
}

/* The workspace we were on before this one. With `output` set, only the ones
 * on that monitor count (back=output); with NULL, the most recent anywhere
 * wins (back=global, the default), so leaving a monitor and coming back is a
 * toggle instead of a jump into that monitor's own history. */
static bool last_workspace_here(const char *output, const char *current,
                                char *out, size_t cap, int *num)
{
    usage_load();

    const Usage *best = NULL;
    for (int i = 0; i < NUSAGE; ++i) {
        const Usage *u = &USAGE[i];
        if (u->last <= 0.0) continue;
        if (current && strcmp(u->name, current) == 0) continue;
        if (output && *output && u->output[0] && strcmp(u->output, output) != 0) continue;
        if (!best || u->last > best->last) best = u;
    }
    if (!best) return false;
    str_set(out, cap, best->name);
    if (num) *num = best->num;
    return true;
}

/* the workspace sway is showing at this moment */
static void focused_workspace(char *name, size_t ncap, char *output, size_t ocap, int *num)
{
    name[0] = 0;
    if (output) output[0] = 0;
    if (num) *num = -1;

    JV *r = sway_query(IPC_GET_WORKSPACES);
    if (r && r->type == J_ARR)
        for (int i = 0; i < r->count; ++i)
            if (jbool(r->items[i], "focused", false)) {
                str_set(name, ncap, jstr(r->items[i], "name", ""));
                if (output) str_set(output, ocap, jstr(r->items[i], "output", ""));
                if (num) *num = jint(r->items[i], "num", -1);
            }
    jfree(r);
}

static bool  node_is_view(const JV *n);
static char *escape_arg(const char *s);

static bool  ADOPT_LOG;               /* --adopt-debug: say what is happening */
static bool  TIMING;                  /* --timing: where startup goes        */
static double T0, T_LAST;

/* Milliseconds since the last mark, on stderr. Cheap enough to leave in and
 * the only honest way to answer "why was that slow this time". */
static void mark(const char *what)
{
    if (!TIMING) return;
    double now = now_secs();
    if (T_LAST == 0.0) T_LAST = T0;
    fprintf(stderr, "swov: %-18s %6.1f ms   (%6.1f total)\n",
            what, (now - T_LAST) * 1000.0, (now - T0) * 1000.0);
    T_LAST = now;
}



/* ------------------------------------------------- workspaces / adopt
 * Two one-shot modes that need no window, no font and no renderer. They are
 * what a launcher talks to: --workspaces to know where a window could go,
 * --adopt to put one there once it shows up.
 */

/* `num  name  output  flags`, tab separated, one workspace per line. */
static int print_workspaces(void)
{
    JV *r = sway_query(IPC_GET_WORKSPACES);
    if (!r || r->type != J_ARR) { jfree(r); return 1; }

    for (int i = 0; i < r->count; ++i) {
        const JV *w = r->items[i];
        char flags[64] = "";
        if (jbool(w, "focused", false)) strcat(flags, "focused,");
        if (jbool(w, "visible", false)) strcat(flags, "visible,");
        if (jbool(w, "urgent",  false)) strcat(flags, "urgent,");
        size_t n = strlen(flags);
        if (n) flags[n - 1] = 0; else str_set(flags, sizeof(flags), "-");

        printf("%d\t%s\t%s\t%s\n", jint(w, "num", -1), jstr(w, "name", ""),
               jstr(w, "output", ""), flags);
    }
    jfree(r);
    return 0;
}

/* Is `pid` the process we launched, or something it started? A launcher that
 * goes through a shell, or an app that re-execs itself, would not match on
 * the pid alone. */
static bool pid_descends_from(int pid, int ancestor)
{
    for (int i = 0; i < 8 && pid > 1; ++i) {
        if (pid == ancestor) return true;

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) return false;

        /* comm can hold spaces and brackets, so start after the last ')' */
        char buf[512];
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[got] = 0;

        char *close = strrchr(buf, ')');
        int ppid = 0;
        if (!close || sscanf(close + 1, " %*c %d", &ppid) != 1) return false;
        pid = ppid;
    }
    return false;
}

/* the container id of the first view belonging to `pid`, or -1 */
static int find_view_of_pid(const JV *node, int pid)
{
    if (!node || node->type != J_OBJ) return -1;

    if (node_is_view(node)) {
        int p = jint(node, "pid", -1);
        if (p > 0 && pid_descends_from(p, pid)) return jint(node, "id", -1);
    }
    for (int i = 0; i < node->count; ++i) {
        const JV *kid = node->items[i];
        if (!kid) continue;
        if (kid->type == J_ARR) {
            for (int j = 0; j < kid->count; ++j) {
                int id = find_view_of_pid(kid->items[j], pid);
                if (id >= 0) return id;
            }
        } else if (kid->type == J_OBJ) {
            int id = find_view_of_pid(kid, pid);
            if (id >= 0) return id;
        }
    }
    return -1;
}

/* Tell sway where the window this process is about to open belongs, before it
 * opens. sway consults `assign` in select_workspace(), which runs before the
 * container is put anywhere, so the window never appears on the workspace you
 * are looking at and nothing there is rearranged. Without this the window
 * lands here first, the layout shuffles, and only then does it move.
 *
 * The rule stays for the rest of the sway session — there is no unassign —
 * but it is tied to one pid, so it does nothing after that process is gone. */
static void assign_pid_to_ws(int pid, const char *ws)
{
    if (str_all_digits(ws)) {
        sway_cmd("assign [pid=%d] workspace number %s", pid, ws);
    } else {
        char *e = escape_arg(ws);
        sway_cmd("assign [pid=%d] workspace \"%s\"", pid, e);
        free(e);
    }
}

/* Every process descending from `root`, so the rule can be put in front of
 * whichever one ends up owning the window. A launcher, a wrapper script or an
 * app that forks and lets the parent go leaves the window belonging to a pid
 * we were never told about, and an assign rule for the pid we started is then
 * matched by nothing. */
static int sweep_descendants(int root, int *out, int cap)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        if (!str_all_digits(e->d_name)) continue;
        int pid = atoi(e->d_name);
        if (pid <= 1 || pid == root) continue;
        if (pid_descends_from(pid, root)) out[n++] = pid;
    }
    closedir(d);
    return n;
}

static void move_to_ws_edge_named(int con_id, const char *ws, const char *edge);

/* every view in the tree, so a window that turns up later can be spotted */
static void collect_view_ids(const JV *node, int *ids, int *n, int cap)
{
    if (!node || node->type != J_OBJ || *n >= cap) return;
    if (node_is_view(node)) {
        int id = jint(node, "id", -1);
        if (id >= 0) ids[(*n)++] = id;
    }
    for (int i = 0; i < node->count; ++i) {
        const JV *kid = node->items[i];
        if (!kid) continue;
        if (kid->type == J_ARR)
            for (int j = 0; j < kid->count; ++j) collect_view_ids(kid->items[j], ids, n, cap);
        else if (kid->type == J_OBJ)
            collect_view_ids(kid, ids, n, cap);
    }
}

static int find_new_view(const JV *node, const int *known, int nknown)
{
    int ids[512], n = 0;
    collect_view_ids(node, ids, &n, (int)SDL_arraysize(ids));
    for (int i = 0; i < n; ++i) {
        bool seen = false;
        for (int j = 0; j < nknown; ++j) if (known[j] == ids[i]) { seen = true; break; }
        if (!seen) return ids[i];
    }
    return -1;
}

/* Wait for the window `pid` opens, then put it where it was dropped. sway has
 * no "run this on workspace N", so this is the only way to land an app
 * somewhere else without switching there first and switching back.
 *
 * The pid is the first thing we look for, but it is not reliable on its own:
 * a launcher, a wrapper script or an app that re-execs leaves the window
 * belonging to a process whose parent chain no longer leads back to us. So we
 * also watch for a view that simply was not there when we started — the one
 * the drop just asked for. */
static int adopt_window(int pid, const char *ws, double timeout, bool focus,
                        int beside, const char *edge, bool no_assign)
{
    double deadline = now_secs() + timeout;
    int    con_id   = -1;

    /* Each of these is a rule sway keeps for the rest of the session — there
     * is no unassign — so they are worth counting. Two dozen per launch was
     * generous; six covers a wrapper script and its children. */
    int assigned[6], nassigned = 0;
    if (!no_assign) {
        assign_pid_to_ws(pid, ws);                /* before anything maps */
        assigned[nassigned++] = pid;
    }

    int known[512], nknown = 0;
    JV *first = sway_query(IPC_GET_TREE);
    if (first) { collect_view_ids(first, known, &nknown, (int)SDL_arraysize(known)); jfree(first); }
    if (ADOPT_LOG) fprintf(stderr, "swov: adopt pid %d -> ws %s (%d views now)\n",
                           pid, ws, nknown);

    double started = now_secs();
    int    ticks = 0;

    while (now_secs() < deadline) {
        struct timespec ts = { 0, 40 * 1000000L };
        nanosleep(&ts, NULL);
        ticks++;

        /* Keep putting the rule in front of anything the app starts, for the
         * first few seconds. Every one of these is a pid that could be the
         * one sway ends up seeing. */
        if (!no_assign && nassigned < (int)SDL_arraysize(assigned) &&
            now_secs() - started < 4.0) {
            int kids[64];
            int nk = sweep_descendants(pid, kids, (int)SDL_arraysize(kids));
            for (int i = 0; i < nk && nassigned < (int)SDL_arraysize(assigned); ++i) {
                bool seen = false;
                for (int j = 0; j < nassigned; ++j)
                    if (assigned[j] == kids[i]) { seen = true; break; }
                if (seen) continue;
                assign_pid_to_ws(kids[i], ws);
                assigned[nassigned++] = kids[i];
                if (ADOPT_LOG) fprintf(stderr, "swov: also assigned pid %d\n", kids[i]);
            }
        }

        if (ticks % 3) continue;                  /* look at the tree at 8 Hz */

        JV *tree = sway_query(IPC_GET_TREE);
        if (!tree) continue;
        con_id = find_view_of_pid(tree, pid);
        if (con_id < 0) con_id = find_new_view(tree, known, nknown);
        jfree(tree);

        if (con_id >= 0) break;
        if (kill(pid, 0) != 0 && errno == ESRCH && now_secs() - started > 3.0) {
            if (ADOPT_LOG) fprintf(stderr, "swov: adopt gave up, pid %d is gone\n", pid);
            break;
        }
    }
    if (con_id < 0) {
        if (ADOPT_LOG) fprintf(stderr, "swov: adopt found no window for pid %d\n", pid);
        return 1;
    }
    if (ADOPT_LOG) fprintf(stderr, "swov: adopt window con_id %d\n", con_id);

    /* If it did land here after all, float it first: a tiled window leaving a
     * workspace makes everything left behind reflow, and that is the flash
     * the assign rule is there to avoid. Floating, moving and tiling again
     * touches only the workspace it ends up on. */
    bool ok;
    sway_cmd("[con_id=%d] floating enable", con_id);
    if (str_all_digits(ws)) {
        ok = sway_cmd("[con_id=%d] move container to workspace number %s", con_id, ws);
    } else {
        char *e = escape_arg(ws);
        ok = sway_cmd("[con_id=%d] move container to workspace \"%s\"", con_id, e);
        free(e);
    }
    sway_cmd("[con_id=%d] floating disable", con_id);

    /* Dropped against the edge of a workspace whose windows are stacked: one
     * move across the stack lifts the new window out of it and puts it beside
     * the whole column, which is what "left of both" means. */
    if (ok && beside <= 0 && edge) {
        move_to_ws_edge_named(con_id, ws, edge);
        if (ADOPT_LOG) fprintf(stderr, "swov: moved to the %s of everything there\n", edge);
    }

    /* and then next to the window it was dropped on, splitting the way the
     * pointer said — the same three commands a drag inside swov uses */
    if (ok && beside > 0 && edge) {
        bool horiz = !strcmp(edge, "left") || !strcmp(edge, "right");
        sway_cmd("[con_id=%d] mark --add _swov_drop", beside);
        sway_cmd("[con_id=%d] split %s", beside, horiz ? "h" : "v");
        sway_cmd("[con_id=%d] move container to mark _swov_drop", con_id);
        if (!strcmp(edge, "left") || !strcmp(edge, "top"))
            sway_cmd("[con_id=%d] move %s", con_id, horiz ? "left" : "up");
        sway_cmd("unmark _swov_drop");
        if (ADOPT_LOG) fprintf(stderr, "swov: placed %s of con_id %d\n", edge, beside);
    }

    if (ok && focus) {
        if (str_all_digits(ws)) sway_cmd("workspace number %s", ws);
        else {
            char *e = escape_arg(ws);
            sway_cmd("workspace --no-auto-back-and-forth \"%s\"", e);
            free(e);
        }
    }
    return ok ? 0 : 1;
}

/* --------------------------------------------------------------- globals */

static Cfg           C;

/* ------------------------------------------------------- cpu per workspace
 * swbr measures it — a rate needs two samples seconds apart, and swov is only
 * on screen for a moment — and leaves the answer here. If swbr is not running
 * the file is stale or missing and nothing is drawn. */
typedef struct { char name[64]; float cores; } CpuWs;
static CpuWs  CPU[64];
static int    NCPU;

static time_t CPU_MTIME;

static void cpu_load(void)
{
    NCPU = 0;
    if (!C.cpu) return;

    char path[512];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(path, sizeof(path), "%s/swbr-cpu", rt);
    else {
        const char *home = getenv("HOME");
        if (!home) return;
        snprintf(path, sizeof(path), "%s/.cache/swbr-cpu", home);
    }

    struct stat st;
    if (stat(path, &st) != 0) return;
    CPU_MTIME = st.st_mtime;
    if (time(NULL) - st.st_mtime > 30) return;     /* nobody is measuring */

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (NCPU < (int)SDL_arraysize(CPU) && fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        str_set(CPU[NCPU].name, sizeof(CPU[0].name), line);
        CPU[NCPU].cores = (float)atof(tab + 1);
        NCPU++;
    }
    fclose(f);
}

/* how busy, 0..1, or -1 when there is nothing worth drawing.
 * The number swbr leaves behind is in cores: 1.0 is one core kept busy. */
static float cpu_frac(const char *ws_name)
{
    for (int i = 0; i < NCPU; ++i) {
        if (strcmp(CPU[i].name, ws_name)) continue;
        if (CPU[i].cores < C.cpu_idle) return -1.0f;   /* genuinely nothing */
        if (CPU[i].cores < C.cpu_min)  return 0.0f;    /* a hint, one dot   */
        float span = C.cpu_full - C.cpu_min;
        float f = span > 0.0f ? (CPU[i].cores - C.cpu_min) / span : 1.0f;
        return SDL_clamp(f, 0.0f, 1.0f);
    }
    return -1.0f;
}
static SDL_Renderer *REN;
static TTF_Font     *F_BADGE;   /* workspace number            */
static TTF_Font     *F_LABEL;   /* app name on a window card   */
static TTF_Font     *F_TITLE;   /* window title (small)        */
static TTF_Font     *F_HINT;    /* header / footer             */
static float         SC = 1.0f; /* supersampling scale factor  */

static int px(float logical) { return (int)(logical * SC + 0.5f); }

/* ----------------------------------------------------------------- text */

typedef struct { SDL_Texture *t; int w, h; } Tex;

static void tex_free(Tex *x)
{
    if (x->t) SDL_DestroyTexture(x->t);
    x->t = NULL;
    x->w = x->h = 0;
}

/* Text is always rendered white and tinted at draw time, so one texture can
 * be reused for normal / hovered / selected states. */
static Tex text_make(TTF_Font *f, const char *s)
{
    Tex out = {0};
    if (!f || !s || !*s) return out;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surf = TTF_RenderText_Blended(f, s, strlen(s), white);
    if (!surf) return out;
    out.t = SDL_CreateTextureFromSurface(REN, surf);
    out.w = surf->w;
    out.h = surf->h;
    SDL_DestroySurface(surf);
    if (out.t) SDL_SetTextureScaleMode(out.t, SDL_SCALEMODE_LINEAR);
    return out;
}

/* Render `s`, shortened with an ellipsis so that it fits into max_w pixels. */
static Tex text_make_fit(TTF_Font *f, const char *s, int max_w)
{
    Tex out = {0};
    if (!f || !s || !*s || max_w <= 0) return out;

    int w = 0, h = 0;
    if (TTF_GetStringSize(f, s, strlen(s), &w, &h) && w <= max_w)
        return text_make(f, s);

    const char *ell = "\xe2\x80\xa6";           /* … */
    int ew = 0, eh = 0;
    TTF_GetStringSize(f, ell, strlen(ell), &ew, &eh);

    int avail = max_w - ew;
    if (avail <= 0) return out;

    int mw = 0;
    size_t fit = 0;
    if (!TTF_MeasureString(f, s, strlen(s), avail, &mw, &fit) || fit == 0)
        return out;

    char *buf = (char *)xmalloc(fit + strlen(ell) + 1);
    memcpy(buf, s, fit);
    memcpy(buf + fit, ell, strlen(ell) + 1);
    out = text_make(f, buf);
    free(buf);
    return out;
}

static void tex_draw(Tex x, float dx, float dy, SDL_FColor col)
{
    if (!x.t) return;
    SDL_SetTextureColorModFloat(x.t, col.r, col.g, col.b);
    SDL_SetTextureAlphaModFloat(x.t, col.a);
    SDL_FRect dst = { dx, dy, (float)x.w, (float)x.h };
    SDL_RenderTexture(REN, x.t, NULL, &dst);
}

static void tex_draw_center(Tex x, float cx, float dy, SDL_FColor col)
{
    tex_draw(x, cx - x.w * 0.5f, dy, col);
}

/* Centre a line inside a box. A text texture is a full line box: it reserves
 * room for descenders even when the string has none, so plain centring makes
 * digits and capitals look too high. Half the descent puts them right. */
static void tex_draw_in_box(Tex x, SDL_FRect box, TTF_Font *f, SDL_FColor col)
{
    if (!x.t) return;
    float desc = f ? (float)-TTF_GetFontDescent(f) : 0.0f;
    float y = box.y + (box.h - (float)x.h) * 0.5f - desc * 0.5f;
    tex_draw(x, box.x + (box.w - (float)x.w) * 0.5f, y, col);
}

/* ------------------------------------------------------------- primitives */

static void set_col(SDL_FColor c)
{
    SDL_SetRenderDrawColorFloat(REN, c.r, c.g, c.b, c.a);
}

static void fill_round_rect(SDL_FRect r, float rad, SDL_FColor c)
{
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    rad = SDL_min(rad, SDL_min(r.w, r.h) * 0.5f);
    set_col(c);
    if (rad <= 0.5f) { SDL_RenderFillRect(REN, &r); return; }

    SDL_FRect mid = { r.x, r.y + rad, r.w, r.h - 2.0f * rad };
    SDL_RenderFillRect(REN, &mid);

    int steps = (int)SDL_ceilf(rad);
    for (int i = 0; i < steps; ++i) {
        float dy = rad - (float)i - 0.5f;
        float dx = SDL_sqrtf(SDL_max(0.0f, rad * rad - dy * dy));
        float x0 = r.x + rad - dx;
        float w  = r.w - 2.0f * rad + 2.0f * dx;
        SDL_FRect top = { x0, r.y + (float)i, w, 1.0f };
        SDL_FRect bot = { x0, r.y + r.h - (float)i - 1.0f, w, 1.0f };
        SDL_RenderFillRect(REN, &top);
        SDL_RenderFillRect(REN, &bot);
    }
}

/* The same, with the top or the bottom left square.
 *
 * A tab meets the panel below it and a panel meets the tab above it: rounding
 * both sides of that join leaves a pinch of background showing through, which
 * is what made a tabbed workspace look wrong. */
static void fill_round_side(SDL_FRect r, float rad, bool round_top,
                            bool round_bot, SDL_FColor c)
{
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    if (round_top && round_bot) { fill_round_rect(r, rad, c); return; }

    rad = SDL_min(rad, SDL_min(r.w, r.h) * 0.5f);
    if (rad <= 0.5f) { set_col(c); SDL_RenderFillRect(REN, &r); return; }

    set_col(c);
    SDL_FRect mid = { r.x, r.y + (round_top ? rad : 0.0f), r.w,
                      r.h - (round_top ? rad : 0.0f) - (round_bot ? rad : 0.0f) };
    SDL_RenderFillRect(REN, &mid);

    int steps = (int)SDL_ceilf(rad);
    for (int i = 0; i < steps; ++i) {
        float dy = rad - (float)i - 0.5f;
        float dx = SDL_sqrtf(SDL_max(0.0f, rad * rad - dy * dy));
        float x0 = r.x + rad - dx;
        float w  = r.w - 2.0f * rad + 2.0f * dx;
        if (round_top) {
            SDL_FRect t = { x0, r.y + (float)i, w, 1.0f };
            SDL_RenderFillRect(REN, &t);
        }
        if (round_bot) {
            SDL_FRect b = { x0, r.y + r.h - (float)i - 1.0f, w, 1.0f };
            SDL_RenderFillRect(REN, &b);
        }
    }
}

/* rounded rectangle outline of thickness t, drawn inside r */
static void stroke_round_rect(SDL_FRect r, float rad, float t, SDL_FColor c)
{
    if (t <= 0.0f || r.w <= 0.0f || r.h <= 0.0f) return;
    rad = SDL_min(rad, SDL_min(r.w, r.h) * 0.5f);
    t   = SDL_min(t, SDL_min(r.w, r.h) * 0.5f);
    set_col(c);

    if (rad <= 0.5f) {
        SDL_FRect e[4] = {
            { r.x, r.y, r.w, t },
            { r.x, r.y + r.h - t, r.w, t },
            { r.x, r.y + t, t, r.h - 2.0f * t },
            { r.x + r.w - t, r.y + t, t, r.h - 2.0f * t }
        };
        SDL_RenderFillRects(REN, e, 4);
        return;
    }

    SDL_FRect edges[4] = {
        { r.x + rad, r.y, r.w - 2.0f * rad, t },                        /* top    */
        { r.x + rad, r.y + r.h - t, r.w - 2.0f * rad, t },              /* bottom */
        { r.x, r.y + rad, t, r.h - 2.0f * rad },                        /* left   */
        { r.x + r.w - t, r.y + rad, t, r.h - 2.0f * rad }               /* right  */
    };
    SDL_RenderFillRects(REN, edges, 4);

    float ri = rad - t;
    int steps = (int)SDL_ceilf(rad);
    for (int i = 0; i < steps; ++i) {
        float dy  = rad - (float)i - 0.5f;
        float dxo = SDL_sqrtf(SDL_max(0.0f, rad * rad - dy * dy));
        float dxi = (ri > 0.0f && SDL_fabsf(dy) < ri)
                        ? SDL_sqrtf(SDL_max(0.0f, ri * ri - dy * dy)) : 0.0f;
        float seg = dxo - dxi;
        if (seg <= 0.0f) continue;

        float lx = r.x + rad - dxo;
        float rx = r.x + r.w - rad + dxi;
        float ty = r.y + (float)i;
        float by = r.y + r.h - (float)i - 1.0f;

        SDL_FRect q[4] = {
            { lx, ty, seg, 1.0f },
            { rx, ty, seg, 1.0f },
            { lx, by, seg, 1.0f },
            { rx, by, seg, 1.0f }
        };
        SDL_RenderFillRects(REN, q, 4);
    }
}

/* soft drop shadow: a few expanded, fading rounded rects */
static void drop_shadow(SDL_FRect r, float rad, float size, SDL_FColor c)
{
    if (!C.shadow || size <= 0.0f) return;
    int layers = C.shadow_layers > 0 ? C.shadow_layers : 3;
    for (int i = layers; i >= 1; --i) {
        float g = size * (float)i / (float)layers;
        SDL_FRect s = { r.x - g, r.y - g, r.w + 2.0f * g, r.h + 2.0f * g };
        SDL_FColor col = with_alpha(c, c.a / (float)(layers * 2));
        fill_round_rect(s, rad + g, col);
    }
}

/* ------------------------------------------------------------ icon lookup
 * app_id (or WM_CLASS) -> .desktop file -> Icon= -> a file on disk.
 * Everything is cached: the .desktop index is built once and lazily, the
 * resulting textures are shared between windows of the same application.
 */

typedef struct {
    char *id;        /* desktop file basename without .desktop */
    char *path;
    char *wmclass;   /* StartupWMClass, may be NULL */
    char *icon;      /* Icon=, may be NULL */
    char *name;      /* Name=, may be NULL */
} DesktopEntry;

static DesktopEntry *g_desktops = NULL;
static int  g_desktop_count = 0;
static bool g_desktop_indexed = false;

static void desktop_scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_desktop_count < MAX_DESKTOPS) {
        const char *n = de->d_name;
        size_t len = strlen(n);
        if (len < 9 || strcmp(n + len - 8, ".desktop") != 0) continue;

        char *path = fmt_alloc("%s/%s", dir, n);
        FILE *f = fopen(path, "r");
        if (!f) { free(path); continue; }

        char *icon = NULL, *wmclass = NULL, *dname = NULL;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (!icon && strncmp(line, "Icon=", 5) == 0) {
                char *v = str_trim(line + 5);
                if (*v) icon = xstrdup(v);
            } else if (!wmclass && strncmp(line, "StartupWMClass=", 15) == 0) {
                char *v = str_trim(line + 15);
                if (*v) wmclass = xstrdup(v);
            } else if (!dname && strncmp(line, "Name=", 5) == 0) {
                char *v = str_trim(line + 5);
                if (*v) dname = xstrdup(v);
            }
            if (icon && wmclass && dname) break;
        }
        fclose(f);

        if (!icon && !dname) { free(wmclass); free(path); continue; }

        DesktopEntry *e = &g_desktops[g_desktop_count++];
        e->path    = path;
        e->icon    = icon;
        e->wmclass = wmclass;
        e->name    = dname;
        e->id      = xstrdup(n);
        e->id[len - 8] = 0;
    }
    closedir(d);
}

static void desktop_index_build(void)
{
    if (g_desktop_indexed) return;
    g_desktop_indexed = true;
    g_desktops = (DesktopEntry *)xmalloc(sizeof(DesktopEntry) * MAX_DESKTOPS);

    const char *home = getenv("HOME");
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");

    char *dirs[16];
    int nd = 0;

    if (xdg_data_home && *xdg_data_home)
        dirs[nd++] = fmt_alloc("%s/applications", xdg_data_home);
    else if (home)
        dirs[nd++] = fmt_alloc("%s/.local/share/applications", home);
    if (home) dirs[nd++] = fmt_alloc("%s/.local/share/flatpak/exports/share/applications", home);

    if (xdg_data_dirs && *xdg_data_dirs) {
        char *copy = xstrdup(xdg_data_dirs);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ":", &save); tok && nd < 14; tok = strtok_r(NULL, ":", &save))
            if (*tok) dirs[nd++] = fmt_alloc("%s/applications", tok);
        free(copy);
    } else {
        dirs[nd++] = xstrdup("/usr/local/share/applications");
        dirs[nd++] = xstrdup("/usr/share/applications");
    }
    if (nd < 15) dirs[nd++] = xstrdup("/var/lib/flatpak/exports/share/applications");

    for (int i = 0; i < nd; ++i) {
        if (is_dir(dirs[i])) desktop_scan_dir(dirs[i]);
        free(dirs[i]);
    }
}

/* org.qutebrowser.qutebrowser -> qutebrowser, org.telegram.desktop -> telegram */
static const char *short_app_name(const char *app_id)
{
    static const char *generic[] = { "desktop", "app", "App", "gui", "GUI", "client",
                                     "Client", "bin", "Bin", "gtk", "qt", "Desktop" };
    if (!app_id || !*app_id) return "unknown";

    const char *dot = strrchr(app_id, '.');
    if (!dot || !dot[1] || strchr(app_id, '.') == dot) return app_id;

    for (size_t i = 0; i < SDL_arraysize(generic); ++i) {
        if (ci_cmp(dot + 1, generic[i]) != 0) continue;
        /* the last part says nothing: use the one before it */
        static char buf[128];
        const char *p = dot - 1;
        while (p > app_id && *p != '.') p--;
        if (*p == '.') p++;
        size_t n = (size_t)(dot - p);
        if (n == 0 || n >= sizeof(buf)) break;
        memcpy(buf, p, n);
        buf[n] = 0;
        return buf;
    }
    return dot + 1;
}


static const DesktopEntry *desktop_find(const char *app_id)
{
    if (!app_id || !*app_id || !C.icons) return NULL;
    desktop_index_build();

    for (int i = 0; i < g_desktop_count; ++i)                    /* exact id     */
        if (ci_cmp(g_desktops[i].id, app_id) == 0) return &g_desktops[i];
    for (int i = 0; i < g_desktop_count; ++i)                    /* WM_CLASS     */
        if (g_desktops[i].wmclass && ci_cmp(g_desktops[i].wmclass, app_id) == 0)
            return &g_desktops[i];

    const char *shrt = short_app_name(app_id);
    for (int i = 0; i < g_desktop_count; ++i)                    /* short name   */
        if (ci_cmp(g_desktops[i].id, shrt) == 0) return &g_desktops[i];
    for (int i = 0; i < g_desktop_count; ++i) {                  /* id suffix    */
        const char *dot = strrchr(g_desktops[i].id, '.');
        if (dot && ci_cmp(dot + 1, shrt) == 0) return &g_desktops[i];
    }
    return NULL;
}

/* Some toolkits hand sway an app_id that identifies nothing: GTK apps
 * launched without one report "GTK Application", and several Electron
 * builds are no better. In that case the process behind the window knows
 * more than the window does. */
static bool app_id_is_useless(const char *id)
{
    static const char *bad[] = {
        "", "gtk application", "gtk-application", "unknown", "wayland",
        "xwayland", "electron", "toplevel", "window", "main", "app"
    };
    if (!id) return true;
    for (size_t i = 0; i < SDL_arraysize(bad); ++i)
        if (ci_cmp(id, bad[i]) == 0) return true;
    return false;
}

static bool is_interpreter(const char *name)
{
    static const char *interp[] = {
        "electron", "node", "python", "python3", "java", "sh", "bash", "zsh",
        "wine", "wine64", "wine-preloader", "flatpak", "snap", "env", "steam"
    };
    for (size_t i = 0; i < SDL_arraysize(interp); ++i)
        if (ci_cmp(name, interp[i]) == 0) return true;
    return false;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* argv[0] of the process, or its comm name; empty when nothing usable */
static void proc_app_name(int pid, char *out, size_t cap)
{
    out[0] = 0;
    if (pid <= 1) return;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        if (n) {
            const char *arg0 = base_name(buf);
            if (*arg0 && !is_interpreter(arg0)) {
                str_set(out, cap, arg0);
                return;
            }
            /* an interpreter: the script or the app directory names it */
            size_t off = strlen(buf) + 1;
            while (off < n && out[0] == 0) {
                const char *arg = buf + off;
                if (*arg != '-') {
                    const char *b = base_name(arg);
                    if (*b && !is_interpreter(b) && !strchr(b, '=')) {
                        str_set(out, cap, b);
                        break;
                    }
                }
                off += strlen(arg) + 1;
            }
            if (out[0]) return;
        }
    }

    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    f = fopen(path, "r");
    if (!f) return;
    char comm[128] = {0};
    if (fgets(comm, sizeof(comm), f)) {
        str_trim(comm);
        if (comm[0] && !is_interpreter(comm)) str_set(out, cap, comm);
    }
    fclose(f);
}

/* what we print on a card: the .desktop Name= if we have one */
static const char *app_display_name(const char *app_id)
{
    const DesktopEntry *de = desktop_find(app_id);
    if (de && de->name && *de->name) return de->name;
    return short_app_name(app_id);
}

static char *icon_theme_name(void)
{
    const char *home = getenv("HOME");
    if (home) {
        char *p = fmt_alloc("%s/.config/gtk-3.0/settings.ini", home);
        FILE *f = fopen(p, "r");
        free(p);
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "gtk-icon-theme-name", 19) == 0) {
                    char *eq = strchr(line, '=');
                    if (eq) {
                        char *v = str_trim(eq + 1);
                        if (*v) { fclose(f); return xstrdup(v); }
                    }
                }
            }
            fclose(f);
        }
    }
    return xstrdup("hicolor");
}

/* The theme directories are scanned once, in preference order. Looking an
 * icon up afterwards is a handful of access() calls instead of the thousands
 * a blind size/extension sweep would cost. */
typedef struct { char *path; int size; } IconDir;
static IconDir *g_icon_dirs = NULL;
static int  g_icon_dir_count = 0, g_icon_dir_cap = 0;
static bool g_icon_dirs_built = false;

static void icon_dir_add(const char *path, int size)
{
    if (!is_dir(path)) return;
    if (g_icon_dir_count == g_icon_dir_cap) {
        g_icon_dir_cap = g_icon_dir_cap ? g_icon_dir_cap * 2 : 32;
        g_icon_dirs = (IconDir *)xrealloc(g_icon_dirs, (size_t)g_icon_dir_cap * sizeof(IconDir));
    }
    g_icon_dirs[g_icon_dir_count].path = xstrdup(path);
    g_icon_dirs[g_icon_dir_count].size = size;
    g_icon_dir_count++;
}

static int cmp_icon_dir(const void *A, const void *B)
{
    const IconDir *a = (const IconDir *)A, *b = (const IconDir *)B;
    if (a->size != b->size) {
        if (a->size == 0) return -1;           /* scalable wins: crisp at any size */
        if (b->size == 0) return 1;
        return b->size - a->size;              /* then big before small */
    }
    return 0;
}

static void icon_dirs_build(void)
{
    if (g_icon_dirs_built) return;
    g_icon_dirs_built = true;

    static char *theme = NULL;
    if (!theme) theme = icon_theme_name();

    const char *home = getenv("HOME");
    char *roots[4];
    int nr = 0;
    if (home) {
        roots[nr++] = fmt_alloc("%s/.local/share/icons", home);
        roots[nr++] = fmt_alloc("%s/.icons", home);
    }
    roots[nr++] = xstrdup("/usr/share/icons");
    roots[nr++] = xstrdup("/usr/local/share/icons");

    const char *themes[] = { theme, "Adwaita", "breeze", "Papirus", "hicolor" };
    const char *cats[]   = { "apps", "applications" };

    for (int r = 0; r < nr; ++r) {
        for (size_t t = 0; t < SDL_arraysize(themes); ++t) {
            if (!themes[t] || !*themes[t]) continue;
            char *tdir = fmt_alloc("%s/%s", roots[r], themes[t]);
            DIR *d = opendir(tdir);
            if (!d) { free(tdir); continue; }

            struct dirent *de;
            while ((de = readdir(d))) {
                const char *n = de->d_name;
                if (n[0] == '.') continue;
                int size = -1;
                if (strcmp(n, "scalable") == 0) size = 0;
                else if (isdigit((unsigned char)n[0])) size = atoi(n);   /* 48x48 */
                if (size < 0) continue;
                if (strstr(n, "@2x")) continue;                          /* skip hidpi */
                for (size_t c = 0; c < SDL_arraysize(cats); ++c) {
                    char *p = fmt_alloc("%s/%s/%s", tdir, n, cats[c]);
                    icon_dir_add(p, size);
                    free(p);
                }
            }
            closedir(d);
            free(tdir);
        }
    }
    for (int i = 0; i < nr; ++i) free(roots[i]);

    qsort(g_icon_dirs, (size_t)g_icon_dir_count, sizeof(IconDir), cmp_icon_dir);
    icon_dir_add("/usr/share/pixmaps", 0);
}

static char *icon_lookup(const char *name, int want_px)
{
    (void)want_px;
    if (!name || !*name) return NULL;
    if (name[0] == '/') return file_readable(name) ? xstrdup(name) : NULL;

    icon_dirs_build();

    const char *exts[] = { ".svg", ".png", ".xpm" };
    bool has_ext = strstr(name, ".png") || strstr(name, ".svg") || strstr(name, ".xpm");

    for (int d = 0; d < g_icon_dir_count; ++d) {
        for (size_t e = 0; e < SDL_arraysize(exts); ++e) {
            char *p = fmt_alloc("%s/%s%s", g_icon_dirs[d].path, name, has_ext ? "" : exts[e]);
            if (file_readable(p)) return p;
            free(p);
            if (has_ext) break;
        }
    }
    return NULL;
}

static SDL_Texture *icon_load_file(const char *path, int want_px)
{
    SDL_Surface *surf = NULL;
    size_t n = strlen(path);
    if (n > 4 && ci_cmp(path + n - 4, ".svg") == 0) {
        SDL_IOStream *io = SDL_IOFromFile(path, "rb");
        if (io) {
            surf = IMG_LoadSizedSVG_IO(io, want_px, want_px);
            SDL_CloseIO(io);
        }
    }
    if (!surf) surf = IMG_Load(path);
    if (!surf) return NULL;

    SDL_Texture *t = SDL_CreateTextureFromSurface(REN, surf);
    SDL_DestroySurface(surf);
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    return t;
}

/* a rounded tile with the first letter, used when no icon is found */
static SDL_Texture *icon_make_letter(const char *app_id, int want_px)
{
    const char *nm = short_app_name(app_id);
    char letter[8] = {0};
    /* copy one UTF-8 code point, upper-cased if ASCII */
    unsigned char c0 = (unsigned char)nm[0];
    int clen = (c0 < 0x80) ? 1 : (c0 < 0xe0) ? 2 : (c0 < 0xf0) ? 3 : 4;
    memcpy(letter, nm, (size_t)clen);
    if (clen == 1) letter[0] = (char)toupper(c0);

    SDL_Texture *t = SDL_CreateTexture(REN, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_TARGET, want_px, want_px);
    if (!t) return NULL;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);

    SDL_Texture *prev = SDL_GetRenderTarget(REN);
    SDL_SetRenderTarget(REN, t);
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_NONE);
    set_col((SDL_FColor){0, 0, 0, 0});
    SDL_RenderClear(REN);
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);

    SDL_FRect box = { 0, 0, (float)want_px, (float)want_px };
    fill_round_rect(box, want_px * 0.24f, mix(C.hl, C.tile, 0.55f));
    stroke_round_rect(box, want_px * 0.24f, SDL_max(1.0f, want_px * 0.05f),
                      with_alpha(C.hl, 0.85f));

    Tex g = text_make(F_LABEL, letter);
    if (g.t) {
        float s = SDL_min((float)want_px * 0.62f / SDL_max(1, g.h), 1.6f);
        SDL_FRect dst = { (want_px - g.w * s) * 0.5f, (want_px - g.h * s) * 0.5f,
                          g.w * s, g.h * s };
        SDL_SetTextureColorModFloat(g.t, C.text.r, C.text.g, C.text.b);
        SDL_RenderTexture(REN, g.t, NULL, &dst);
        tex_free(&g);
    }

    SDL_SetRenderTarget(REN, prev);
    return t;
}

typedef struct { char key[192]; SDL_Texture *tex; } IconCacheEntry;
static IconCacheEntry g_icons[256];
static int g_icon_count = 0;

static SDL_Texture *icon_for_app(const char *app_id, int want_px)
{
    if (!C.icons) return NULL;
    const char *key = (app_id && *app_id) ? app_id : "unknown";

    for (int i = 0; i < g_icon_count; ++i)
        if (strcmp(g_icons[i].key, key) == 0) return g_icons[i].tex;

    SDL_Texture *tex = NULL;
    const DesktopEntry *de = desktop_find(key);
    if (de && de->icon) {
        char *path = icon_lookup(de->icon, want_px);
        if (path) { tex = icon_load_file(path, want_px); free(path); }
    }
    if (!tex) {                                   /* app_id often *is* the icon */
        char *path = icon_lookup(key, want_px);
        if (!path) path = icon_lookup(short_app_name(key), want_px);
        if (path) { tex = icon_load_file(path, want_px); free(path); }
    }
    if (!tex) tex = icon_make_letter(key, want_px);

    if (g_icon_count < (int)SDL_arraysize(g_icons)) {
        str_set(g_icons[g_icon_count].key, sizeof(g_icons[0].key), key);
        g_icons[g_icon_count].tex = tex;
        g_icon_count++;
    }
    return tex;
}

static void icons_free(void)
{
    for (int i = 0; i < g_icon_count; ++i)
        if (g_icons[i].tex) SDL_DestroyTexture(g_icons[i].tex);
    g_icon_count = 0;

    for (int i = 0; i < g_desktop_count; ++i) {
        free(g_desktops[i].id);
        free(g_desktops[i].path);
        free(g_desktops[i].icon);
        free(g_desktops[i].wmclass);
        free(g_desktops[i].name);
    }
    for (int i = 0; i < g_icon_dir_count; ++i) free(g_icon_dirs[i].path);
    free(g_icon_dirs);
    g_icon_dirs = NULL;
    g_icon_dir_count = g_icon_dir_cap = 0;
    g_icon_dirs_built = false;

    free(g_desktops);
    g_desktops = NULL;
    g_desktop_count = 0;
    g_desktop_indexed = false;
}

/* ----------------------------------------------------------------- model */

typedef struct {
    int   con_id;
    int   pid;
    char  app_id[128];      /* what sway reports                            */
    char  app_key[128];     /* what we look the icon and the name up with   */
    char  title[512];
    char  ws_name[64];
    char  output[64];

    int   x, y, w, h;          /* absolute geometry, as sway reports it */
    bool  floating, focused, urgent, fullscreen, sticky;
    bool  is_self;             /* swov's own window, or its backdrop         */
    bool  marked;              /* multi-selection */
    bool  match;               /* passes the current filter */
    bool  visible;             /* the one on top of its tabbed container */
    SDL_FRect tab;             /* its slot in the container's tab strip   */
    bool  has_tab;

    int   ws;                  /* index into WSS */
    SDL_FRect card;            /* where it is drawn (render pixels) */
    SDL_FRect hit;             /* what the mouse acts on: the whole card, or
                                * just the name plate of a floating window   */
    int   group;               /* head index of a tabbed/stacked group, or -1 */

    int   lay_mode;            /* how the card contents are arranged */
    float lay_icon;            /* icon edge length on this card */
    bool  lay_sub;             /* is there room for the title */

    SDL_Texture *icon;         /* borrowed from the icon cache */
    Tex   label, subtitle;
} Win;

typedef struct {
    char  name[64];      /* what sway calls it: "3" or "3:code"           */
    char  label[64];     /* just the name part, "" when it is only a number */
    char  output[64];
    double usage;        /* seconds spent here, from the tracker            */
    int   num;
    bool  focused, visible, urgent;
    int   rx, ry, rw, rh;      /* workspace rect */

    int   first, count;        /* range in WINS */
    int   sel;                 /* selected window, relative to first; -1 = the
                                * workspace itself is selected             */
    int   top[64];             /* top level containers, in order           */
    int   ntop;

    SDL_FRect tile, screen;    /* layout (render pixels) */
    SDL_FRect title_box;       /* where the name is drawn in the header      */
    SDL_FRect title_hit;       /* its middle half: clicking there renames.
                                * The quarter to the left and to the right
                                * selects the workspace, like the tile. */
    SDL_FRect tile_from;       /* where this tile animates from */
    Tex   badge, sub, title;
} Ws;

static Win WINS[MAX_WINDOWS];
static int NWIN = 0;
static Ws  WSS[MAX_WORKSPACES];
static int NWS = 0;

static bool DRAG_ALL_OUTPUTS = false;   /* mid-drag: every screen shows */
static char FOCUSED_OUTPUT[64] = {0};   /* the output whose workspaces we show */
static char HOME_OUTPUT[64]    = {0};   /* the one sway is really on           */
static bool PINNED_OUTPUT;              /* ...and we are looking elsewhere     */

/* Every monitor, in the arrangement sway has them in, so the overview can
 * draw a little map of the desk and let you step onto another screen. */
typedef struct {
    char name[64];
    int  x, y, w, h;
    bool focused;
    SDL_FRect box;                      /* where it sits in the map */
} Out;
static Out  OUTS[8];
static int  NOUTS;
static bool dirty   = true;      /* a frame is only drawn when this is set */
static SDL_FRect MAP_RECT;              /* the corner it is drawn in */
static SDL_FRect CANCEL_RECT;           /* and the ✕ beside it, while dragging */

/* Holding an app over another monitor for a moment steps the overview onto
 * it, so a drag that started on one screen can finish on another. It is a
 * press you hold, so it shows how far along it is. */
static int    MAP_HOVER = -1;
static int    DRAG_CON;          /* the window being dragged, by id */
static double MAP_HOVER_SINCE;
static float  MAP_HOVER_X, MAP_HOVER_Y;

static void outputs_reload(void)
{
    NOUTS = 0;
    JV *outs = sway_query(IPC_GET_OUTPUTS);
    if (!outs || outs->type != J_ARR) { jfree(outs); return; }

    for (int i = 0; i < outs->count && NOUTS < (int)SDL_arraysize(OUTS); ++i) {
        const JV *o = outs->items[i];
        if (!jbool(o, "active", true)) continue;
        const JV *r = jget(o, "rect");
        Out *d = &OUTS[NOUTS++];
        str_set(d->name, sizeof(d->name), jstr(o, "name", ""));
        d->x = jint(r, "x", 0);      d->y = jint(r, "y", 0);
        d->w = jint(r, "width", 0);  d->h = jint(r, "height", 0);
        d->focused = jbool(o, "focused", false);
        if (d->focused) str_set(HOME_OUTPUT, sizeof(HOME_OUTPUT), d->name);
        d->box = (SDL_FRect){ 0, 0, 0, 0 };
    }
    jfree(outs);
}
static char CUR_OUTPUT[64]     = {0};   /* output the tree walk is inside */

static void model_free(void)
{
    for (int i = 0; i < NWIN; ++i) { tex_free(&WINS[i].label); tex_free(&WINS[i].subtitle); }
    for (int i = 0; i < NWS; ++i)  {
        tex_free(&WSS[i].badge);
        tex_free(&WSS[i].sub);
        tex_free(&WSS[i].title);
    }
    NWIN = NWS = 0;
}

static bool node_is_view(const JV *n)
{
    if (jget(n, "app_id") && jget(n, "app_id")->type == J_STR) return true;
    const JV *wp = jget(n, "window_properties");
    return wp && jget(wp, "class") && jget(wp, "class")->type == J_STR;
}

static void collect_views(const JV *node, Ws *ws, bool floating)
{
    if (!node || node->type != J_OBJ) return;

    if (node_is_view(node) && NWIN < MAX_WINDOWS) {
        Win *w = &WINS[NWIN];
        memset(w, 0, sizeof(*w));

        const char *app = jstr(node, "app_id", NULL);
        if (!app) {
            const JV *wp = jget(node, "window_properties");
            app = wp ? jstr(wp, "class", NULL) : NULL;
        }
        str_set(w->app_id, sizeof(w->app_id), app ? app : "unknown");
        memcpy(w->app_key, w->app_id, sizeof(w->app_key));
        str_set(w->title, sizeof(w->title), jstr(node, "name", ""));
        str_set(w->ws_name, sizeof(w->ws_name), ws->name);
        str_set(w->output, sizeof(w->output), ws->output);

        w->con_id     = jint(node, "id", 0);
        w->pid        = jint(node, "pid", 0);
        /* This very window, and the backdrop behind it. It is on the
         * workspace like anything else and hiding it would leave a hole, but
         * it must not be selectable, droppable or in the way: it is about to
         * be gone. */
        w->is_self    = w->pid == getpid() ||
                        !strncmp(w->app_id, APP_ID, sizeof(APP_ID) - 1);
        w->focused    = jbool(node, "focused", false);
        w->visible    = jbool(node, "visible", true);
        w->urgent     = jbool(node, "urgent", false);
        w->sticky     = jbool(node, "sticky", false);
        w->floating   = floating;
        w->fullscreen = jint(node, "fullscreen_mode", 0) != 0;
        w->match      = true;
        w->ws         = (int)(ws - WSS);

        const JV *r = jget(node, "rect");
        w->x = jint(r, "x", ws->rx);
        w->y = jint(r, "y", ws->ry);
        w->w = jint(r, "width",  100);
        w->h = jint(r, "height", 100);
        if (w->w <= 0) w->w = 100;
        if (w->h <= 0) w->h = 100;

        NWIN++;
        ws->count++;
        return;                                  /* views have no children */
    }

    const JV *nodes = jget(node, "nodes");
    if (nodes && nodes->type == J_ARR)
        for (int i = 0; i < nodes->count; ++i) collect_views(nodes->items[i], ws, floating);

    const JV *fl = jget(node, "floating_nodes");
    if (fl && fl->type == J_ARR)
        for (int i = 0; i < fl->count; ++i) collect_views(fl->items[i], ws, true);
}

static int cmp_win_pos(const void *A, const void *B)
{
    const Win *a = (const Win *)A, *b = (const Win *)B;
    bool af = a->floating || a->fullscreen;      /* both are drawn on top */
    bool bf = b->floating || b->fullscreen;
    if (af != bf) return af ? 1 : -1;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return a->con_id - b->con_id;
}

static void walk_outputs(const JV *node)
{
    if (!node || node->type != J_OBJ) return;
    const char *type = jstr(node, "type", "");

    if (strcmp(type, "workspace") == 0) {
        const char *name = jstr(node, "name", "?");
        if (strcmp(name, "__i3_scratchpad") == 0) return;
        if (NWS >= MAX_WORKSPACES) return;

        Ws *ws = &WSS[NWS];
        memset(ws, 0, sizeof(*ws));
        str_set(ws->name, sizeof(ws->name), name);
        str_set(ws->output, sizeof(ws->output), CUR_OUTPUT);
        ws->num     = jint(node, "num", str_all_digits(name) ? atoi(name) : -1);

        const char *colon = strchr(name, ':');       /* "3:code" -> "code" */
        if (colon) str_set(ws->label, sizeof(ws->label), colon + 1);
        else if (!str_all_digits(name)) str_set(ws->label, sizeof(ws->label), name);
        ws->focused = jbool(node, "focused", false);
        ws->urgent  = jbool(node, "urgent", false);

        const JV *r = jget(node, "rect");
        ws->rx = jint(r, "x", 0);
        ws->ry = jint(r, "y", 0);
        ws->rw = jint(r, "width",  1920);
        ws->rh = jint(r, "height", 1080);

        ws->first = NWIN;
        ws->count = 0;
        ws->sel   = -1;
        ws->ntop  = 0;
        NWS++;

        /* remember the direct children: moving those keeps their layout */
        const JV *kids[2] = { jget(node, "nodes"), jget(node, "floating_nodes") };
        for (int a = 0; a < 2; ++a)
            if (kids[a] && kids[a]->type == J_ARR)
                for (int b = 0; b < kids[a]->count && ws->ntop < 64; ++b) {
                    int id = jint(kids[a]->items[b], "id", 0);
                    if (id) ws->top[ws->ntop++] = id;
                }

        collect_views(node, ws, false);

        if (ws->count > 1)
            qsort(&WINS[ws->first], (size_t)ws->count, sizeof(Win), cmp_win_pos);
        for (int i = 0; i < ws->count; ++i) WINS[ws->first + i].ws = (int)(ws - WSS);
        return;
    }

    if (strcmp(type, "output") == 0) {
        const char *name = jstr(node, "name", "");
        if (strcmp(name, "__i3") == 0) return;
        if (!C.all_outputs && !DRAG_ALL_OUTPUTS &&
        FOCUSED_OUTPUT[0] && strcmp(name, FOCUSED_OUTPUT) != 0) return;
        str_set(CUR_OUTPUT, sizeof(CUR_OUTPUT), name);
    }

    const JV *nodes = jget(node, "nodes");
    if (nodes && nodes->type == J_ARR)
        for (int i = 0; i < nodes->count; ++i) walk_outputs(nodes->items[i]);
}

static int cmp_ws(const void *A, const void *B)
{
    const Ws *a = (const Ws *)A, *b = (const Ws *)B;
    int o = strcmp(a->output, b->output);
    if (o) return o;
    if (a->num >= 0 && b->num >= 0 && a->num != b->num) return a->num < b->num ? -1 : 1;
    if ((a->num >= 0) != (b->num >= 0)) return a->num >= 0 ? -1 : 1;
    return strcmp(a->name, b->name);
}

/* Fetch workspaces + tree and rebuild the model. Returns false on failure. */
static bool model_reload(void)
{
    model_free();

    char focused[64] = {0};
    JV *wsr = sway_query(IPC_GET_WORKSPACES);
    if (wsr && wsr->type == J_ARR) {
        for (int i = 0; i < wsr->count; ++i)
            if (jbool(wsr->items[i], "focused", false))
                str_set(focused, sizeof(focused), jstr(wsr->items[i], "output", ""));
    }

    /* sway reports no focused workspace for a moment after a rename; keeping
     * the previous output beats falling back to "every output" */
    if (focused[0] && !PINNED_OUTPUT)
        str_set(FOCUSED_OUTPUT, sizeof(FOCUSED_OUTPUT), focused);
    outputs_reload();

    JV *tree = sway_query(IPC_GET_TREE);
    if (!tree) { jfree(wsr); return false; }

    CUR_OUTPUT[0] = 0;
    walk_outputs(tree);

    /* per-output names + visible flags from get_workspaces */
    if (wsr && wsr->type == J_ARR) {
        for (int i = 0; i < NWS; ++i) {
            for (int j = 0; j < wsr->count; ++j) {
                const JV *o = wsr->items[j];
                if (strcmp(jstr(o, "name", ""), WSS[i].name) != 0) continue;
                str_set(WSS[i].output, sizeof(WSS[i].output), jstr(o, "output", ""));
                WSS[i].visible = jbool(o, "visible", false);
                WSS[i].focused = jbool(o, "focused", WSS[i].focused);
                WSS[i].urgent  = jbool(o, "urgent",  WSS[i].urgent);
                WSS[i].num     = jint(o, "num", WSS[i].num);
                break;
            }
        }
    }

    jfree(wsr);
    jfree(tree);

    /* drop empty workspaces if the user does not want them */
    if (!C.show_empty) {
        int k = 0;
        for (int i = 0; i < NWS; ++i)
            if (WSS[i].count > 0) WSS[k++] = WSS[i];
        NWS = k;
    }

    /* sorting workspaces moves them around; fix up window -> workspace links */
    if (NWS > 1) qsort(WSS, (size_t)NWS, sizeof(Ws), cmp_ws);
    for (int i = 0; i < NWS; ++i)
        for (int j = 0; j < WSS[i].count; ++j) WINS[WSS[i].first + j].ws = i;

    usage_load();
    double pending = usage_pending();
    bool   changed = false;

    for (int i = 0; i < NUSAGE; ) {                  /* forget what is gone */
        bool alive = false;
        for (int j = 0; j < NWS; ++j)
            if (strcmp(WSS[j].name, USAGE[i].name) == 0 && WSS[j].count > 0) alive = true;
        if (alive) { ++i; continue; }
        USAGE[i] = USAGE[--NUSAGE];                  /* an empty workspace resets */
        changed = true;
    }
    if (changed) usage_save();

    USAGE_MAX = 0.0;
    for (int i = 0; i < NWS; ++i) {
        Usage *u = usage_find(WSS[i].name, false);
        WSS[i].usage = u ? u->secs : 0.0;
        if (strcmp(WSS[i].name, USAGE_CUR) == 0) WSS[i].usage += pending;
        if (WSS[i].count == 0) WSS[i].usage = 0.0;   /* empty means start over */
        if (WSS[i].usage > USAGE_MAX) USAGE_MAX = WSS[i].usage;
    }

    for (int i = 0; i < NWIN; ++i) {
        Win *w = &WINS[i];
        if (app_id_is_useless(w->app_id)) {          /* ask the process instead */
            char nm[128];
            proc_app_name(w->pid, nm, sizeof(nm));
            if (nm[0]) str_set(w->app_key, sizeof(w->app_key), nm);
        }
        w->icon = icon_for_app(w->app_key, px((float)C.icon_px * 1.6f));
    }

    return true;
}

/* ---------------------------------------------------------------- layout */

typedef struct { int ws; int num; SDL_FRect tile, from; } Slot;  /* ws < 0 = ghost */
static Slot SLOTS[MAX_WORKSPACES + 16];
static int  NSLOTS;
static bool drag_ws_mode;         /* a workspace is being dragged right now  */
static int  ghost_lo = -1, ghost_hi = -1;   /* free numbers currently offered */

static Slot   DYING[16];          /* ghosts on their way out                 */
static int    NDYING;
static Uint64 dying_start;
static bool   laid_out_once;

static Uint64 anim_start;         /* tiles glide when the grid changes       */

static bool anim_running(void)
{
    if (C.anim_ms <= 0.0f) return false;
    if (anim_start && (float)(SDL_GetTicks() - anim_start) < C.anim_ms) return true;
    return NDYING > 0 && (float)(SDL_GetTicks() - dying_start) < C.anim_ms;
}

static float phase_since(Uint64 start)
{
    if (!start || C.anim_ms <= 0.0f) return 1.0f;
    float t = (float)(SDL_GetTicks() - start) / C.anim_ms;
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

/* 0..1, ease-out so the movement settles instead of stopping dead */
static float anim_phase(void)
{
    if (!anim_start || C.anim_ms <= 0.0f) return 1.0f;
    float t = (float)(SDL_GetTicks() - anim_start) / C.anim_ms;
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static SDL_FRect rect_lerp(SDL_FRect a, SDL_FRect b, float t)
{
    return (SDL_FRect){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                        a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t };
}

static SDL_FRect rect_shrink(SDL_FRect r, float f)
{
    float w = r.w * f, h = r.h * f;
    return (SDL_FRect){ r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h };
}

static int   GRID_COLS = 1;       /* columns the tiles are arranged in       */
static int   RW, RH;              /* size of the render target, in pixels   */
static float HEADER_H, FOOTER_H;

/* position strings: none | top-left | top-center | top-right |
 *                          bottom-left | bottom-center | bottom-right       */
static bool pos_is_none(const char *p)   { return !p || !*p || ci_cmp(p, "none") == 0 ||
                                                  ci_cmp(p, "off") == 0 || ci_cmp(p, "hidden") == 0; }
static bool pos_is_top(const char *p)    { return !pos_is_none(p) && strncmp(p, "top", 3) == 0; }
static bool pos_is_bottom(const char *p) { return !pos_is_none(p) && !pos_is_top(p); }

/* x of a text of width w for the horizontal half of the position string */
static float pos_x(const char *p, float w, float margin)
{
    const char *dash = strchr(p ? p : "", '-');
    const char *h = dash ? dash + 1 : "right";
    if (ci_cmp(h, "left") == 0)   return margin;
    if (ci_cmp(h, "center") == 0 || ci_cmp(h, "centre") == 0)
        return ((float)RW - w) * 0.5f;
    return (float)RW - margin - w;
}

static int  sel_ws = 0;
static bool sel_active = true;   /* false: no tile and no window highlighted */
static int  hov_ws = -1, hov_win = -1;

/* A floating window is see-through and answers the pointer only through its
 * name plate, so the windows underneath stay reachable. Once you are on that
 * plate it comes forward and takes the whole of itself — otherwise you lose
 * it the moment you move off the plate, which is exactly when you were about
 * to aim inside it. It is given up again at the edges of its own card, and
 * for one that covers the whole workspace, in a band along the tile: without
 * that there would be no way back out. */
static int  LIFT_WIN = -1;

static bool lift_holds(const Win *w, const Ws *ws, float x, float y)
{
    SDL_FRect c = w->card;
    if (x < c.x || x >= c.x + c.w || y < c.y || y >= c.y + c.h) return false;

    float band = SDL_clamp(SDL_min(ws->tile.w, ws->tile.h) * 0.06f,
                           8.0f * SC, 40.0f * SC);
    bool fills = c.w >= ws->screen.w - band && c.h >= ws->screen.h - band;
    if (!fills) return true;

    return x >= ws->tile.x + band && x < ws->tile.x + ws->tile.w - band &&
           y >= ws->tile.y + band && y < ws->tile.y + ws->tile.h - band;
}

static void lift_update(float x, float y, int ws_idx, int win_idx)
{
    if (LIFT_WIN >= 0 && LIFT_WIN < NWIN) {
        const Win *w = &WINS[LIFT_WIN];
        if (w->ws >= 0 && w->ws < NWS && lift_holds(w, &WSS[w->ws], x, y)) return;
        LIFT_WIN = -1;
    }
    if (win_idx >= 0 && win_idx < NWIN && ws_idx >= 0 && !WINS[win_idx].is_self &&
        (WINS[win_idx].floating || WINS[win_idx].fullscreen))
        LIFT_WIN = win_idx;
}
static char query[128];
static int  qlen = 0;
static bool filtering = false;   /* "/" — hides everything that does not match */
static bool searching = false;   /* "s" — highlights what matches, hides nothing */

static bool query_active(void) { return filtering || searching; }

/* The key press that opens a mode is followed by its own text input event —
 * "s" would otherwise be the first character of the search. */
static bool swallow_next_text = false;
static bool confirm_kill = false;

/* renaming a workspace in place, from a click on its title */
static bool editing = false;
static int  edit_ws = -1;
static char edit_buf[64];
static int  edit_len = 0;

/* NULL means "the workspace itself is selected, not a window in it" */
static Win *ws_sel_win(Ws *w)
{
    if (w->count <= 0 || w->sel < 0) return NULL;
    if (w->sel >= w->count) w->sel = w->count - 1;
    return &WINS[w->first + w->sel];
}

static bool rect_overlap_same(const Win *a, const Win *b)
{
    int tol = 6;
    return SDL_abs(a->x - b->x) <= tol && SDL_abs(a->y - b->y) <= tol &&
           SDL_abs(a->w - b->w) <= tol && SDL_abs(a->h - b->h) <= tol;
}

static void layout_cards(Ws *ws)
{
    if (ws->count <= 0) return;

    /* the windows live in an area that keeps an equal border on all four
     * sides of the mini screen, so nothing ever touches the tile frame */
    float sp = C.screen_pad * SC;
    SDL_FRect area = { ws->screen.x + sp, ws->screen.y + sp,
                       ws->screen.w - 2.0f * sp, ws->screen.h - 2.0f * sp };
    if (area.w < 8.0f || area.h < 8.0f) area = ws->screen;

    float sx = area.w / (float)SDL_max(1, ws->rw);
    float sy = area.h / (float)SDL_max(1, ws->rh);
    float inset = C.win_gap * SC * 0.5f;
    float min_w = 74.0f * SC, min_h = 52.0f * SC;
    min_w = SDL_min(min_w, area.w);
    min_h = SDL_min(min_h, area.h);

    for (int i = 0; i < ws->count; ++i) {
        Win *w = &WINS[ws->first + i];
        SDL_FRect r = {
            area.x + (float)(w->x - ws->rx) * sx + inset,
            area.y + (float)(w->y - ws->ry) * sy + inset,
            (float)w->w * sx - 2.0f * inset,
            (float)w->h * sy - 2.0f * inset
        };
        if (r.w < min_w) { r.x -= (min_w - r.w) * 0.5f; r.w = min_w; }
        if (r.h < min_h) { r.y -= (min_h - r.h) * 0.5f; r.h = min_h; }
        w->card = r;
    }

    /* Tabbed / stacked containers report identical geometry for every child.
     * Split such a group into side by side slices so all of them stay
     * visible and clickable. */
    bool done[MAX_WINDOWS];
    memset(done, 0, sizeof(bool) * (size_t)ws->count);
    for (int i = 0; i < ws->count; ++i) {
        WINS[ws->first + i].group = -1;
        WINS[ws->first + i].has_tab = false;
    }

    for (int i = 0; i < ws->count; ++i) {
        if (done[i]) continue;
        int group[64];
        int n = 0;
        group[n++] = i;
        for (int j = i + 1; j < ws->count && n < 64; ++j) {
            if (done[j]) continue;
            if (WINS[ws->first + i].floating != WINS[ws->first + j].floating) continue;
            if (rect_overlap_same(&WINS[ws->first + i], &WINS[ws->first + j])) {
                group[n++] = j;
                done[j] = true;
            }
        }
        done[i] = true;
        if (n < 2) continue;

        float ge = 3.0f * SC;                    /* room for the group outline */
        SDL_FRect base = WINS[ws->first + i].card;
        base.x += ge; base.y += ge; base.w -= 2.0f * ge; base.h -= 2.0f * ge;
        if (base.w < 4.0f || base.h < 4.0f) base = WINS[ws->first + i].card;
        for (int k = 0; k < n; ++k) WINS[ws->first + group[k]].group = i;

        /* Which child is on top: sway marks it visible, otherwise the focused
         * one, otherwise the first. */
        int active = 0;
        for (int k = 0; k < n; ++k) {
            Win *cw = &WINS[ws->first + group[k]];
            if (cw->focused) { active = k; break; }
            if (cw->visible) active = k;
        }

        float strip = SDL_min(32.0f * SC, base.h * 0.36f);
        bool  tabs_fit = strip >= 16.0f * SC && base.w / (float)n >= 26.0f * SC;

        if (tabs_fit) {
            /* A tab per window across the top, the active one showing its
             * contents underneath — the same shape sway draws. The strip is
             * inset from the group's own edge, so a tab never sits against
             * the border and the first and last are not clipped by its
             * rounded corners. */
            float pad = SDL_clamp(base.w * 0.02f, 2.0f * SC, 8.0f * SC);
            float inner = base.w - 2.0f * pad;
            float tw = inner / (float)n;
            for (int k = 0; k < n; ++k) {
                Win *cw = &WINS[ws->first + group[k]];
                cw->tab = (SDL_FRect){ base.x + pad + (float)k * tw + inset * 0.5f,
                                       base.y + pad, tw - inset, strip };
                cw->has_tab = true;
                cw->card = (k == active)
                    ? (SDL_FRect){ base.x + pad, base.y + pad + strip + inset * 0.5f,
                                   inner, base.h - 2.0f * pad - strip - inset * 0.5f }
                    : cw->tab;
            }
        } else {
            bool horiz = base.w >= base.h;          /* too small: plain slices */
            for (int k = 0; k < n; ++k) {
                SDL_FRect r = base;
                if (horiz) {
                    r.w = (base.w - (float)(n - 1) * inset * 2.0f) / (float)n;
                    r.x = base.x + (float)k * (r.w + inset * 2.0f);
                } else {
                    r.h = (base.h - (float)(n - 1) * inset * 2.0f) / (float)n;
                    r.y = base.y + (float)k * (r.h + inset * 2.0f);
                }
                WINS[ws->first + group[k]].card = r;
                WINS[ws->first + group[k]].has_tab = false;
            }
        }
    }

    /* keep everything inside the tile */
    for (int i = 0; i < ws->count; ++i) {
        SDL_FRect *r = &WINS[ws->first + i].card;
        if (r->w > area.w) r->w = area.w;
        if (r->h > area.h) r->h = area.h;
        if (r->x < area.x) r->x = area.x;
        if (r->y < area.y) r->y = area.y;
        if (r->x + r->w > area.x + area.w) r->x = area.x + area.w - r->w;
        if (r->y + r->h > area.y + area.h) r->y = area.y + area.h - r->h;
    }
}

/* shallow cards (a tab, say) cannot spare 7px top and bottom */
static float card_pad(const Win *w)
{
    return SDL_min(7.0f * SC, w->card.h * 0.13f);
}

enum { CL_ICON, CL_STACK, CL_ROW, CL_TEXT };

/* Decide how much fits on a card: icon + app name + title, icon + name,
 * a row (icon left, text right) on wide flat cards, or just an icon. */
static void compute_card_layout(Win *w)
{
    float pad    = card_pad(w);
    float availw = w->card.w - 2.0f * pad;
    float availh = w->card.h - 2.0f * pad;
    float lab_h  = (float)TTF_GetFontHeight(F_LABEL);
    float sub_h  = (float)TTF_GetFontHeight(F_TITLE);
    float g1 = 4.0f * SC, g2 = 1.0f * SC;
    float icon_max = C.icons ? SDL_min((float)C.icon_px * SC,
                                       SDL_min(availw * 0.72f, availh * 0.72f)) : 0.0f;
    float icon_min = 16.0f * SC;

    w->lay_mode = CL_ICON;
    w->lay_icon = icon_max;
    w->lay_sub  = false;

    if (availw <= 4.0f || availh <= 4.0f) return;

    if (availh >= icon_max + g1 + lab_h + g2 + sub_h) {          /* everything */
        w->lay_mode = CL_STACK;
        w->lay_icon = icon_max;
        w->lay_sub  = true;
        return;
    }
    if (availw >= availh * 1.15f && availw >= lab_h * 5.0f &&
        availh >= lab_h + g2 + sub_h) {                          /* wide and flat */
        w->lay_mode = CL_ROW;
        w->lay_icon = C.icons ? SDL_min(icon_max, SDL_min(availh * 0.9f, availw * 0.3f)) : 0.0f;
        if (w->lay_icon < icon_min) w->lay_icon = 0.0f;
        w->lay_sub  = true;
        return;
    }
    if (availh - (lab_h + g1 + g2 + sub_h) >= icon_min) {        /* shrink the icon */
        w->lay_mode = CL_STACK;
        w->lay_icon = availh - (lab_h + g1 + g2 + sub_h);
        w->lay_sub  = true;
        return;
    }
    if (availh >= icon_min + g1 + lab_h) {                       /* icon + name */
        w->lay_mode = CL_STACK;
        w->lay_icon = SDL_min(icon_max, availh - lab_h - g1);
        w->lay_sub  = false;
        return;
    }
    if (availh >= lab_h) {                                       /* name only */
        w->lay_mode = CL_TEXT;
        w->lay_icon = 0.0f;
        w->lay_sub  = false;
        return;
    }
    w->lay_icon = SDL_min(availw, availh);                       /* icon only */
}

/* Floating and fullscreen windows are drawn as see-through frames with a name
 * plate at the top. The plate is the only part that takes the mouse, so the
 * windows underneath stay clickable. */
static bool card_is_overlay(const Win *w) { return w->floating || w->fullscreen; }

static bool overlay_plate(const Win *w, SDL_FRect r, SDL_FRect *plate, float *icon_w)
{
    if (!w->label.t) return false;

    float ip = 6.0f * SC;
    float ih = SDL_min(22.0f * SC, (float)C.icon_px * SC * 0.6f);
    bool  has_icon = C.icons && w->icon && w->lay_icon >= 10.0f * SC && ih >= 10.0f * SC;
    float iw = has_icon ? ih + 5.0f * SC : 0.0f;
    float pw = iw + (float)w->label.w + 2.0f * ip;
    float ph = SDL_max((float)w->label.h, ih) + ip;

    if (pw > r.w - 4.0f * SC) {                 /* tight: drop the icon first */
        iw = 0.0f;
        pw = (float)w->label.w + 2.0f * ip;
        ph = (float)w->label.h + ip;
    }
    if (pw > r.w - 4.0f * SC || ph > r.h * 0.6f) return false;

    *plate = (SDL_FRect){ r.x + (r.w - pw) * 0.5f, r.y + 7.0f * SC, pw, ph };
    if (icon_w) *icon_w = iw;
    return true;
}

static float card_text_width(const Win *w)
{
    float pad = card_pad(w);
    float availw = w->card.w - 2.0f * pad;
    if (w->lay_mode == CL_ROW && w->lay_icon > 0.0f) availw -= w->lay_icon + 6.0f * SC;
    return availw;
}

/* The name printed in bold on a card. Sway's app_id is used when it says
 * something; otherwise the process behind the window has already been asked
 * (app_key), and failing that the tail of the title is the best guess:
 * "document.txt - Some Editor". */
static const char *card_label_name(const Win *w)
{
    /* sway knew the app: that is the best answer */
    if (!app_id_is_useless(w->app_id)) return app_display_name(w->app_id);

    /* the process behind it matched a .desktop file: also solid */
    const DesktopEntry *de = desktop_find(w->app_key);
    if (de && de->name && *de->name) return de->name;

    /* otherwise the tail of the title names the app more often than a binary
     * does: "document.txt - Some Editor", or plainly "Claude" */
    if (w->title[0]) {
        const char *tail = NULL;
        for (const char *p = strstr(w->title, " - "); p; p = strstr(p + 3, " - ")) tail = p + 3;
        for (const char *p = strstr(w->title, " \xe2\x80\x94 "); p; p = strstr(p + 5, " \xe2\x80\x94 "))
            tail = p + 5;
        if (tail && *tail) return tail;

        /* no separator at all: a short title is usually the app name itself
         * ("Claude"), a long one is a document and the binary is the better
         * answer */
        if (strlen(w->title) <= 24) return w->title;
    }

    /* last resort: the executable name, without its extension */
    if (!app_id_is_useless(w->app_key)) {
        static char buf[128];
        str_set(buf, sizeof(buf), w->app_key);
        char *dot = strrchr(buf, '.');
        if (dot && (ci_cmp(dot, ".py") == 0 || ci_cmp(dot, ".sh") == 0 ||
                    ci_cmp(dot, ".js") == 0 || ci_cmp(dot, ".bin") == 0 ||
                    ci_cmp(dot, ".exe") == 0)) *dot = 0;
        if (buf[0]) return buf;
    }
    return w->title[0] ? w->title : "unknown";
}

static void build_texts(void)
{
    for (int i = 0; i < NWS; ++i) {
        Ws *ws = &WSS[i];
        tex_free(&ws->badge);
        tex_free(&ws->sub);

        char num[16];
        if (ws->num >= 0) snprintf(num, sizeof(num), "%d", ws->num);
        else              str_set(num, sizeof(num), ws->name);
        ws->badge = text_make(F_BADGE, num);
        ws->title = ws->label[0] ? text_make(F_LABEL, ws->label) : (Tex){0};

        /* "DP-1 · 3 windows · 1h 20m", assembled from bounded pieces */
        char count[32], time[32] = "", out[80] = "";
        if (ws->count == 0)      snprintf(count, sizeof(count), "empty");
        else if (ws->count == 1) snprintf(count, sizeof(count), "1 window");
        else                     snprintf(count, sizeof(count), "%d windows", ws->count);

        if (C.usage_dots && ws->usage >= 60.0) {   /* say what the dots mean */
            int mins = (int)(ws->usage / 60.0);
            if (mins < 60) snprintf(time, sizeof(time), " \xc2\xb7 %dm", mins);
            else snprintf(time, sizeof(time), " \xc2\xb7 %dh %dm", mins / 60, mins % 60);
        }
        if (C.all_outputs && ws->output[0])
            snprintf(out, sizeof(out), "%.63s \xc2\xb7 ", ws->output);

        char sub[192];
        snprintf(sub, sizeof(sub), "%.79s%.31s%.31s", out, count, time);
        ws->sub = text_make(F_HINT, sub);
    }

    for (int i = 0; i < NWIN; ++i) {
        Win *w = &WINS[i];
        tex_free(&w->label);
        tex_free(&w->subtitle);
        w->hit = w->card;                    /* the whole card, unless a plate
                                              * takes over below */

        compute_card_layout(w);
        int maxw = (int)card_text_width(w);
        if (maxw < 12 || w->lay_mode == CL_ICON) continue;

        const char *name = card_label_name(w);
        w->label = text_make_fit(F_LABEL, name, maxw);
        if (w->lay_sub && strcmp(name, w->title) != 0)
            w->subtitle = text_make_fit(F_TITLE, w->title, maxw);

        SDL_FRect plate;
        if (card_is_overlay(w) && overlay_plate(w, w->card, &plate, NULL))
            w->hit = plate;
    }
}

static void layout(void)
{
    float m   = C.margin * SC;
    float gap = C.gap * SC;

    bool head_on  = C.show_header && !pos_is_none(C.header_pos);
    bool hints_on = C.show_hints  && !pos_is_none(C.hints_pos);
    float line = (float)C.hint_px * SC * C.ui_scale * 2.4f;

    bool top    = (head_on && pos_is_top(C.header_pos))    || (hints_on && pos_is_top(C.hints_pos));
    bool bottom = (head_on && pos_is_bottom(C.header_pos)) || (hints_on && pos_is_bottom(C.hints_pos));

    HEADER_H = top ? line : m;
    FOOTER_H = bottom ? line : 0.0f;

    float ax = m;
    float ay = HEADER_H;
    float aw = (float)RW - 2.0f * m;
    float ah = (float)RH - HEADER_H - FOOTER_H - m;
    if (aw < 40.0f || ah < 40.0f || NWS == 0) return;

    /* The monitor map keeps a fixed place in the bottom left, always the
     * same one whichever screen is being shown. Handing it whatever cell the
     * grid happened to leave over meant it moved about, and vanished
     * entirely when the grid came out full. The tiles get what is left. */
    MAP_RECT = (SDL_FRect){ 0, 0, 0, 0 };
    CANCEL_RECT = (SDL_FRect){ 0, 0, 0, 0 };
    if (C.outputs_map && NOUTS > 1) {
        int minx = OUTS[0].x, miny = OUTS[0].y;
        int maxx = OUTS[0].x + OUTS[0].w, maxy = OUTS[0].y + OUTS[0].h;
        for (int i = 1; i < NOUTS; ++i) {
            if (OUTS[i].x < minx) minx = OUTS[i].x;
            if (OUTS[i].y < miny) miny = OUTS[i].y;
            if (OUTS[i].x + OUTS[i].w > maxx) maxx = OUTS[i].x + OUTS[i].w;
            if (OUTS[i].y + OUTS[i].h > maxy) maxy = OUTS[i].y + OUTS[i].h;
        }
        float dw = (float)(maxx - minx), dh = (float)(maxy - miny);
        if (dw > 1.0f && dh > 1.0f) {
            float mw = aw * SDL_clamp(C.outputs_map_w, 0.05f, 0.45f);
            float mh = mw * dh / dw;
            float cap = ah * 0.30f;
            if (mh > cap) { mh = cap; mw = mh * dw / dh; }
            MAP_RECT = (SDL_FRect){ ax, ay + ah - mh, mw, mh };

            /* Somewhere to put a drag down and have nothing happen. It runs
               from the monitors to the right edge and fills the band, so
               dragging downwards is enough — no aiming, no corner to find. */
            float cx0 = ax + mw + gap * 2.0f;
            CANCEL_RECT = (SDL_FRect){ cx0, ay + ah - mh,
                                       SDL_max(mh, ax + aw - cx0), mh };

            ah -= mh + gap;
            if (ah < 60.0f) { MAP_RECT = (SDL_FRect){ 0, 0, 0, 0 }; ah = (float)RH - HEADER_H - FOOTER_H - m; }
        }
    }

    /* aspect of the screen we are mirroring */
    float aspect = 16.0f / 9.0f;
    for (int i = 0; i < NWS; ++i)
        if (WSS[i].rw > 0 && WSS[i].rh > 0) { aspect = (float)WSS[i].rw / (float)WSS[i].rh; break; }

    int best_rows = 1, best_cols = NWS;
    float best_area = -1.0f;

    if (C.rows > 0 || C.cols > 0) {
        best_rows = C.rows > 0 ? C.rows : (NWS + C.cols - 1) / C.cols;
        best_cols = C.cols > 0 ? C.cols : (NWS + C.rows - 1) / C.rows;
        if (best_rows * best_cols < NWS) best_cols = (NWS + best_rows - 1) / best_rows;
    } else {
        for (int rows = 1; rows <= NWS; ++rows) {
            int cols = (NWS + rows - 1) / rows;
            float tw = (aw - gap * (float)(cols - 1)) / (float)cols;
            float th = (ah - gap * (float)(rows - 1)) / (float)rows;
            if (tw <= 0.0f || th <= 0.0f) continue;
            /* tiles keep the screen aspect (plus room for the tile header) */
            float ch = tw / aspect;
            if (ch > th) tw = th * aspect; else th = ch;
            float area = tw * th;
            if (area > best_area) { best_area = area; best_rows = rows; best_cols = cols; }
        }
    }

    /* While a workspace is dragged every free number 0..10 becomes a small
     * ghost slot, so it can be dropped on a workspace that does not exist
     * yet. Otherwise the slots are just the workspaces themselves. */
    /* Slots are the workspaces themselves. Only while something is dragged
     * into a gap do the free numbers of that gap join them, as small ghost
     * tiles to drop on — they are not shown before they mean anything. */
    /* Where everything is *right now*, mid-glide included: a relayout during
     * an animation has to continue from what the eye sees, otherwise the
     * movement snaps. */
    Slot old[MAX_WORKSPACES + 16];
    int  nold = SDL_min(NSLOTS, (int)SDL_arraysize(SLOTS));
    float ph_now = anim_phase();
    for (int i = 0; i < nold; ++i) {
        old[i] = SLOTS[i];
        old[i].tile = rect_lerp(SLOTS[i].from, SLOTS[i].tile, ph_now);
    }

    NSLOTS = 0;
    for (int i = 0; i < NWS && NSLOTS < (int)SDL_arraysize(SLOTS); ++i) {
        if (WSS[i].num < 0) continue;
        SLOTS[NSLOTS].ws  = i;
        SLOTS[NSLOTS].num = WSS[i].num;
        NSLOTS++;
    }
    for (int n = ghost_lo; ghost_lo >= 0 && n <= ghost_hi &&
                           NSLOTS < (int)SDL_arraysize(SLOTS); ++n) {
        bool taken = false;
        for (int i = 0; i < NWS; ++i) if (WSS[i].num == n) { taken = true; break; }
        if (taken) continue;
        SLOTS[NSLOTS].ws  = -1;
        SLOTS[NSLOTS].num = n;
        NSLOTS++;
    }
    for (int i = 0; i + 1 < NSLOTS; ++i)              /* order by number */
        for (int j = i + 1; j < NSLOTS; ++j)
            if (SLOTS[j].num < SLOTS[i].num) {
                Slot t = SLOTS[i]; SLOTS[i] = SLOTS[j]; SLOTS[j] = t;
            }
    for (int i = 0; i < NWS && NSLOTS < (int)SDL_arraysize(SLOTS); ++i)
        if (WSS[i].num < 0) {                         /* named workspaces last */
            SLOTS[NSLOTS].ws  = i;
            SLOTS[NSLOTS].num = -1;
            NSLOTS++;
        }

    int n_tiles = NSLOTS;
    if (C.rows <= 0 && C.cols <= 0 && n_tiles != NWS) {   /* redo the grid search */
        best_area = -1.0f;
        for (int rows = 1; rows <= n_tiles; ++rows) {
            int cols = (n_tiles + rows - 1) / rows;
            float tw = (aw - gap * (float)(cols - 1)) / (float)cols;
            float th = (ah - gap * (float)(rows - 1)) / (float)rows;
            if (tw <= 0.0f || th <= 0.0f) continue;
            float ch = tw / aspect;
            if (ch > th) tw = th * aspect; else th = ch;
            float area = tw * th;
            if (area > best_area) { best_area = area; best_rows = rows; best_cols = cols; }
        }
    }

    int cols = best_cols, rows = best_rows;
    if (cols * rows < n_tiles) cols = (n_tiles + rows - 1) / rows;
    GRID_COLS = SDL_max(1, cols);
    float tw = (aw - gap * (float)(cols - 1)) / (float)cols;
    float th = (ah - gap * (float)(rows - 1)) / (float)rows;
    float head_h = (float)C.ws_px * SC * 1.55f;
    float body_aspect_h = tw / aspect + head_h;
    if (body_aspect_h < th) th = body_aspect_h;

    float used_w = (float)cols * tw + (float)(cols - 1) * gap;
    float used_h = (float)rows * th + (float)(rows - 1) * gap;
    float ox = ax + (aw - used_w) * 0.5f;
    float oy = ay + (ah - used_h) * 0.5f;

    for (int sidx = 0; sidx < NSLOTS; ++sidx) {
        int r = sidx / cols, c = sidx % cols;
        SLOTS[sidx].tile = (SDL_FRect){ ox + (float)c * (tw + gap),
                                        oy + (float)r * (th + gap), tw, th };
        if (SLOTS[sidx].ws < 0) continue;                 /* ghost: nothing to lay out */

        Ws *ws = &WSS[SLOTS[sidx].ws];
        ws->tile = SLOTS[sidx].tile;

        float p = C.pad * SC;
        float gutter = C.usage_dots ? C.dot_px * SC * 2.0f : 0.0f;
        SDL_FRect body = { ws->tile.x + p + gutter,
                           ws->tile.y + head_h,
                           ws->tile.w - 2.0f * p - gutter,
                           ws->tile.h - head_h - p };
        if (body.h < 8.0f) body.h = 8.0f;

        /* letterbox the body to the real screen aspect so the mini layout
         * has exactly the proportions of the monitor */
        float wa = (ws->rw > 0 && ws->rh > 0) ? (float)ws->rw / (float)ws->rh : aspect;
        float sw = body.w, sh = body.w / wa;
        if (sh > body.h) { sh = body.h; sw = body.h * wa; }
        ws->screen = (SDL_FRect){ body.x + (body.w - sw) * 0.5f,
                                  body.y + (body.h - sh) * 0.5f, sw, sh };
        layout_cards(ws);
    }

    /* match every slot with where it was a moment ago; things that were not
     * there yet grow out of their own centre */
    bool moved = false;
    for (int i = 0; i < NSLOTS; ++i) {
        SLOTS[i].from = rect_shrink(SLOTS[i].tile, 0.55f);
        bool matched = false;
        for (int j = 0; j < nold; ++j) {
            bool same = (SLOTS[i].ws >= 0)
                          ? (old[j].ws == SLOTS[i].ws)
                          : (old[j].ws < 0 && old[j].num == SLOTS[i].num);
            if (same && old[j].tile.w > 0.0f) {
                SLOTS[i].from = old[j].tile;
                matched = true;
                break;
            }
        }
        if (!laid_out_once) SLOTS[i].from = SLOTS[i].tile;      /* no intro */
        if (!matched || SDL_fabsf(SLOTS[i].from.x - SLOTS[i].tile.x) > 0.5f ||
                        SDL_fabsf(SLOTS[i].from.y - SLOTS[i].tile.y) > 0.5f ||
                        SDL_fabsf(SLOTS[i].from.w - SLOTS[i].tile.w) > 0.5f)
            moved = true;
        if (SLOTS[i].ws >= 0) WSS[SLOTS[i].ws].tile_from = SLOTS[i].from;
    }

    /* ghosts that are no longer part of the grid shrink away instead of
     * blinking out; they are drawn from this list until they are done */
    int dying = 0;
    for (int j = 0; j < nold && dying < (int)SDL_arraysize(DYING); ++j) {
        if (old[j].ws >= 0) continue;
        bool still_there = false;
        for (int i = 0; i < NSLOTS; ++i)
            if (SLOTS[i].ws < 0 && SLOTS[i].num == old[j].num) { still_there = true; break; }
        if (still_there) continue;

        DYING[dying].ws   = -1;
        DYING[dying].num  = old[j].num;
        DYING[dying].from = old[j].tile;
        DYING[dying].tile = rect_shrink(old[j].tile, 0.55f);
        dying++;
    }
    if (dying > 0) {                       /* a later relayout must not eat them */
        NDYING = dying;
        dying_start = SDL_GetTicks();
        moved = true;
    }

    if (moved && laid_out_once) anim_start = SDL_GetTicks();
    laid_out_once = true;

    build_texts();

    /* the clickable name area, between the count and the number */
    for (int i = 0; i < NWS; ++i) {
        Ws *ws = &WSS[i];
        float p = C.pad * SC;
        float head_h = (float)C.ws_px * SC * 1.55f;
        float left  = ws->tile.x + p + (ws->sub.t ? (float)ws->sub.w : 0.0f) + p;
        float right = ws->tile.x + ws->tile.w - p -
                      (ws->badge.t ? (float)ws->badge.w : 0.0f) - p;
        float w = SDL_max(right - left, 0.0f);
        ws->title_box = (SDL_FRect){ left, ws->tile.y, w, head_h };
        ws->title_hit = (SDL_FRect){ left + w * 0.25f, ws->tile.y, w * 0.5f, head_h };
    }
}

/* ------------------------------------------------------------- navigation */

static bool win_visible(const Win *w) { return !filtering || w->match; }

/* first window of the workspace that matches the query, if any */
static int ws_first_visible_match(const Ws *ws)
{
    for (int i = 0; i < ws->count; ++i)
        if (WINS[ws->first + i].match) return i;
    return -1;
}

static int ws_first_visible(const Ws *ws)
{
    for (int i = 0; i < ws->count; ++i)
        if (win_visible(&WINS[ws->first + i])) return i;
    return -1;
}

/* select a workspace; keep_window=false drops down to workspace level */
static void select_ws(int idx, bool keep_window);
static void select_ws(int idx, bool keep_window)
{
    if (NWS == 0) return;
    sel_ws = (idx % NWS + NWS) % NWS;
    Ws *ws = &WSS[sel_ws];
    if (!keep_window || ws->count == 0) { ws->sel = -1; return; }
    if (ws->sel >= ws->count || (ws->sel >= 0 && !win_visible(&WINS[ws->first + ws->sel]))) {
        int v = ws_first_visible(ws);
        ws->sel = v;
    }
}

/* A workspace is worth stepping to while a query is up if something on it
 * matched, or if its own name did — an empty workspace called "mail" is a
 * hit for "mail" even though it has no windows to mark. */
static bool ws_is_hit(const Ws *ws)
{
    if (qlen == 0) return true;
    if (ci_contains(ws->name, query)) return true;
    return ws_first_visible_match(ws) >= 0;
}

/* The focus order swbr keeps, most recent first. swov opens and closes in a
 * moment, so it can never watch focus itself; swbr is running all day and
 * leaves the list where this can pick it up. */
static int  FOCUS[32];
static int  NFOCUS;

static void focus_load(void)
{
    NFOCUS = 0;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/swbr-focus", rt);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[64];
    while (NFOCUS < (int)SDL_arraysize(FOCUS) && fgets(line, sizeof(line), f)) {
        int id = atoi(line);
        if (id > 0) FOCUS[NFOCUS++] = id;
    }
    fclose(f);
}

/* where a window sits in that order, or a big number if it is not in it */
static int focus_rank(int con_id)
{
    for (int i = 0; i < NFOCUS; ++i) if (FOCUS[i] == con_id) return i;
    return 1000;
}

/* Tab through windows in the order they were last used, rather than through
 * workspaces. The first press lands on the one you were in before this one —
 * which is what the key means everywhere else — and holding shift walks back.
 * Returns false when there is no order to walk, so tab falls back to
 * workspaces rather than doing nothing. */
static bool step_recent(int dir)
{
    if (NFOCUS == 0 || NWIN == 0) return false;

    /* every window we can see, in the order they were last focused */
    int order[MAX_WINDOWS], n = 0;
    for (int i = 0; i < NWIN && n < (int)SDL_arraysize(order); ++i)
        if (win_visible(&WINS[i]) && !WINS[i].is_self) order[n++] = i;
    if (n == 0) return false;

    for (int i = 1; i < n; ++i)                    /* small n, plain sort */
        for (int j = i; j > 0 &&
             focus_rank(WINS[order[j]].con_id) < focus_rank(WINS[order[j-1]].con_id); --j) {
            int t = order[j]; order[j] = order[j-1]; order[j-1] = t;
        }

    int at = -1;
    Win *cur = NWS > 0 ? ws_sel_win(&WSS[sel_ws]) : NULL;
    for (int i = 0; i < n; ++i)
        if (cur ? (&WINS[order[i]] == cur) : WINS[order[i]].focused) { at = i; break; }

    int next = at < 0 ? 0 : (at + dir + n) % n;
    if (at < 0 && dir < 0) next = n - 1;

    Win *w = &WINS[order[next]];
    if (w->ws < 0 || w->ws >= NWS) return false;
    sel_ws = w->ws;
    sel_active = true;
    WSS[sel_ws].sel = (int)(w - &WINS[WSS[sel_ws].first]);
    dirty = true;
    return true;
}

static void step_ws(int dir)
{
    for (int i = 1; i <= NWS; ++i) {
        int cand = (((sel_ws + dir * i) % NWS) + NWS) % NWS;
        if (ws_is_hit(&WSS[cand])) { select_ws(cand, false); return; }
    }
}

/* one row up or down in the grid, wrapping through the list */
static void step_ws_row(int dir)
{
    if (NWS == 0) return;
    int step = SDL_max(1, GRID_COLS) * dir;
    int cand = ((sel_ws + step) % NWS + NWS) % NWS;
    select_ws(cand, false);
}

static float rect_cx(SDL_FRect r) { return r.x + r.w * 0.5f; }
static float rect_cy(SDL_FRect r) { return r.y + r.h * 0.5f; }

/* move the selection one step into direction (dx, dy); crosses tiles */
static void navigate(int dx, int dy)
{
    if (NWS == 0) return;
    Ws *ws = &WSS[sel_ws];

    if (ws->sel < 0 && ws->count > 0) {          /* step into the workspace */
        int v = ws_first_visible(ws);
        if (v >= 0) { ws->sel = v; return; }
    }
    Win *cur = ws->count ? ws_sel_win(ws) : NULL;

    float fx = cur ? rect_cx(cur->card) : rect_cx(ws->tile);
    float fy = cur ? rect_cy(cur->card) : rect_cy(ws->tile);

    int best = -1;
    float best_score = 1e30f;

    for (int i = 0; i < ws->count; ++i) {
        Win *w = &WINS[ws->first + i];
        if (w == cur || !win_visible(w)) continue;
        float cx = rect_cx(w->card) - fx, cy = rect_cy(w->card) - fy;
        float along = cx * (float)dx + cy * (float)dy;
        float perp  = SDL_fabsf(cx * (float)dy - cy * (float)dx);
        if (along <= 4.0f * SC) continue;
        float score = along + perp * 2.0f;
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best >= 0) { ws->sel = best; return; }

    /* nothing in that direction inside the tile: jump to the next tile */
    int best_ws = -1;
    best_score = 1e30f;
    for (int i = 0; i < NWS; ++i) {
        if (i == sel_ws) continue;
        float cx = rect_cx(WSS[i].tile) - rect_cx(ws->tile);
        float cy = rect_cy(WSS[i].tile) - rect_cy(ws->tile);
        float along = cx * (float)dx + cy * (float)dy;
        float perp  = SDL_fabsf(cx * (float)dy - cy * (float)dx);
        if (along <= 1.0f) continue;
        float score = along + perp * 3.0f;
        if (score < best_score) { best_score = score; best_ws = i; }
    }
    if (best_ws < 0) {
        /* nothing further in that direction: wrap around to the far side,
         * staying as close to the current row or column as possible */
        float bs = 1e30f;
        for (int i = 0; i < NWS; ++i) {
            if (i == sel_ws) continue;
            float cx = rect_cx(WSS[i].tile), cy = rect_cy(WSS[i].tile);
            float along = cx * (float)dx + cy * (float)dy;          /* far side */
            float perp  = SDL_fabsf((cx - rect_cx(ws->tile)) * (float)dy -
                                    (cy - rect_cy(ws->tile)) * (float)dx);
            float score = along + perp * 3.0f;
            if (score < bs) { bs = score; best_ws = i; }
        }
    }

    if (best_ws >= 0) {
        select_ws(best_ws, false);
        Ws *nw = &WSS[sel_ws];
        if (nw->count > 0) {                       /* land on the closest card */
            int bi = -1;
            float bs = 1e30f;
            for (int i = 0; i < nw->count; ++i) {
                Win *w = &WINS[nw->first + i];
                if (!win_visible(w)) continue;
                float d = SDL_fabsf(rect_cx(w->card) - fx) + SDL_fabsf(rect_cy(w->card) - fy);
                if (d < bs) { bs = d; bi = i; }
            }
            if (bi >= 0) nw->sel = bi;
        }
    }
}

static void apply_filter(void)
{
    for (int i = 0; i < NWIN; ++i) {
        Win *w = &WINS[i];
        w->match = (qlen == 0) || ci_contains(w->title, query) ||
                   ci_contains(w->app_id, query) || ci_contains(w->app_key, query) ||
                   ci_contains(w->ws_name, query);
    }
    if (qlen > 0) {                       /* jump to the first workspace with a hit */
        for (int i = 0; i < NWS; ++i) {
            int v = ws_first_visible_match(&WSS[i]);
            if (v >= 0) { sel_ws = i; WSS[i].sel = v; sel_active = true; return; }
        }
        /* Nothing matched. Leaving the previous selection lit says something
         * was found and points at the wrong thing; enter would then act on
         * it. Nothing is selected until the query matches again. */
        for (int i = 0; i < NWS; ++i) WSS[i].sel = -1;
        sel_active = false;
    } else {
        select_ws(sel_ws, true);
    }
}

static int hit_test(float mx, float my, int *out_ws)
{
    *out_ws = -1;
    for (int i = 0; i < NWS; ++i) {
        Ws *ws = &WSS[i];
        if (mx < ws->tile.x || mx >= ws->tile.x + ws->tile.w ||
            my < ws->tile.y || my >= ws->tile.y + ws->tile.h) continue;
        *out_ws = i;
        for (int j = ws->count - 1; j >= 0; --j) {      /* floats are last = on top */
            Win *w = &WINS[ws->first + j];
            if (w->is_self) continue;              /* scenery, not a target */
            SDL_FRect r = (ws->first + j == LIFT_WIN)
                        ? w->card                          /* lifted: all of it */
                        : ((w->hit.w > 0.0f) ? w->hit : w->card);
            if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h)
                return ws->first + j;
        }
        return -1;
    }
    return -1;
}

static char *escape_arg(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)xmalloc(n * 2 + 1);
    char *o = out;
    for (const char *p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') *o++ = '\\';
        *o++ = *p;
    }
    *o = 0;
    return out;
}

/* Bring the history in line with reality. Workspaces also get switched by
 * sway keybindings, which swov never sees; without this, "go back" bounces
 * between the last two workspaces swov itself visited and can never return to
 * one the user reached another way. */
static void usage_sync(void)
{
    if (!C.track) return;

    char name[64] = {0}, out[64] = {0};
    int  num = -1;
    focused_workspace(name, sizeof(name), out, sizeof(out), &num);
    if (!name[0]) return;

    usage_load();
    if (strcmp(name, USAGE_CUR) == 0) return;      /* already up to date */

    usage_switch(name, out, num);                  /* credits the old one */
}

/* ------------------------------------------------------------ drag & drop
 * A press only arms a drag. If the pointer stays put, the release focuses
 * (that is the "press and release on the same thing" rule); if it moves, we
 * are dragging either a window or a whole workspace.
 */

typedef enum {
    DROP_NONE,
    DROP_WIN_WS,     /* window onto a workspace tile                       */
    DROP_WIN_NEAR,   /* window next to another window, splitting h or v    */
    DROP_WS_SWAP,    /* workspace onto another workspace                   */
    DROP_WS_NUM,     /* workspace onto a free number (a ghost slot)        */
    DROP_WIN_NEWWS,  /* window onto a free number: sway creates it          */
    DROP_WS_EDGE,    /* window along one edge of a tile: beside the lot      */
    DROP_CANCEL      /* the ✕ beside the monitors: let go and nothing happens */
} DropKind;

enum { EDGE_LEFT, EDGE_RIGHT, EDGE_TOP, EDGE_BOTTOM };

static bool  press_down;
static float press_x, press_y;
static int   press_win = -1, press_ws = -1;
static bool  drag_active;
static float drag_x, drag_y;
static SDL_FRect drag_src_tile;   /* where the dragged window's workspace was
                                   * when the drag began — a fixed rectangle,
                                   * so the reflow cannot move the threshold */

static DropKind drop_kind;
static int   drop_ws = -1, drop_win = -1, drop_num = -1, drop_edge = EDGE_RIGHT;
static bool  drop_insert;        /* WS_SWAP position means "insert here"   */

static void layout(void);
/* the workspace sway is showing right now */
static void select_current_workspace(void)
{
    sel_ws = 0;
    for (int i = 0; i < NWS; ++i)
        if (WSS[i].focused) { sel_ws = i; return; }
    for (int i = 0; i < NWS; ++i)
        if (WSS[i].visible) { sel_ws = i; return; }
}

static void reload_model(void);

static int slot_at(float x, float y)
{
    for (int i = 0; i < NSLOTS; ++i) {
        SDL_FRect t = SLOTS[i].tile;
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) return i;
    }
    return -1;
}

/* which side of a card the pointer is closest to */
static int edge_at(SDL_FRect r, float x, float y)
{
    float dx = (x - (r.x + r.w * 0.5f)) / SDL_max(1.0f, r.w);
    float dy = (y - (r.y + r.h * 0.5f)) / SDL_max(1.0f, r.h);
    if (SDL_fabsf(dx) >= SDL_fabsf(dy)) return dx < 0.0f ? EDGE_LEFT : EDGE_RIGHT;
    return dy < 0.0f ? EDGE_TOP : EDGE_BOTTOM;
}

static bool point_in(SDL_FRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Every screen's workspaces, or only this one's. Rebuilding the model in the
 * middle of a drag would lose the window being dragged, so its id is kept and
 * looked up again on the other side. */
static void show_all_outputs(bool on)
{
    if (DRAG_ALL_OUTPUTS == on) return;
    DRAG_ALL_OUTPUTS = on;

    int keep = press_win >= 0 ? WINS[press_win].con_id : DRAG_CON;
    reload_model();
    press_win = -1;
    if (keep > 0)
        for (int i = 0; i < NWIN; ++i)
            if (WINS[i].con_id == keep) { press_win = i; break; }
    dirty = true;
}

static void set_ghosts(int lo, int hi)
{
    if (lo == ghost_lo && hi == ghost_hi) return;
    ghost_lo = lo;
    ghost_hi = hi;
    layout();      /* the ghosts take their place, layout() starts the glide */
}

/* A workspace being dragged always wants the free numbers on screen. A window
 * asks for them once it leaves the workspace it lives on — and then keeps
 * them for the rest of the drag. Toggling them back off would move that very
 * workspace back under the pointer, which asks for them again: the tiles would
 * shudder along the border instead of settling. */
static void update_ghosts(float x, float y)
{
    if (ghost_lo >= 0) return;                     /* already out, leave them */
    if (anim_running()) return;                    /* never decide mid-glide  */

    bool want = false;
    if (drag_ws_mode) {
        want = true;
    } else if (press_win >= 0) {
        want = !point_in(drag_src_tile, x, y);
    }
    if (want) {
        if (C.drop_outputs) show_all_outputs(true);
        set_ghosts(0, 10);
    }
}

/* Holding a drag over another monitor steps the overview onto it. The press
 * has to be still: any movement starts the wait again, so brushing past a
 * plate on the way somewhere else never triggers it. */
static bool point_in_cancel(float x, float y)
{
    return CANCEL_RECT.w > 0.0f &&
           x >= CANCEL_RECT.x && x < CANCEL_RECT.x + CANCEL_RECT.w &&
           y >= CANCEL_RECT.y && y < CANCEL_RECT.y + CANCEL_RECT.h;
}

static bool map_dwell_hover(float x, float y)
{
    if (MAP_RECT.w <= 0.0f) return false;

    int on = -1;
    for (int i = 0; i < NOUTS; ++i) {
        SDL_FRect r = OUTS[i].box;
        if (r.w > 0.0f && x >= r.x && x < r.x + r.w &&
            y >= r.y && y < r.y + r.h) { on = i; break; }
    }
    if (on >= 0 && strcmp(OUTS[on].name, FOCUSED_OUTPUT) == 0) on = -1;

    float moved = SDL_fabsf(x - MAP_HOVER_X) + SDL_fabsf(y - MAP_HOVER_Y);
    if (on != MAP_HOVER || moved > 4.0f * SC) {
        MAP_HOVER = on;
        MAP_HOVER_SINCE = now_secs();
        MAP_HOVER_X = x;
        MAP_HOVER_Y = y;
        dirty = true;
    }
    return on >= 0;
}

static void drag_update_target(float x, float y)
{
    if (point_in_cancel(x, y)) {
        drop_kind = DROP_CANCEL;
        drop_ws = drop_win = drop_num = -1;
        MAP_HOVER = -1;
        return;
    }
    if (map_dwell_hover(x, y)) {          /* over a monitor: nothing else */
        drop_kind = DROP_NONE;
        drop_ws = drop_win = drop_num = -1;
        return;
    }

    drop_kind = DROP_NONE;
    drop_ws = drop_win = drop_num = -1;
    drop_insert = false;

    update_ghosts(x, y);

    int gs = slot_at(x, y);
    if (gs >= 0 && SLOTS[gs].ws < 0) {                /* a ghost: a free number */
        drop_kind = drag_ws_mode ? DROP_WS_NUM : DROP_WIN_NEWWS;
        drop_num  = SLOTS[gs].num;
        return;
    }

    if (drag_ws_mode) {
        int si = gs;
        if (si < 0) return;
        if (SLOTS[si].ws == press_ws) return;         /* itself */

        SDL_FRect t = SLOTS[si].tile;
        float rel = (x - t.x) / SDL_max(1.0f, t.w);
        drop_ws  = SLOTS[si].ws;
        drop_num = SLOTS[si].num;
        if (rel < 0.22f || rel > 0.78f) {             /* the edges insert */
            drop_kind   = DROP_WS_SWAP;
            drop_insert = true;
            drop_edge   = rel < 0.22f ? EDGE_LEFT : EDGE_RIGHT;
        } else {
            drop_kind = DROP_WS_SWAP;                 /* the middle swaps */
        }
        return;
    }

    int ws_idx = -1;
    int win_idx = hit_test(x, y, &ws_idx);
    if (ws_idx < 0) return;

    /* a band along the inside of the tile: beside everything on it */
    {
        SDL_FRect t = WSS[ws_idx].screen;
        float bx = SDL_clamp(t.w * 0.09f, 6.0f * SC, t.w * 0.18f);
        float by = SDL_clamp(t.h * 0.09f, 6.0f * SC, t.h * 0.18f);
        int e = -1;
        if      (x - t.x < bx)         e = EDGE_LEFT;
        else if (t.x + t.w - x < bx)   e = EDGE_RIGHT;
        else if (y - t.y < by)         e = EDGE_TOP;
        else if (t.y + t.h - y < by)   e = EDGE_BOTTOM;
        if (e >= 0 && WSS[ws_idx].count > 0) {
            drop_kind = DROP_WS_EDGE;
            drop_ws   = ws_idx;
            drop_edge = e;
            return;
        }
    }

    if (win_idx >= 0 && win_idx != press_win) {
        drop_kind = DROP_WIN_NEAR;
        drop_win  = win_idx;
        drop_ws   = ws_idx;
        drop_edge = edge_at(WINS[win_idx].card, x, y);
        return;
    }
    if (win_idx < 0) {
        drop_kind = DROP_WIN_WS;
        drop_ws   = ws_idx;
    }
}

/* Dropping against the edge of a tile means "next to everything here", not
 * "next to whichever window happens to be under the pointer". Two windows
 * stacked one above the other are a column, and left of the column is not the
 * same as left of its top window — which is all sway can be told about a
 * single view.
 *
 * Whether the column has to be broken out of depends on how the workspace is
 * split. If the windows sit side by side, putting the new one beside the
 * outermost of them is already right. If they are stacked, the new window has
 * to be moved out of that stack, which is what a perpendicular `move` does.
 *
 * Returns the window to work from, and sets *pop when the stack has to be
 * escaped. -1 when the workspace is empty and a plain move will do. */
static int ws_edge_ref(const Ws *ws, int edge, bool *pop)
{
    *pop = false;
    if (ws->count <= 0) return -1;

    bool horiz = (edge == EDGE_LEFT || edge == EDGE_RIGHT);
    int  best = -1;
    float best_v = 0.0f;
    int   aligned = 0;

    for (int i = 0; i < ws->count; ++i) {
        const Win *w = &WINS[ws->first + i];
        if (!win_visible(w)) continue;
        float v = horiz ? w->card.x : w->card.y;
        if (edge == EDGE_RIGHT)  v = w->card.x + w->card.w;
        if (edge == EDGE_BOTTOM) v = w->card.y + w->card.h;

        bool better = best < 0 ||
                      ((edge == EDGE_LEFT || edge == EDGE_TOP) ? v < best_v : v > best_v);
        if (better) { best = ws->first + i; best_v = v; }
    }
    if (best < 0) return -1;

    /* how many share the outermost one's line: more than one means they are
     * stacked across the direction we are dropping from */
    float ref = horiz ? WINS[best].card.x : WINS[best].card.y;
    float tol = horiz ? WINS[best].card.w * 0.25f : WINS[best].card.h * 0.25f;
    for (int i = 0; i < ws->count; ++i) {
        const Win *w = &WINS[ws->first + i];
        if (!win_visible(w)) continue;
        float v = horiz ? w->card.x : w->card.y;
        if (SDL_fabsf(v - ref) <= tol) aligned++;
    }
    *pop = aligned > 1;
    return best;
}

/* "7:chat" keeps its label when it becomes workspace 8 */
static char *ws_name_with_num(const Ws *ws, int num)
{
    const char *colon = strchr(ws->name, ':');
    if (colon) return fmt_alloc("%d%s", num, colon);
    return fmt_alloc("%d", num);
}

static void ws_rename(const Ws *ws, const char *to)
{
    char *from = escape_arg(ws->name);
    char *dst  = escape_arg(to);
    sway_cmd("rename workspace \"%s\" to \"%s\"", from, dst);
    free(from);
    free(dst);
}

static void act_ws_assign(int ws_idx, int num)
{
    if (ws_idx < 0 || ws_idx >= NWS) return;
    char *to = ws_name_with_num(&WSS[ws_idx], num);
    ws_rename(&WSS[ws_idx], to);
    free(to);
}

static void act_ws_swap(int a, int b)
{
    if (a < 0 || b < 0 || a == b) return;
    int na = WSS[a].num, nb = WSS[b].num;
    if (na < 0 || nb < 0) return;

    char *tmp = fmt_alloc("swov_tmp_%d", na);
    ws_rename(&WSS[a], tmp);
    act_ws_assign(b, na);

    char *to = ws_name_with_num(&WSS[a], nb);          /* a is called tmp now */
    char *from = escape_arg(tmp);
    char *dst  = escape_arg(to);
    sway_cmd("rename workspace \"%s\" to \"%s\"", from, dst);
    free(from);
    free(dst);
    free(to);
    free(tmp);
}

/* Insert the dragged workspace at `num`, pushing the occupied run up by one.
 * With 1, 2, 5 on screen, inserting at 2 moves 2 to 3 and stops there. */
static void act_ws_insert(int ws_idx, int num)
{
    if (ws_idx < 0 || num < 0) return;
    if (WSS[ws_idx].num == num) return;

    int run[MAX_WORKSPACES];
    int n = 0;
    for (int want = num; n < MAX_WORKSPACES; ++want) {
        int found = -1;
        for (int i = 0; i < NWS; ++i)
            if (i != ws_idx && WSS[i].num == want) { found = i; break; }
        if (found < 0) break;
        run[n++] = found;
    }
    for (int i = n - 1; i >= 0; --i)                   /* top down, no clashes */
        act_ws_assign(run[i], WSS[run[i]].num + 1);
    act_ws_assign(ws_idx, num);
}

/* Put the dragged window next to `target`, splitting the target the right
 * way first. A mark is the only reliable way to say "there" to sway. */
static void act_win_drop_near(Win *drag, Win *target, int edge)
{
    if (!drag || !target || drag == target) return;
    bool horiz = (edge == EDGE_LEFT || edge == EDGE_RIGHT);

    sway_cmd("[con_id=%d] mark --add _swov_drop", target->con_id);
    sway_cmd("[con_id=%d] split %s", target->con_id, horiz ? "h" : "v");
    sway_cmd("[con_id=%d] move container to mark _swov_drop", drag->con_id);
    if (edge == EDGE_LEFT || edge == EDGE_TOP)
        sway_cmd("[con_id=%d] move %s", drag->con_id, horiz ? "left" : "up");
    sway_cmd("unmark _swov_drop");
}

static const JV *jfind_workspace(const JV *node, const char *name)
{
    if (!node || node->type != J_OBJ) return NULL;
    if (!strcmp(jstr(node, "type", ""), "workspace") &&
        !strcmp(jstr(node, "name", ""), name)) return node;

    for (int i = 0; i < node->count; ++i) {
        const JV *kid = node->items[i];
        if (!kid) continue;
        if (kid->type == J_ARR) {
            for (int j = 0; j < kid->count; ++j) {
                const JV *r = jfind_workspace(kid->items[j], name);
                if (r) return r;
            }
        } else if (kid->type == J_OBJ) {
            const JV *r = jfind_workspace(kid, name);
            if (r) return r;
        }
    }
    return NULL;
}

static bool subtree_has_id(const JV *node, int id)
{
    if (!node || node->type != J_OBJ) return false;
    if (jint(node, "id", -1) == id) return true;
    for (int i = 0; i < node->count; ++i) {
        const JV *kid = node->items[i];
        if (!kid) continue;
        if (kid->type == J_ARR) {
            for (int j = 0; j < kid->count; ++j)
                if (subtree_has_id(kid->items[j], id)) return true;
        } else if (kid->type == J_OBJ) {
            if (subtree_has_id(kid, id)) return true;
        }
    }
    return false;
}

/* Is `con_id` the first (or last) direct child of that workspace?
 *  1 = yes, 0 = on the workspace but not at that end, -1 = not there at all */
static int ws_edge_state(const char *ws_name, int con_id, bool want_first)
{
    JV *tree = sway_query(IPC_GET_TREE);
    if (!tree) return -1;

    const JV *ws = jfind_workspace(tree, ws_name);
    int state = -1;
    if (ws) {
        const JV *kids = jget(ws, "nodes");
        if (kids && kids->type == J_ARR && kids->count > 0) {
            const JV *end = kids->items[want_first ? 0 : kids->count - 1];
            if (end && jint(end, "id", -1) == con_id) state = 1;
            else state = subtree_has_id(ws, con_id) ? 0 : -1;
        } else {
            state = subtree_has_id(ws, con_id) ? 0 : -1;
        }
    }
    jfree(tree);
    return state;
}

/* Move it to one end of the workspace and check, rather than guessing how
 * many moves it takes. One `move left` out of a stack lifts the window clear
 * of the whole column; one inside a row only swaps it with a neighbour. Both
 * end at the same place if you keep going until sway says it is the first
 * child, and stopping on that condition means never one move too many —
 * which would push it onto the next monitor. */
static void move_to_ws_edge(int con_id, const char *ws_name, int edge)
{
    static const char *DIR[] = { "left", "right", "up", "down" };
    bool first = (edge == EDGE_LEFT || edge == EDGE_TOP);

    for (int i = 0; i < 10; ++i) {
        int st = ws_edge_state(ws_name, con_id, first);
        if (st == 1) return;                       /* where it was asked for */
        if (st < 0)  return;                       /* not there: leave it be */
        sway_cmd("[con_id=%d] move %s", con_id, DIR[edge]);
    }
}

static void move_to_ws_edge_named(int con_id, const char *ws, const char *edge)
{
    int e = !strcmp(edge, "left") ? EDGE_LEFT : !strcmp(edge, "right") ? EDGE_RIGHT
          : !strcmp(edge, "top")  ? EDGE_TOP  : EDGE_BOTTOM;
    move_to_ws_edge(con_id, ws, e);
}

/* Beside everything on a workspace. Side by side already means the outermost
 * window is the whole edge, so that is the ordinary case. Stacked means the
 * new window has to leave the stack: a move across the stack's direction
 * lifts it out and puts it beside the whole thing, which is the one shape
 * sway can be asked for directly. */
static void act_win_drop_edge(Win *drag, const Ws *ws, int edge)
{
    bool pop = false;
    int  ref = ws_edge_ref(ws, edge, &pop);

    if (ref >= 0 && !pop) { act_win_drop_near(drag, &WINS[ref], edge); return; }

    if (drag->ws != (int)(ws - WSS)) {
        if (ws->num >= 0)
            sway_cmd("[con_id=%d] move container to workspace number %d",
                     drag->con_id, ws->num);
        else {
            char *e = escape_arg(ws->name);
            sway_cmd("[con_id=%d] move container to workspace \"%s\"", drag->con_id, e);
            free(e);
        }
    }
    if (ref < 0) return;                       /* nothing there to be beside */

    move_to_ws_edge(drag->con_id, ws->name, edge);
}

static void drag_finish(void)
{
    switch (drop_kind) {
    case DROP_WIN_WS:
        if (press_win >= 0 && drop_ws >= 0 && WINS[press_win].ws != drop_ws) {
            Ws *ws = &WSS[drop_ws];
            if (ws->num >= 0)
                sway_cmd("[con_id=%d] move container to workspace number %d",
                         WINS[press_win].con_id, ws->num);
            else {
                char *e = escape_arg(ws->name);
                sway_cmd("[con_id=%d] move container to workspace \"%s\"",
                         WINS[press_win].con_id, e);
                free(e);
            }
        }
        break;

    case DROP_WIN_NEAR:
        if (press_win >= 0 && drop_win >= 0)
            act_win_drop_near(&WINS[press_win], &WINS[drop_win], drop_edge);
        break;

    case DROP_WS_EDGE:
        if (press_win >= 0 && drop_ws >= 0)
            act_win_drop_edge(&WINS[press_win], &WSS[drop_ws], drop_edge);
        break;

    case DROP_WS_SWAP:
        if (drop_insert) {
            int num = drop_num + (drop_edge == EDGE_RIGHT ? 1 : 0);
            act_ws_insert(press_ws, num);
        } else {
            act_ws_swap(press_ws, drop_ws);
        }
        break;

    case DROP_WS_NUM:
        act_ws_assign(press_ws, drop_num);
        break;

    case DROP_WIN_NEWWS:                     /* sway creates the workspace */
        if (press_win >= 0 && drop_num >= 0)
            sway_cmd("[con_id=%d] move container to workspace number %d",
                     WINS[press_win].con_id, drop_num);
        break;

    case DROP_CANCEL:                        /* put down where it started */
    case DROP_NONE:
    default:
        break;
    }

    show_all_outputs(false);
    DRAG_CON = 0;
    drag_active = drag_ws_mode = false;
    drop_kind = DROP_NONE;
    press_win = press_ws = -1;
    ghost_lo = ghost_hi = -1;
    layout();
    reload_model();          /* the event socket refreshes again once sway settles */
}

static void drag_cancel(void)
{
    bool had_ghosts = ghost_lo >= 0;
    drag_active = drag_ws_mode = false;
    drop_kind = DROP_NONE;
    press_down = false;
    press_win = press_ws = -1;
    ghost_lo = ghost_hi = -1;
    if (had_ghosts) layout();
}

/* ---------------------------------------------------------------- chrome */

static Tex T_HEADER, T_HINTS, T_QUERY;

static void rebuild_chrome(void)
{
    tex_free(&T_HEADER);
    tex_free(&T_HINTS);
    tex_free(&T_QUERY);

    if (C.show_header) {
        char buf[256];
        /* Looking at a screen you are not on is worth saying outright: every
         * key and every drop from here lands over there. */
        snprintf(buf, sizeof(buf), "%s%.63s   %d window%s on %d workspace%s",
                 PINNED_OUTPUT ? "viewing " : "",
                 FOCUSED_OUTPUT[0] ? FOCUSED_OUTPUT : "sway",
                 NWIN, NWIN == 1 ? "" : "s", NWS, NWS == 1 ? "" : "s");
        T_HEADER = text_make(F_HINT, buf);
    }
    if (C.show_hints) {
        /* when both lines share a band the hints get whatever width is left */
        int avail = RW - (int)(C.margin * SC * 2.0f);
        bool same_band = C.show_header && !pos_is_none(C.header_pos) &&
                         !pos_is_none(C.hints_pos) &&
                         pos_is_top(C.header_pos) == pos_is_top(C.hints_pos);
        if (same_band && T_HEADER.t) avail -= T_HEADER.w + (int)(C.margin * SC);

        T_HINTS = text_make_fit(F_HINT,
            "\xe2\x86\xb5 focus    drag to move    tab workspace    0-9 go to    "
            "ctrl+0-9 move    space mark    x close    f find    / filter    "
            "esc quit", avail);
    }

    if (confirm_kill) {
        int n = 0;
        for (int i = 0; i < NWIN; ++i) if (WINS[i].marked) n++;
        if (n == 0 && NWS > 0) {
            Win *w = ws_sel_win(&WSS[sel_ws]);
            n = w ? 1 : WSS[sel_ws].count;
        }
        char buf[192];
        snprintf(buf, sizeof(buf), "close %d window%s?   enter to confirm",
                 n, n == 1 ? "" : "s");
        T_QUERY = text_make(F_HINT, buf);
    } else if (query_active()) {
        char buf[192];
        snprintf(buf, sizeof(buf), "%s  %.127s\xe2\x96\x8f",
                 filtering ? "filter:" : "search:", query);
        T_QUERY = text_make(F_HINT, buf);
    }
}

/* --------------------------------------------------------------- drawing */

/* While the grid is settling, a workspace is drawn where it currently is on
 * its way, and everything inside it rides along on the same transform. */
static float XF_SX = 1.0f, XF_SY = 1.0f, XF_X0, XF_Y0, XF_DX, XF_DY;
static bool  XF_ON;

static void xf_set(SDL_FRect from, SDL_FRect to)
{
    XF_ON = true;
    XF_SX = (from.w > 0.0f) ? to.w / from.w : 1.0f;
    XF_SY = (from.h > 0.0f) ? to.h / from.h : 1.0f;
    XF_X0 = from.x;
    XF_Y0 = from.y;
    XF_DX = to.x;
    XF_DY = to.y;
}

static void xf_off(void) { XF_ON = false; XF_SX = XF_SY = 1.0f; }

static SDL_FRect xf(SDL_FRect r)
{
    if (!XF_ON) return r;
    return (SDL_FRect){ XF_DX + (r.x - XF_X0) * XF_SX, XF_DY + (r.y - XF_Y0) * XF_SY,
                        r.w * XF_SX, r.h * XF_SY };
}

static void draw_icon(SDL_Texture *icon, float cx, float top, float size)
{
    if (!icon) return;
    float w = size, h = size;
    float tw = 0.0f, th = 0.0f;
    if (SDL_GetTextureSize(icon, &tw, &th) && tw > 0.0f && th > 0.0f) {
        if (tw > th) h = size * (th / tw);
        else if (th > tw) w = size * (tw / th);
    }
    SDL_FRect dst = { cx - w * 0.5f, top + (size - h) * 0.5f, w, h };
    SDL_RenderTexture(REN, icon, NULL, &dst);
}

static void draw_card(Win *w, bool tile_selected)
{
    SDL_FRect r = xf(w->card);
    if (r.w < 4.0f || r.h < 4.0f) return;

    bool selected = tile_selected && sel_active && ws_sel_win(&WSS[w->ws]) == w;
    bool is_hovered = selected;          /* pointer and keyboard share a cursor */
    bool dimmed   = filtering && !w->match;
    bool is_hit   = searching && qlen > 0 && w->match;
    float rad     = SDL_min(C.radius * SC * 0.62f, SDL_min(r.w, r.h) * 0.32f);

    SDL_FColor fill = w->focused ? C.card_focus : C.card;
    if (is_hovered) fill = C.card_hover;
    if (is_hit)     fill = mix(fill, C.match, 0.22f);
    if (confirm_kill && (w->marked || selected)) fill = mix(fill, C.urgent, 0.25f);
    if (selected)   fill = mix(fill, C.hl, is_hovered ? 0.16f : 0.10f);
    if (dimmed)     fill = with_alpha(mix(fill, C.tile, 0.6f), fill.a * 0.45f);

    /* The one being dragged, still drawn where it came from. Halfway to the
     * background, so it reads as a space the window has left rather than a
     * window sitting there — and it is the only thing on screen wearing the
     * accent, so there is no forgetting which one is on the pointer. */
    bool being_dragged = drag_active && press_win >= 0 && &WINS[press_win] == w;
    if (being_dragged)
        fill = with_alpha(mix(fill, C.bg, 0.55f), fill.a * 0.8f);

    /* A floating dialog, and especially a fullscreen window, sits on top of
     * the tiled ones. Drawing it solid would hide the whole workspace, so it
     * gets see-through — the more of the screen it covers, the more so. Its
     * border, icon and label stay at full strength, so it is still obvious
     * which window it is. */
    /* Aiming inside a floating window is guesswork when it is see-through:
     * you are trying to hit an edge you can barely make out. Point at one —
     * or drag over one — and it comes forward, solid, until the pointer
     * leaves again. */
    bool lifted = LIFT_WIN >= 0 && LIFT_WIN < NWIN && &WINS[LIFT_WIN] == w;

    float over = 1.0f, cover = 0.0f;
    bool on_top = w->floating || w->fullscreen;
    if (on_top && !lifted) {
        const Ws *ws = &WSS[w->ws];
        float screen_area = ws->screen.w * ws->screen.h;
        cover = (screen_area > 1.0f) ? (w->card.w * w->card.h) / screen_area : 1.0f;
        cover = SDL_clamp(cover, 0.0f, 1.0f);
        over = C.float_alpha * (1.0f - 0.55f * cover);
        fill = with_alpha(fill, fill.a * over);
    }
    /* Anything drawn on top of other windows gets a frame and a name plate
     * rather than a filled card: its label would otherwise sit in the middle,
     * exactly where the label of the window showing through already is. */
    bool as_overlay = on_top && !lifted;

    /* A tab is rounded on top and flat where it meets its panel; the panel
     * is flat on top and rounded below. A window that is neither is rounded
     * all round, as before. */
    bool is_tab   = w->has_tab && w->card.h <= w->tab.h + 1.0f;
    bool is_panel = w->has_tab && !is_tab;

    if (w->floating) drop_shadow(r, rad, 9.0f * SC, with_alpha(C.shadow_col, C.shadow_col.a * over));
    if (is_tab)        fill_round_side(r, rad, true, false, fill);
    else if (is_panel) fill_round_side(r, rad, false, true, fill);
    else               fill_round_rect(r, rad, fill);

    /* border: what is being dragged beats everything, then selection, hover,
       mark, plain */
    float bw = SDL_max(1.0f, C.border * SC * 0.75f);
    if (lifted && !being_dragged) {
        /* it has come forward: a heavier accent frame, and a soft one just
           outside it so it reads as raised rather than merely outlined */
        SDL_FRect glow = { r.x - 3.0f * SC, r.y - 3.0f * SC,
                           r.w + 6.0f * SC, r.h + 6.0f * SC };
        stroke_round_rect(glow, rad + 3.0f * SC, SDL_max(1.0f, 3.0f * SC),
                          with_alpha(C.accent, 0.25f));
    }

    if (being_dragged)     stroke_round_rect(r, rad, bw * 1.6f, C.accent);
    else if (lifted)       stroke_round_rect(r, rad, bw * 1.8f, C.accent);
    else if (confirm_kill && (w->marked || selected))
                           stroke_round_rect(r, rad, bw * 1.35f, C.urgent);
    else if (selected)     stroke_round_rect(r, rad, bw * 1.35f, C.hl);
    else if (is_hit)       stroke_round_rect(r, rad, bw * 1.2f, C.match);
    else if (is_hovered)   stroke_round_rect(r, rad, bw, C.accent);
    else if (w->marked)    stroke_round_rect(r, rad, bw, with_alpha(C.hl, 0.85f));
    else if (w->urgent)    stroke_round_rect(r, rad, bw, C.urgent);
    else if (on_top)       stroke_round_rect(r, rad, SDL_max(1.0f, bw * 0.7f),
                                             with_alpha(C.accent, dimmed ? 0.25f : 0.5f));
    else                   stroke_round_rect(r, rad, SDL_max(1.0f, bw * 0.4f),
                                             with_alpha(C.outline, dimmed ? 0.3f : 0.7f));

    /* the window sway has focused: marked as current, not as selected */
    if (w->focused && !dimmed) {
        SDL_FRect bar = { r.x + bw * 0.6f, r.y + rad * 0.7f,
                          SDL_max(2.0f, 3.0f * SC), r.h - rad * 1.4f };
        fill_round_rect(bar, bar.w * 0.5f, C.current);
    }

    /* multi selection marker */
    if (w->marked) {
        float d = SDL_max(6.0f, 9.0f * SC);
        SDL_FRect dot = { r.x + r.w - d - 5.0f * SC, r.y + 5.0f * SC, d, d };
        fill_round_rect(dot, d * 0.5f, C.hl);
    }

    float pad = card_pad(w);
    float a   = dimmed ? 0.45f : 1.0f;
    SDL_FColor lab_col = (selected || is_hovered) ? C.text : C.subtext;
    if (w->urgent) lab_col = C.urgent;
    SDL_FColor sub_col = mix(C.dim, C.text, (selected || is_hovered) ? 0.55f : 0.32f);
    lab_col = with_alpha(lab_col, lab_col.a * a);
    sub_col = with_alpha(sub_col, sub_col.a * a);

    bool has_icon = C.icons && w->icon && w->lay_icon >= 10.0f * SC;
    if (has_icon) SDL_SetTextureAlphaModFloat(w->icon, a);

    SDL_FRect plate;
    float plate_iw = 0.0f;
    if (as_overlay && overlay_plate(w, r, &plate, &plate_iw)) {
        /* Centre would land on top of the cards showing through, so the name
         * rides in a small plate at the top edge instead. That plate is also
         * the only part of this window the mouse can grab. */
        float ip = 6.0f * SC;
        fill_round_rect(plate, plate.h * 0.32f, with_alpha(mix(C.card, C.bg, 0.25f), 0.94f));
        stroke_round_rect(plate, plate.h * 0.32f, SDL_max(1.0f, 1.4f * SC),
                          is_hovered ? C.accent : with_alpha(C.accent, 0.55f));

        float tx = plate.x + ip;
        if (plate_iw > 0.0f && has_icon) {
            float ih = plate_iw - 5.0f * SC;
            draw_icon(w->icon, tx + ih * 0.5f, plate.y + (plate.h - ih) * 0.5f, ih);
            tx += plate_iw;
        }
        tex_draw(w->label, tx, plate.y + (plate.h - (float)w->label.h) * 0.5f,
                 with_alpha(C.text, a));
        if (has_icon) SDL_SetTextureAlphaModFloat(w->icon, 1.0f);
        return;
    }

    float lab_h = w->label.t    ? (float)w->label.h    : 0.0f;
    float sub_h = w->subtitle.t ? (float)w->subtitle.h : 0.0f;
    float g1 = 4.0f * SC, g2 = 1.0f * SC;

    switch (w->lay_mode) {
    case CL_ROW: {
        float tx = r.x + pad;
        if (has_icon) {
            draw_icon(w->icon, tx + w->lay_icon * 0.5f,
                      r.y + (r.h - w->lay_icon) * 0.5f, w->lay_icon);
            tx += w->lay_icon + 6.0f * SC;
        }
        float block = lab_h + (sub_h > 0.0f ? g2 + sub_h : 0.0f);
        float y = r.y + (r.h - block) * 0.5f;
        tex_draw(w->label, tx, y, lab_col);
        if (sub_h > 0.0f) tex_draw(w->subtitle, tx, y + lab_h + g2, sub_col);
        break;
    }
    case CL_TEXT: {
        float y = r.y + (r.h - lab_h) * 0.5f;
        tex_draw_center(w->label, r.x + r.w * 0.5f, y, lab_col);
        break;
    }
    case CL_ICON: {
        if (has_icon)
            draw_icon(w->icon, r.x + r.w * 0.5f,
                      r.y + (r.h - w->lay_icon) * 0.5f, w->lay_icon);
        break;
    }
    default: {                                     /* CL_STACK */
        float icon = has_icon ? w->lay_icon : 0.0f;
        float block = icon + (lab_h > 0.0f ? g1 + lab_h : 0.0f) +
                             (sub_h > 0.0f ? g2 + sub_h : 0.0f);
        float cx = r.x + r.w * 0.5f;
        /* a window drawn on top keeps to the upper edge, where it is not
         * sitting on the label of whatever shows through beneath it */
        float y  = as_overlay ? r.y + pad : r.y + (r.h - block) * 0.5f;
        if (has_icon) { draw_icon(w->icon, cx, y, icon); y += icon; }
        if (lab_h > 0.0f) { y += g1; tex_draw_center(w->label, cx, y, lab_col); y += lab_h; }
        if (sub_h > 0.0f) { y += g2; tex_draw_center(w->subtitle, cx, y, sub_col); }
        break;
    }
    }

    if (has_icon) SDL_SetTextureAlphaModFloat(w->icon, 1.0f);
}

static void rgb_to_hsv(SDL_FColor c, float *h, float *s, float *v)
{
    float mx = SDL_max(c.r, SDL_max(c.g, c.b));
    float mn = SDL_min(c.r, SDL_min(c.g, c.b));
    float d = mx - mn;
    *v = mx;
    *s = mx > 0.0001f ? d / mx : 0.0f;
    if (d < 0.0001f) { *h = 0.0f; return; }
    if      (mx == c.r) *h = 60.0f * SDL_fmodf((c.g - c.b) / d, 6.0f);
    else if (mx == c.g) *h = 60.0f * ((c.b - c.r) / d + 2.0f);
    else                *h = 60.0f * ((c.r - c.g) / d + 4.0f);
    if (*h < 0.0f) *h += 360.0f;
}

static SDL_FColor hsv_to_rgb(float h, float s, float v, float a)
{
    h = SDL_fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float c = v * s, x = c * (1.0f - SDL_fabsf(SDL_fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c, r = 0, g = 0, b = 0;
    if      (h <  60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return (SDL_FColor){ r + m, g + m, b + m, a };
}

/* One colour per monitor: the accent, turned around the wheel by the golden
 * angle for each screen. Two monitors never look alike, they keep the
 * palette's weight, and none of them lands on `hl` — that one already means
 * "this is the one you are on", and having a whole screen's worth of
 * workspaces wear it said the opposite of what it meant. */
static SDL_FColor output_colour(const char *name)
{
    int at = 0;
    for (int i = 0; i < NOUTS; ++i)
        if (!strcmp(OUTS[i].name, name)) { at = i; break; }

    float h, sat, val, hl_h, hl_s, hl_v;
    rgb_to_hsv(C.accent, &h, &sat, &val);
    rgb_to_hsv(C.hl, &hl_h, &hl_s, &hl_v);

    h += 137.5f * (float)at;                    /* golden angle: no repeats */
    if (sat < 0.35f) sat = 0.55f;               /* a grey accent gives no hue */
    if (val < 0.55f) val = 0.75f;

    for (int guard = 0; guard < 4; ++guard) {   /* keep clear of hl */
        float d = SDL_fabsf(SDL_fmodf(h - hl_h + 540.0f, 360.0f) - 180.0f);
        if (d > 35.0f) break;
        h += 50.0f;
    }
    return hsv_to_rgb(h, sat, val, 1.0f);
}

static SDL_FColor output_colour(const char *name);

static void draw_workspace(int idx)
{
    Ws *ws = &WSS[idx];
    bool selected = sel_active && (idx == sel_ws);
    bool hovered  = false;               /* the selection is the only cursor */
    float rad = C.radius * SC;

    float t = anim_phase();
    if (t < 1.0f && ws->tile_from.w > 0.0f) xf_set(ws->tile, rect_lerp(ws->tile_from, ws->tile, t));
    else xf_off();

    SDL_FRect tile = xf(ws->tile);

    bool has_hit = qlen == 0 || ws->count == 0 || ws_first_visible(ws) >= 0;

    /* A workspace on another screen, shown because something is being
     * dragged: a real place to drop on, but not here. Sinking it into the
     * background was too polite to see at a glance, so it wears its
     * monitor's colour instead — the tile is tinted with it, the border is
     * it, and the name sits in a badge of it. */
    bool elsewhere = ws->output[0] && FOCUSED_OUTPUT[0] &&
                     strcmp(ws->output, FOCUSED_OUTPUT) != 0;
    SDL_FColor ocol = output_colour(ws->output);

    SDL_FColor fill = selected ? C.tile_sel : (hovered ? C.tile_hover : C.tile);
    if (!has_hit) fill = with_alpha(mix(fill, C.bg, 0.35f), fill.a * 0.75f);
    if (elsewhere) fill = mix(fill, ocol, 0.16f);

    drop_shadow(tile, rad, 7.0f * SC, C.shadow_col);
    fill_round_rect(tile, rad, fill);

    SDL_FColor bc = C.outline;
    float bw = C.border * SC;
    if (selected)          bc = C.hl;
    else if (hovered)      bc = with_alpha(C.accent, 0.9f);
    else if (ws->urgent)   bc = C.urgent;
    else if (searching && qlen > 0 && ws_first_visible_match(ws) >= 0)
                           bc = with_alpha(C.match, 0.8f);
    else if (ws->visible)  bc = with_alpha(C.current, 0.75f);
    else                   bw = SDL_max(1.0f, C.border * SC * 0.45f);
    if (elsewhere && !selected) {
        bc = with_alpha(ocol, 0.95f);
        bw = SDL_max(1.5f, C.border * SC * 1.3f);
    }
    stroke_round_rect(tile, rad, bw, bc);

    /* ---- tile header: count left, name centred, number right ---- */
    float p = C.pad * SC;
    float top = tile.y + C.badge_top * SC;

    bool live = ws->visible || ws->focused;
    SDL_FColor num_col = selected ? C.hl
                       : live     ? C.current
                                  : mix(C.subtext, C.tile, 0.25f);
    if (ws->urgent && !selected) num_col = C.urgent;

    tex_draw(ws->badge, tile.x + tile.w - p - (float)ws->badge.w, top, num_col);


    float count_right = tile.x + p;
    if (ws->sub.t) {
        float base = top + ((float)ws->badge.h - (float)ws->sub.h) * 0.62f;
        SDL_FColor col = ws->count ? mix(C.dim, C.text, selected ? 0.45f : 0.25f)
                                   : with_alpha(C.dim, 0.85f);
        tex_draw(ws->sub, tile.x + p, base, col);
        count_right += (float)ws->sub.w;
    }

    /* the name sits between the two; clicking it starts a rename */
    (void)count_right;
    SDL_FRect tb = xf(ws->title_box);
    float tb_x = tb.x, tb_w = tb.w;

    bool editing_this = editing && edit_ws == idx;
    if (editing_this) {
        SDL_FRect box = { tb_x, top - p * 0.35f, SDL_max(tb_w, 40.0f * SC),
                          (float)ws->badge.h + p * 0.7f };
        fill_round_rect(box, box.h * 0.3f, mix(C.tile, C.bg, 0.2f));
        stroke_round_rect(box, box.h * 0.3f, SDL_max(1.0f, 1.5f * SC), C.hl);

        char buf[80];
        snprintf(buf, sizeof(buf), "%.63s\xe2\x96\x8f", edit_buf);
        Tex t = text_make_fit(F_LABEL, buf[0] ? buf : "\xe2\x96\x8f", (int)(box.w - p));
        tex_draw(t, box.x + p * 0.5f, box.y + (box.h - (float)t.h) * 0.5f, C.text);
        tex_free(&t);
    } else if (elsewhere && tb_w > 20.0f * SC) {
        /* the monitor first, in its own colour, then the workspace */
        Tex on = text_make_fit(F_LABEL, ws->output, (int)(tb_w * 0.55f));
        float bh = (float)on.h + p * 0.5f;
        SDL_FRect pill = { tb_x, top - p * 0.2f, (float)on.w + p, bh };
        fill_round_rect(pill, bh * 0.35f, ocol);
        tex_draw(on, pill.x + p * 0.5f, pill.y + (bh - (float)on.h) * 0.5f, C.bg);
        tex_free(&on);

        if (ws->title.t) {
            float left = pill.x + pill.w + p * 0.6f;
            Tex t = text_make_fit(F_LABEL, ws->name, (int)(tb_x + tb_w - left));
            tex_draw(t, left, top + (bh - (float)t.h) * 0.5f - p * 0.2f,
                     with_alpha(C.text, 0.9f));
            tex_free(&t);
        }
    } else if (ws->title.t && tb_w > 20.0f * SC) {
        Tex t = ws->title;
        float x = tb_x + (tb_w - (float)t.w) * 0.5f;
        if ((float)t.w > tb_w) x = tb_x;
        SDL_FColor col = selected ? C.text : mix(C.subtext, C.tile, 0.15f);
        tex_draw(t, x, top + ((float)ws->badge.h - (float)t.h) * 0.55f, col);
    }

    /* How much this workspace gets used, as a column of dots down the left
     * edge: the more of them are lit, the more time is spent here. The scale
     * is relative to the busiest workspace. */
    if (C.usage_dots && C.dot_count > 0) {
        float d = C.dot_px * SC;
        float gap = d * 0.62f;

        SDL_FRect screen_now = xf(ws->screen);
        int fits = (int)((screen_now.h + gap) / (d + gap));
        int n = SDL_clamp(SDL_min(C.dot_count, fits), 0, 40);
        if (n < 2) n = 0;
        float total = (float)n * d + (float)(n - 1) * gap;
        float cx = tile.x + C.pad * SC + d * 0.55f;
        float y0 = screen_now.y + (screen_now.h - total) * 0.5f;

        float frac = (USAGE_MAX > 0.0) ? (float)(ws->usage / USAGE_MAX) : 0.0f;
        int lit = (ws->usage > 0.0) ? (int)SDL_ceilf(frac * (float)n) : 0;
        lit = SDL_clamp(lit, 0, n);

        for (int i = 0; i < n; ++i) {
            /* fills from the bottom like a level gauge */
            bool on = (n - i) <= lit;
            SDL_FRect dot = { cx - d * 0.5f, y0 + (float)i * (d + gap), d, d };
            fill_round_rect(dot, d * 0.5f,
                            on ? C.current : with_alpha(C.dim, 0.38f));
        }

        /* the same scale down the other edge, for what the workspace is doing
         * now rather than how long you have spent there — swbr measures it,
         * and draws the identical dots on its own workspace buttons */
        float cpu = cpu_frac(ws->name);
        if (cpu >= 0.0f) {
            float cxr = tile.x + tile.w - C.pad * SC - d * 0.55f;
            int   cl  = SDL_clamp((int)SDL_ceilf(cpu * (float)n), 1, n);
            SDL_FColor hot = cpu < 0.85f ? C.accent
                                         : mix(C.accent, C.urgent, (cpu - 0.85f) / 0.15f);
            /* below the scale but not asleep: one dot, faint */
            if (cpu <= 0.0f) hot = with_alpha(mix(C.dim, C.accent, 0.4f), 0.55f);
            for (int i = 0; i < n; ++i) {
                bool on = (n - i) <= cl;
                SDL_FRect dot = { cxr - d * 0.5f, y0 + (float)i * (d + gap), d, d };
                fill_round_rect(dot, d * 0.5f,
                                on ? hot : with_alpha(C.dim, 0.30f));
            }
        }
    }

    /* ---- the mini screen ---- */
    SDL_FRect screen = xf(ws->screen);
    float srad = SDL_min(rad * 0.7f, SDL_min(screen.w, screen.h) * 0.5f);
    fill_round_rect(screen, srad, C.mini_bg);
    stroke_round_rect(screen, srad, SDL_max(1.0f, 1.2f * SC), with_alpha(C.outline, 0.55f));

    if (ws->count == 0) {
        /* nothing here: keep the tile obviously "a screen, but empty" */
        xf_off();
        return;
    }

    /* a container whose children share one rectangle (tabbed or stacked) is
     * drawn as slices; a shared outline shows that they belong together */
    for (int i = 0; i < ws->count; ++i) {
        if (WINS[ws->first + i].group != i) continue;
        SDL_FRect u = WINS[ws->first + i].card;
        for (int j = i + 1; j < ws->count; ++j) {
            if (WINS[ws->first + j].group != i) continue;
            SDL_FRect c = WINS[ws->first + j].card;
            float x1 = SDL_max(u.x + u.w, c.x + c.w), y1 = SDL_max(u.y + u.h, c.y + c.h);
            u.x = SDL_min(u.x, c.x);
            u.y = SDL_min(u.y, c.y);
            u.w = x1 - u.x;
            u.h = y1 - u.y;
        }
        u = xf(u);
        float e = 3.0f * SC;
        SDL_FRect box = { u.x - e, u.y - e, u.w + 2.0f * e, u.h + 2.0f * e };
        stroke_round_rect(box, C.radius * SC * 0.7f, SDL_max(1.0f, 1.6f * SC),
                          with_alpha(C.accent, 0.5f));

        if (WINS[ws->first + i].has_tab) {
            /* The strip the tabs sit on, exactly as wide as the tabs. It used
             * to span the group's whole box, which left a dark band sticking
             * out either side of the tabs once they were inset — the shadow
             * that looked like the background bleeding over the edges. */
            float x0 = 1e9f, x1 = -1e9f, ty = 0.0f, th = 0.0f;
            for (int j = 0; j < ws->count; ++j) {
                Win *tw = &WINS[ws->first + j];
                if (tw->group != i || !tw->has_tab) continue;
                SDL_FRect t = xf(tw->tab);
                if (t.x < x0) x0 = t.x;
                if (t.x + t.w > x1) x1 = t.x + t.w;
                ty = t.y; th = t.h;
            }
            if (x1 > x0) {
                SDL_FRect strip = { x0 - e * 0.5f, ty - e * 0.5f,
                                    x1 - x0 + e, th + e };
                fill_round_rect(strip, C.radius * SC * 0.5f,
                                with_alpha(C.mini_bg, 0.8f));
            }
        }
    }

    for (int i = 0; i < ws->count; ++i) {
        int gi = ws->first + i;
        draw_card(&WINS[gi], selected);
    }
    xf_off();
}


/* ---- what the drop would do, shown while the button is still held ---- */
static void draw_drop_indicator(void)
{
    float thick = SDL_max(2.0f, C.border * SC * 1.2f);

    switch (drop_kind) {
    case DROP_WIN_WS:
        if (drop_ws >= 0) {
            SDL_FRect r = WSS[drop_ws].screen;
            fill_round_rect(r, C.radius * SC * 0.7f, with_alpha(C.hl, 0.13f));
            stroke_round_rect(r, C.radius * SC * 0.7f, thick, C.hl);
        }
        break;

    case DROP_WIN_NEAR:
        if (drop_win >= 0) {
            SDL_FRect r = WINS[drop_win].card;
            stroke_round_rect(r, C.radius * SC * 0.6f, SDL_max(1.0f, thick * 0.6f),
                              with_alpha(C.accent, 0.8f));

            /* the bar shows which side the window lands on, and therefore
             * whether the container ends up split horizontally or vertically */
            float b = thick * 1.6f;
            SDL_FRect bar = r;
            if (drop_edge == EDGE_LEFT)        bar = (SDL_FRect){ r.x - b * 0.5f, r.y, b, r.h };
            else if (drop_edge == EDGE_RIGHT)  bar = (SDL_FRect){ r.x + r.w - b * 0.5f, r.y, b, r.h };
            else if (drop_edge == EDGE_TOP)    bar = (SDL_FRect){ r.x, r.y - b * 0.5f, r.w, b };
            else                               bar = (SDL_FRect){ r.x, r.y + r.h - b * 0.5f, r.w, b };
            fill_round_rect(bar, b * 0.5f, C.hl);
        }
        break;

    case DROP_WS_EDGE:
        if (drop_ws >= 0) {
            SDL_FRect r = WSS[drop_ws].screen;
            stroke_round_rect(r, C.radius * SC * 0.7f, SDL_max(1.0f, thick * 0.6f),
                              with_alpha(C.accent, 0.8f));
            float b = thick * 1.8f;
            SDL_FRect bar;
            if (drop_edge == EDGE_LEFT)        bar = (SDL_FRect){ r.x, r.y, b, r.h };
            else if (drop_edge == EDGE_RIGHT)  bar = (SDL_FRect){ r.x + r.w - b, r.y, b, r.h };
            else if (drop_edge == EDGE_TOP)    bar = (SDL_FRect){ r.x, r.y, r.w, b };
            else                               bar = (SDL_FRect){ r.x, r.y + r.h - b, r.w, b };
            fill_round_rect(bar, b * 0.5f, C.hl);
        }
        break;

    case DROP_WS_SWAP:
        if (drop_ws >= 0) {
            SDL_FRect t = WSS[drop_ws].tile;
            if (drop_insert) {
                float b = thick * 1.8f;
                float x = (drop_edge == EDGE_LEFT) ? t.x - b : t.x + t.w;
                SDL_FRect bar = { x, t.y, b, t.h };
                fill_round_rect(bar, b * 0.5f, C.hl);
            } else {
                fill_round_rect(t, C.radius * SC, with_alpha(C.hl, 0.16f));
                stroke_round_rect(t, C.radius * SC, thick, C.hl);
            }
        }
        break;

    default:
        break;
    }

    /* the thing being dragged, held under the pointer */
    if (drag_ws_mode && press_ws >= 0) {
        SDL_FRect t = WSS[press_ws].tile;
        float w = t.w * 0.34f, h = t.h * 0.34f;
        SDL_FRect g = { drag_x - w * 0.5f, drag_y - h * 0.5f, w, h };
        fill_round_rect(g, C.radius * SC * 0.7f, with_alpha(C.tile_sel, 0.92f));
        stroke_round_rect(g, C.radius * SC * 0.7f, thick, C.hl);
        tex_draw_in_box(WSS[press_ws].badge, g, F_BADGE, C.text);
    } else if (press_win >= 0) {
        Win *w = &WINS[press_win];
        SDL_FRect r = w->card;
        SDL_FRect g = { drag_x - r.w * 0.5f, drag_y - r.h * 0.5f, r.w, r.h };
        fill_round_rect(g, C.radius * SC * 0.6f, with_alpha(C.card_hover, 0.9f));
        stroke_round_rect(g, C.radius * SC * 0.6f, thick, C.hl);
        if (w->icon && C.icons) {
            float sz = SDL_min(SDL_min(g.w, g.h) * 0.5f, (float)C.icon_px * SC);
            draw_icon(w->icon, g.x + g.w * 0.5f, g.y + (g.h - sz) * 0.5f, sz);
        }
    }
}

/* The monitors as they sit on the desk, scaled into whatever corner of the
 * grid was left over. Click one to look at its workspaces instead; the one
 * you are looking at is filled, the one sway is really on keeps a ring. */
static void draw_outputs_map(void)
{
    if (MAP_RECT.w < 16.0f || NOUTS < 2) return;

    int minx = OUTS[0].x, miny = OUTS[0].y;
    int maxx = OUTS[0].x + OUTS[0].w, maxy = OUTS[0].y + OUTS[0].h;
    for (int i = 1; i < NOUTS; ++i) {
        if (OUTS[i].x < minx) minx = OUTS[i].x;
        if (OUTS[i].y < miny) miny = OUTS[i].y;
        if (OUTS[i].x + OUTS[i].w > maxx) maxx = OUTS[i].x + OUTS[i].w;
        if (OUTS[i].y + OUTS[i].h > maxy) maxy = OUTS[i].y + OUTS[i].h;
    }
    float dw = (float)(maxx - minx), dh = (float)(maxy - miny);
    if (dw < 1.0f || dh < 1.0f) return;

    /* fit the whole desk inside its box, aspect kept */
    float pad = C.pad * SC * 0.5f;
    float aw = MAP_RECT.w - 2.0f * pad, ah = MAP_RECT.h - 2.0f * pad;
    float sc = SDL_min(aw / dw, ah / dh);
    float bw = dw * sc, bh = dh * sc;
    float bx = MAP_RECT.x + (MAP_RECT.w - bw) * 0.5f;
    float by = MAP_RECT.y + (MAP_RECT.h - bh) * 0.5f;

    float rad = SDL_max(2.0f, 4.0f * SC);
    for (int i = 0; i < NOUTS; ++i) {
        Out *d = &OUTS[i];
        SDL_FRect r = { bx + (float)(d->x - minx) * sc + 1.0f * SC,
                        by + (float)(d->y - miny) * sc + 1.0f * SC,
                        (float)d->w * sc - 2.0f * SC,
                        (float)d->h * sc - 2.0f * SC };
        d->box = r;
        if (r.w < 4.0f || r.h < 4.0f) continue;

        bool showing = strcmp(d->name, FOCUSED_OUTPUT) == 0;

        /* Held over: the plate presses in a little and fills from the left,
         * so the wait reads as a button going down rather than a pause. */
        float held = 0.0f;
        if (i == MAP_HOVER && C.map_dwell_ms > 0)
            held = SDL_clamp((float)((now_secs() - MAP_HOVER_SINCE) /
                                     (C.map_dwell_ms / 1000.0)), 0.0f, 1.0f);
        if (held > 0.0f) {
            float in = held * SDL_min(r.w, r.h) * 0.06f;   /* pressed in */
            r.x += in; r.y += in; r.w -= 2.0f * in; r.h -= 2.0f * in;
        }

        /* the same colour its workspaces wear while dragging, so the map and
           the grid agree about which screen is which */
        SDL_FColor oc = output_colour(d->name);
        fill_round_rect(r, rad, showing ? mix(C.tile_sel, oc, 0.30f)
                                        : mix(with_alpha(C.tile, 0.8f), oc, 0.35f));
        stroke_round_rect(r, rad, SDL_max(1.0f, 1.2f * SC),
                          with_alpha(oc, showing ? 0.95f : 0.7f));

        if (held > 0.0f) {
            SDL_FRect fillr = { r.x, r.y, r.w * held, r.h };
            SDL_Rect clip = { (int)r.x, (int)r.y, (int)SDL_ceilf(fillr.w), (int)SDL_ceilf(r.h) };
            SDL_Rect prev;
            bool had = SDL_GetRenderClipRect(REN, &prev) && SDL_RenderClipEnabled(REN);
            SDL_SetRenderClipRect(REN, &clip);
            fill_round_rect(r, rad, with_alpha(C.hl, 0.55f + 0.35f * held));
            SDL_SetRenderClipRect(REN, had ? &prev : NULL);
            stroke_round_rect(r, rad, SDL_max(1.0f, 2.0f * SC), with_alpha(C.hl, 0.95f));
        }

        if (d->focused)
            stroke_round_rect(r, rad, SDL_max(1.0f, 2.0f * SC),
                              with_alpha(C.current, 0.9f));

        Tex t = text_make_fit(F_HINT, d->name, (int)(r.w - 4.0f * SC));
        if (t.t) {
            tex_draw(t, r.x + (r.w - (float)t.w) * 0.5f,
                     r.y + (r.h - (float)t.h) * 0.5f,
                     showing ? C.hltext : with_alpha(C.text, 0.85f));
            tex_free(&t);
        }
    }
}

/* Looking at a screen you are not sitting in front of is easy to forget, and
 * everything you press lands over there. The whole overview gets a frame in
 * that monitor's colour, the same one its workspaces and its plate on the map
 * wear — hard to mistake for the ordinary view, impossible to miss. */
static void draw_pinned_frame(void)
{
    if (!PINNED_OUTPUT || !FOCUSED_OUTPUT[0]) return;

    SDL_FColor oc = output_colour(FOCUSED_OUTPUT);
    float w = SDL_max(3.0f, 5.0f * SC);
    SDL_FRect r = { w * 0.5f, w * 0.5f, (float)RW - w, (float)RH - w };
    stroke_round_rect(r, C.radius * SC, w, with_alpha(oc, 0.9f));

    /* and a second, softer one just inside it, so it reads as a frame rather
       than a window border the compositor drew */
    SDL_FRect inner = { r.x + w, r.y + w, r.w - 2.0f * w, r.h - 2.0f * w };
    stroke_round_rect(inner, C.radius * SC, SDL_max(1.0f, 1.5f * SC),
                      with_alpha(oc, 0.28f));
}

static void draw_cancel_target(void)
{
    if (CANCEL_RECT.w < 8.0f || !drag_active) return;

    SDL_FRect r = CANCEL_RECT;
    bool hot = drop_kind == DROP_CANCEL;
    float rad = C.radius * SC;                 /* the same corner as a tile */

    fill_round_rect(r, rad, hot ? with_alpha(C.urgent, 0.18f)
                                : with_alpha(C.tile, 0.28f));
    if (hot)
        stroke_round_rect(r, rad, SDL_max(1.0f, 1.5f * SC),
                          with_alpha(C.urgent, 0.85f));

    /* A small ✕ in the middle, two strokes rather than a line of dots — the
     * dots were what made it look chewed. */
    float box = r.h * 0.20f, th = SDL_max(1.0f, 1.6f * SC);
    SDL_FColor c = with_alpha(hot ? C.urgent : C.subtext, hot ? 0.95f : 0.45f);
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;

    set_col(c);
    for (float o = -th * 0.5f; o <= th * 0.5f; o += 0.7f) {
        SDL_RenderLine(REN, cx - box + o, cy - box, cx + box + o, cy + box);
        SDL_RenderLine(REN, cx - box + o, cy + box, cx + box + o, cy - box);
    }
}

static void render(void)
{
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_NONE);
    set_col(C.bg);
    SDL_RenderClear(REN);
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);

    if (NWS == 0) {
        Tex t = text_make(F_LABEL, "no windows found on this output");
        tex_draw_center(t, (float)RW * 0.5f, (float)RH * 0.5f, C.subtext);
        tex_free(&t);
        return;
    }

    if (ghost_lo >= 0)                            /* free numbers to drop on */
        for (int i = 0; i < NSLOTS; ++i) {
            if (SLOTS[i].ws >= 0) continue;
            float ph = anim_phase();
            SDL_FRect t = rect_lerp(SLOTS[i].from, SLOTS[i].tile, ph);
            float ix = t.w * 0.22f, iy = t.h * 0.22f;
            SDL_FRect g = { t.x + ix, t.y + iy, t.w - 2.0f * ix, t.h - 2.0f * iy };
            bool hot = (drop_kind == DROP_WS_NUM || drop_kind == DROP_WIN_NEWWS) &&
                       drop_num == SLOTS[i].num;

            fill_round_rect(g, C.radius * SC * 0.8f,
                            with_alpha(hot ? C.hl : C.tile, (hot ? 0.30f : 0.55f) * ph));
            stroke_round_rect(g, C.radius * SC * 0.8f, SDL_max(1.0f, 2.0f * SC),
                              with_alpha(hot ? C.hl : C.accent, (hot ? 1.0f : 0.35f) * ph));

            char num[16];
            snprintf(num, sizeof(num), "%d", SLOTS[i].num);
            Tex t2 = text_make(F_BADGE, num);
            tex_draw_in_box(t2, g, F_BADGE, with_alpha(hot ? C.hl : C.dim, 0.9f * ph));
            tex_free(&t2);
        }

    if (NDYING > 0) {                             /* ghosts fading away */
        float ph = phase_since(dying_start);
        if (ph >= 1.0f) NDYING = 0;
        for (int i = 0; i < NDYING; ++i) {
            SDL_FRect t = rect_lerp(DYING[i].from, DYING[i].tile, ph);
            float ix = t.w * 0.22f, iy = t.h * 0.22f;
            SDL_FRect g = { t.x + ix, t.y + iy, t.w - 2.0f * ix, t.h - 2.0f * iy };
            float a = 1.0f - ph;

            fill_round_rect(g, C.radius * SC * 0.8f, with_alpha(C.tile, 0.55f * a));
            stroke_round_rect(g, C.radius * SC * 0.8f, SDL_max(1.0f, 2.0f * SC),
                              with_alpha(C.accent, 0.35f * a));

            char num[16];
            snprintf(num, sizeof(num), "%d", DYING[i].num);
            Tex t2 = text_make(F_BADGE, num);
            tex_draw_in_box(t2, g, F_BADGE, with_alpha(C.dim, 0.9f * a));
            tex_free(&t2);
        }
    }

    for (int i = 0; i < NWS; ++i)
        if (i != sel_ws) draw_workspace(i);
    draw_workspace(sel_ws);                       /* selection is drawn last */

    draw_outputs_map();
    draw_cancel_target();
    draw_pinned_frame();

    if (drag_active) draw_drop_indicator();

    float m = C.margin * SC;
    float head_x0 = (float)RW, head_x1 = 0.0f;
    bool  head_top = pos_is_top(C.header_pos);

    if (T_HEADER.t && !pos_is_none(C.header_pos)) {
        float y = head_top ? (HEADER_H - (float)T_HEADER.h) * 0.5f
                           : (float)RH - FOOTER_H * 0.5f - (float)T_HEADER.h * 0.5f;
        head_x0 = pos_x(C.header_pos, (float)T_HEADER.w, m);
        head_x1 = head_x0 + (float)T_HEADER.w;
        SDL_FColor pin = output_colour(FOCUSED_OUTPUT);
        if (PINNED_OUTPUT) {                       /* not the screen you are on */
            float p = 8.0f * SC;
            SDL_FRect box = { head_x0 - p, y - p * 0.5f,
                              (float)T_HEADER.w + 2.0f * p, (float)T_HEADER.h + p };
            fill_round_rect(box, box.h * 0.35f, pin);
        }
        tex_draw(T_HEADER, head_x0, y, PINNED_OUTPUT ? C.bg : C.hint);
    }

    if (T_QUERY.t) {
        float p = 10.0f * SC;
        SDL_FRect box = { (float)RW * 0.5f - ((float)T_QUERY.w * 0.5f + p),
                          (HEADER_H - (float)T_QUERY.h) * 0.55f - p * 0.5f,
                          (float)T_QUERY.w + 2.0f * p, (float)T_QUERY.h + p };
        SDL_FColor qc = confirm_kill ? C.urgent : (filtering ? C.accent : C.match);
        fill_round_rect(box, box.h * 0.35f, mix(C.tile, C.bg, 0.15f));
        stroke_round_rect(box, box.h * 0.35f, SDL_max(1.0f, 1.5f * SC), with_alpha(qc, 0.8f));
        tex_draw(T_QUERY, box.x + p, box.y + p * 0.5f, qc);
    }

    if (T_HINTS.t && !pos_is_none(C.hints_pos)) {
        bool hint_top = pos_is_top(C.hints_pos);
        float y = hint_top ? (HEADER_H - (float)T_HINTS.h) * 0.5f
                           : (float)RH - FOOTER_H * 0.5f - (float)T_HINTS.h * 0.5f;
        float x = pos_x(C.hints_pos, (float)T_HINTS.w, m);

        /* both lines in the same band: keep the hints clear of the header */
        if (T_HEADER.t && head_top == hint_top && x + (float)T_HINTS.w > head_x0 - m * 0.5f) {
            if (head_x0 > (float)RW * 0.5f) x = SDL_min(x, head_x0 - m * 0.5f - (float)T_HINTS.w);
            else                            x = SDL_max(x, head_x1 + m * 0.5f);
            x = SDL_max(x, m * 0.5f);
        }
        tex_draw(T_HINTS, x, y, with_alpha(C.hint, 0.8f));
    }
}


/* --------------------------------------------------------------- actions */

static bool running = true;
static bool BACKDROP;            /* drawn behind another program's window   */

/* swbr writes a new set of numbers every few seconds; the overview sits open
 * for longer than that, so it watches the file rather than reading it once. */
static void cpu_poll(void)
{
    if (!C.cpu) return;

    static Uint64 next;
    Uint64 now = SDL_GetTicks();
    if (now < next) return;
    next = now + 1000;

    char path[512];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(path, sizeof(path), "%s/swbr-cpu", rt);
    else {
        const char *home = getenv("HOME");
        if (!home) return;
        snprintf(path, sizeof(path), "%s/.cache/swbr-cpu", home);
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_mtime == CPU_MTIME) return;
    cpu_load();
    dirty = true;
}

/* A fullscreen window is above everything a normal window can reach, so an
 * overlay that is not on the layer shell simply cannot be seen over it. The
 * only honest way is to take it off fullscreen for as long as we are up, and
 * put it back exactly as it was on the way out. */
static int FS_RESTORE;

static void fullscreen_step_aside(void)
{
    if (!C.over_fullscreen || BACKDROP) return;

    for (int i = 0; i < NWIN; ++i) {
        if (!WINS[i].fullscreen) continue;
        if (FOCUSED_OUTPUT[0] && WINS[i].ws >= 0 && WINS[i].ws < NWS &&
            strcmp(WSS[WINS[i].ws].output, FOCUSED_OUTPUT) != 0) continue;
        FS_RESTORE = WINS[i].con_id;
        sway_cmd("[con_id=%d] fullscreen disable", FS_RESTORE);
        break;
    }
}

static void fullscreen_put_back(void)
{
    if (FS_RESTORE <= 0) return;
    sway_cmd("[con_id=%d] fullscreen enable", FS_RESTORE);
    FS_RESTORE = 0;
}

static void reload_model(void)
{
    /* remember what was selected so a reload does not lose the cursor */
    int keep_con = -1;
    char keep_ws[64] = {0};
    if (NWS > 0) {
        str_set(keep_ws, sizeof(keep_ws), WSS[sel_ws].name);
        Win *w = ws_sel_win(&WSS[sel_ws]);
        if (w) keep_con = w->con_id;
    }
    int marks[MAX_WINDOWS];
    int nmarks = 0;
    for (int i = 0; i < NWIN && nmarks < MAX_WINDOWS; ++i)
        if (WINS[i].marked) marks[nmarks++] = WINS[i].con_id;

    if (!model_reload()) {
        /* The tree is being read while sway is busy starting something; a
         * miss is not a reason to disappear. The overlay quits over it, the
         * backdrop keeps what it had and tries again on the next event. */
        if (!BACKDROP) running = false;
        return;
    }

    for (int i = 0; i < NWIN; ++i)
        for (int j = 0; j < nmarks; ++j)
            if (WINS[i].con_id == marks[j]) WINS[i].marked = true;

    select_current_workspace();

    for (int i = 0; i < NWS; ++i) {
        WSS[i].sel = -1;
        if (keep_ws[0] && strcmp(WSS[i].name, keep_ws) == 0) sel_ws = i;
        for (int j = 0; j < WSS[i].count; ++j)
            if (WINS[WSS[i].first + j].con_id == keep_con) { sel_ws = i; WSS[i].sel = j; }
    }

    cpu_load();                  /* whatever swbr last measured */
    focus_load();                /* and the order things were used in */
    mark("cpu numbers");
    if (!FS_RESTORE) fullscreen_step_aside();
    layout();
    apply_filter();
    rebuild_chrome();
    hov_ws = hov_win = -1;
}

static void act_focus_window(Win *w)
{
    if (!w) return;
    sway_cmd("[con_id=%d] focus", w->con_id);
    running = false;
}

static void act_goto_workspace(const Ws *ws)
{
    if (!ws) return;
    char *e = escape_arg(ws->name);
    sway_cmd("workspace --no-auto-back-and-forth \"%s\"", e);
    free(e);
    if (C.track) usage_switch(ws->name, ws->output, ws->num);
    running = false;
}

/* windows the next action applies to: all marked ones, else the selected one.
 * Returns 0 when the selection is a whole workspace rather than windows. */
static int gather_targets(int *out, int cap)
{
    int n = 0;
    for (int i = 0; i < NWIN && n < cap; ++i)
        if (WINS[i].marked) out[n++] = WINS[i].con_id;
    if (n == 0 && NWS > 0) {
        Win *w = ws_sel_win(&WSS[sel_ws]);
        if (w) out[n++] = w->con_id;
    }
    return n;
}

static void act_move_to_workspace(int number)
{
    int ids[MAX_WINDOWS];
    int n = gather_targets(ids, MAX_WINDOWS);

    if (n > 0) {
        for (int i = 0; i < n; ++i)
            sway_cmd("[con_id=%d] move container to workspace number %d", ids[i], number);
    } else if (NWS > 0 && WSS[sel_ws].ntop > 0) {
        /* Whole workspace: move its top level containers, not the single
         * windows. A container carries its own split/tab layout along, so
         * the arrangement survives the move. */
        Ws *ws = &WSS[sel_ws];
        if (ws->num == number) return;
        for (int i = 0; i < ws->ntop; ++i)
            sway_cmd("[con_id=%d] move container to workspace number %d", ws->top[i], number);
    } else return;

    for (int i = 0; i < NWIN; ++i) WINS[i].marked = false;
    if (C.quit_after_action) { running = false; return; }
    reload_model();
}

static void act_close(void)
{
    int ids[MAX_WINDOWS];
    int n = gather_targets(ids, MAX_WINDOWS);

    if (n == 0 && NWS > 0 && WSS[sel_ws].sel < 0) {   /* the whole workspace */
        Ws *ws = &WSS[sel_ws];
        for (int i = 0; i < ws->count && n < MAX_WINDOWS; ++i)
            ids[n++] = WINS[ws->first + i].con_id;
    }
    for (int i = 0; i < n; ++i) sway_cmd("[con_id=%d] kill", ids[i]);
    for (int i = 0; i < NWIN; ++i) WINS[i].marked = false;
    reload_model();
}

static void act_toggle_mark(Win *w)
{
    if (w) w->marked = !w->marked;
}

/* mark every window of the workspace, or clear them all if they already are */
static void toggle_ws_marks(Ws *ws)
{
    bool all = ws->count > 0;
    for (int i = 0; i < ws->count; ++i)
        if (!WINS[ws->first + i].marked) { all = false; break; }
    for (int i = 0; i < ws->count; ++i) WINS[ws->first + i].marked = !all;
}

/* ----------------------------------------------------------------- fonts */

/* One fc-match run resolves the regular and the bold cut together; spawning
 * a process twice is the most expensive thing startup would otherwise do. */
static char *FC_REGULAR, *FC_BOLD;

static void fontconfig_resolve(const char *family)
{
    /* the family ends up in a shell command, so keep it to harmless characters */
    char fam[128] = "sans";
    if (family && *family) {
        size_t n = 0;
        for (const char *p = family; *p && n < sizeof(fam) - 1; ++p)
            if (isalnum((unsigned char)*p) || *p == ' ' || *p == '-' ||
                *p == '_' || *p == '.' || *p == ',') fam[n++] = *p;
        fam[n] = 0;
        if (!n) snprintf(fam, sizeof(fam), "sans");
    }

    /* The answer never changes between runs, and a shell plus two fc-match
     * processes is the most expensive thing startup does — tens of
     * milliseconds when the fontconfig cache is warm, far worse when it is
     * not. Keep it next to the usage file and the whole thing becomes two
     * reads. */
    char cache[512] = "";
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)       snprintf(cache, sizeof(cache), "%s/swov/fonts", xdg);
    else if (home && *home) snprintf(cache, sizeof(cache), "%s/.cache/swov/fonts", home);

    char l1[1024] = {0}, l2[1024] = {0}, want[160] = {0};
    snprintf(want, sizeof(want), "%s", fam);

    if (cache[0]) {                       /* family, then the two paths */
        FILE *c = fopen(cache, "r");
        if (c) {
            char had[160] = {0};
            if (fgets(had, sizeof(had), c)) str_trim(had);
            if (fgets(l1, sizeof(l1), c))   str_trim(l1);
            if (fgets(l2, sizeof(l2), c))   str_trim(l2);
            fclose(c);
            if (strcmp(had, want) || !file_readable(l1)) l1[0] = l2[0] = 0;
        }
    }

    if (!l1[0]) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "fc-match -f '%%{file}\n' '%s' 2>/dev/null; "
                 "fc-match -f '%%{file}\n' '%s:bold' 2>/dev/null", fam, fam);

        FILE *p = popen(cmd, "r");
        if (!p) return;
        if (fgets(l1, sizeof(l1), p)) str_trim(l1);
        if (fgets(l2, sizeof(l2), p)) str_trim(l2);
        pclose(p);

        if (cache[0] && l1[0]) {
            char *slash = strrchr(cache, '/');       /* mkdir -p, one level */
            if (slash) {
                *slash = 0;
                char *up = strrchr(cache, '/');
                if (up) { *up = 0; mkdir(cache, 0755); *up = '/'; }
                mkdir(cache, 0755);
                *slash = '/';
            }
            FILE *c = fopen(cache, "w");
            if (c) { fprintf(c, "%s\n%s\n%s\n", want, l1, l2); fclose(c); }
        }
    }

    if (l1[0] && file_readable(l1)) FC_REGULAR = xstrdup(l1);
    if (l2[0] && file_readable(l2)) FC_BOLD    = xstrdup(l2);
}

static char *pick_font(const char *spec, bool bold)
{
    if (spec && *spec && strchr(spec, '/') && file_readable(spec)) return xstrdup(spec);

    const char *hit = bold ? FC_BOLD : FC_REGULAR;
    if (hit) return xstrdup(hit);

    const char *fallback[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"
    };
    for (size_t i = 0; i < SDL_arraysize(fallback); ++i)
        if (file_readable(fallback[i])) return xstrdup(fallback[i]);
    return NULL;
}

static TTF_Font *open_font(const char *path, float size, bool bold, bool synth_bold)
{
    TTF_Font *f = TTF_OpenFont(path, size);
    if (!f) return NULL;
    if (bold && synth_bold) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
    return f;
}

static void load_fonts(void)
{
    /* a configured path needs no fontconfig at all */
    bool have_paths = C.font[0] && strchr(C.font, '/') &&
                      (!C.font_bold[0] || strchr(C.font_bold, '/'));
    if (!have_paths) fontconfig_resolve(C.font);
    mark("font lookup");

    char *regular = pick_font(C.font, false);
    if (!regular) die("no usable font found (install fontconfig and a TTF font)");
    char *bold = C.font_bold[0] ? pick_font(C.font_bold, true) : pick_font(C.font, true);
    bool synth = false;
    if (!bold) { bold = xstrdup(regular); synth = true; }
    else if (strcmp(bold, regular) == 0) synth = true;

    float u = C.ui_scale * SC;
    F_BADGE = open_font(bold,    (float)C.ws_px    * u, true, synth);
    F_LABEL = open_font(bold,    (float)C.label_px * u, true, synth);
    F_TITLE = open_font(regular, (float)C.title_px * u, false, false);
    F_HINT  = open_font(regular, (float)C.hint_px  * u, false, false);
    free(regular);
    free(bold);
    free(FC_REGULAR);
    free(FC_BOLD);
    FC_REGULAR = FC_BOLD = NULL;

    if (!F_BADGE || !F_LABEL || !F_TITLE || !F_HINT) die("could not open fonts: %s", SDL_GetError());
    mark("font open");
}

/* ------------------------------------------------------------------ main */

static SDL_Window  *WIN_HANDLE;
static const char  *SHOT_PATH;
static float        SHOT_MX = -1.0f, SHOT_MY = -1.0f;
static SDL_Texture *TARGET;
static SDL_Cursor  *CUR_ARROW, *CUR_HAND;
static float        MOUSE_SCALE = 1.0f;

/* --- backdrop: swov drawn behind another program's window ---------------
 * No input of its own; a launcher in front sends pointer positions on stdin
 * and gets back the workspace under them. It fades in so that picking an app
 * before the tree is even read shows nothing at all, rather than a frame that
 * flashes into view. */
/* Anything else starting with a dash is a typo, and saying so beats doing
 * nothing quietly — but only for names that are not options at all. */
static bool arg_is_known(const char *a)
{
    static const char *KNOWN[] = {
        "-h", "-n", "-v", "--help", "--version", "--info", "--usage",
        "--workspaces", "--adopt", "--adopt-debug", "--adopt-focus",
        "--adopt-timeout", "--adopt-wait", "--beside", "--edge", "--no-assign",
        "--backdrop", "--backdrop-debug", "--timing", "--shot", "--mouse",
        "--go", "--set", "--back", "--config", "--no-config", "--",
    };
    for (size_t i = 0; i < SDL_arraysize(KNOWN); ++i)
        if (!strcmp(a, KNOWN[i])) return true;
    return false;
}

static bool  BACKDROP;
static int   OV_IN = 0, OV_OUT = 1;   /* the launcher's pipes, or our own
                                         stdin and stdout when spawned */
static bool  SERVING;                 /* a launcher of ours is in front */
static void  serve_end(void);
static void  backdrop_reply(const char *fmt, ...);
static void  backdrop_raise_tick(void);
static pid_t SERVE_PID;
static float BACKDROP_A;              /* what is drawn now, 0..1 */
static float BACKDROP_WANT = 1.0f;    /* where it is heading      */
static bool  BACKDROP_LEAVING;
static bool  BACKDROP_LOG;            /* --backdrop-debug: narrate to stderr */
static char  RAISE_APP[64];           /* who to put back on top of us */
static int   RAISE_LEFT;
static double RAISE_AT;
static float BLUR_NOW  = 1.0f;        /* 1 = as soft as `blur` says, 0 = sharp */
static float BLUR_WANT = 1.0f;        /* a drag sharpens it, so you can aim    */
static SDL_Texture *BLUR_HALF, *BLUR_SMALL;
static int   BLUR_W, BLUR_H;

static void usage(void)
{
    printf(
"swov — window and workspace overview for sway\n"
"\n"
"usage: swov [options] [key=value ...]\n"
"  -g, --go N|NAME     switch to that workspace and exit, without a window\n"
"  -b, --back          switch to the previously used workspace and exit\n"
"      --workspaces    print the workspaces as num/name/output/flags, and exit\n"
"      --adopt PID WS  wait for the window PID opens, move it to workspace WS\n"
"                      and exit. It tells sway where that window belongs\n"
"                      before it opens, so nothing flashes onto the workspace\n"
"                      you are on; --no-assign leaves that out.\n"
"                      --beside CON_ID --edge left|right|top|bottom\n"
"                      puts it next to that window instead of at the end,\n"
"                      --adopt-debug says what it is doing\n"
"                      and exit. Runs in the background; --adopt-wait keeps it\n"
"                      in the foreground, --adopt-focus also switches there,\n"
"                      --adopt-timeout SECS gives up after that (default 20)\n"
"      --timing        print how long each part of startup took, to stderr\n"
"      --backdrop-debug  the same, narrating the conversation to stderr\n"
"      --backdrop      draw the overview behind another window, taking no\n"
"                      input of its own: it fades in, and reads commands on\n"
"                      stdin (hover FX FY, drag on|off, fade in|out, quit).\n"
"                      This is what swas puts behind its wheel\n"
"      --usage         print how long each workspace has been used, and exit\n"
"      --info          print every path and setting swov is using, and exit\n"
"  -c, --config PATH   read this config file instead of the default\n"
"  -n, --no-config     ignore the config file\n"
"      --shot PATH     render one frame to a PNG and exit (handy for tuning)\n"
"  -h, --help          this text\n"
"  -v, --version       print the version and exit\n"
"\n"
"Every config key is also a command line option, in three spellings:\n"
"      swov --ui_scale=1.2 hl=ff8800 -s ssaa=1\n"
"\n"
"default config: ${XDG_CONFIG_HOME:-~/.config}/swov/config\n"
"keys: ssaa icons icon_px shadow shadow_layers vsync ui_scale ws_px label_px\n"
"      title_px hint_px font font_bold cols rows margin gap pad win_gap\n"
"      screen_pad radius border show_empty all_outputs header_pos hints_pos\n"
"      start_selection quit_on_focus_loss quit_after_action\n"
"      colors: bg tile tile_sel tile_hover mini_bg card card_hover card_focus\n"
"              hl text subtext dim accent hltext hint urgent outline\n"
"\n"
"keys:\n"
"  enter / click       focus the window under the cursor and leave\n"
"  click on a tile     switch to that workspace\n"
"  arrows or hjkl      move the selection; it walks through tile borders\n"
"  tab / shift+tab     previous / next workspace\n"
"  ctrl+tab            one row down in the grid, with shift one row up\n"
"  w                   switch between window and whole-workspace selection\n"
"  space / right click mark or unmark the window under the cursor\n"
"  shift+space, a      mark or unmark every window of the workspace\n"

"  c                   clear all marks\n"
"  0-9                 switch to that workspace and leave\n"
"  ctrl+0-9            move marked or selected windows to that workspace;\n"
"                      with a whole workspace selected it moves everything\n"
"                      there, keeping the container layout intact\n"

"  x / delete          close marked or selected windows, enter confirms\n"
"  f                   find windows by app id, title or workspace name\n"
"  /                   filter: same search, but hides everything else\n"
"  r                   reload the tree\n"
"  esc / q             quit\n");
}

static bool create_target(void)
{
    if (TARGET) { SDL_DestroyTexture(TARGET); TARGET = NULL; }

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(WIN_HANDLE, &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;

    int ssaa = C.ssaa;
    while (ssaa > 1 && (long long)pw * ph * ssaa * ssaa > 34000000LL) ssaa--;

    if (ssaa > 1 || BACKDROP) {
        TARGET = SDL_CreateTexture(REN, SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_TARGET, pw * ssaa, ph * ssaa);
        if (TARGET) {
            SDL_SetTextureScaleMode(TARGET, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(TARGET, SDL_BLENDMODE_BLEND);
        }
    }
    SC = TARGET ? (float)ssaa : 1.0f;
    RW = TARGET ? pw * ssaa : pw;
    RH = TARGET ? ph * ssaa : ph;

    int ww = 0, wh = 0;
    SDL_GetWindowSize(WIN_HANDLE, &ww, &wh);
    MOUSE_SCALE = (ww > 0) ? (float)RW / (float)ww : 1.0f;
    return true;
}

static void goto_workspace_number(int num)
{
    for (int i = 0; i < NWS; ++i)
        if (WSS[i].num == num) { act_goto_workspace(&WSS[i]); return; }

    sway_cmd("workspace number %d", num);           /* it does not exist yet */
    if (C.track) {
        char name[16];
        snprintf(name, sizeof(name), "%d", num);
        usage_switch(name, FOCUSED_OUTPUT, num);
    }
    running = false;
}

static int digit_from_scancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (int)(sc - SDL_SCANCODE_1) + 1;
    if (sc == SDL_SCANCODE_0) return 0;
    return -1;
}

static void begin_edit(int idx);
static void end_edit(bool commit);

/* Hand the overview to a launcher without giving up being the overview.
 *
 * swas normally starts its own backdrop, so pressing `d` here would replace
 * this window with an identical one. Instead it is started with two pipes and
 * told to talk to us: the wheel appears in front, this stays behind it, and
 * when the wheel is done we are still here. */
static void serve_end(void)
{
    if (!SERVING) return;
    SERVING = false;
    if (OV_IN  > 2) close(OV_IN);
    if (OV_OUT > 2) close(OV_OUT);
    OV_IN = 0; OV_OUT = 1;
    if (SERVE_PID > 0) { waitpid(SERVE_PID, NULL, WNOHANG); SERVE_PID = 0; }

    drag_active = false;
    drop_kind = DROP_NONE;
    set_ghosts(-1, -1);
    dirty = true;
}

static void serve_launcher(const char *cmd)
{
    if (!cmd || !*cmd || SERVING) return;

    int to_swov[2], to_swas[2];
    if (pipe(to_swov) != 0) return;                 /* swas -> us */
    if (pipe(to_swas) != 0) { close(to_swov[0]); close(to_swov[1]); return; }

    pid_t p = fork();
    if (p < 0) {
        close(to_swov[0]); close(to_swov[1]);
        close(to_swas[0]); close(to_swas[1]);
        return;
    }
    if (p == 0) {
        char in[16], out[16];
        snprintf(in,  sizeof(in),  "%d", to_swas[0]);   /* swas reads this */
        snprintf(out, sizeof(out), "%d", to_swov[1]);   /* and writes this */
        close(to_swov[0]);
        close(to_swas[1]);
        setenv("SWAS_OV_IN",  in,  1);
        setenv("SWAS_OV_OUT", out, 1);
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(to_swov[1]);
    close(to_swas[0]);
    OV_IN  = to_swov[0];
    OV_OUT = to_swas[1];
    SERVE_PID = p;
    SERVING = true;
    fprintf(stderr, "swov: serving '%s' as its overview (pid %d)\n", cmd, (int)p);

    int fl = fcntl(OV_IN, F_GETFL, 0);
    if (fl != -1) fcntl(OV_IN, F_SETFL, fl | O_NONBLOCK);

    backdrop_reply("ready");
}

static void handle_key(const SDL_KeyboardEvent *k)
{
    bool shift = (k->mod & SDL_KMOD_SHIFT) != 0;
    bool was_active = sel_active;
    sel_active = true;
    SDL_Keycode key = k->key;

    if (editing) {
        switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: end_edit(true);  reload_model(); return;
        case SDLK_ESCAPE:   end_edit(false); return;
        case SDLK_BACKSPACE:
            while (edit_len > 0) {
                unsigned char c = (unsigned char)edit_buf[--edit_len];
                edit_buf[edit_len] = 0;
                if ((c & 0xc0) != 0x80) break;
            }
            return;
        default: return;
        }
    }

    if (editing) {
        switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: end_edit(true);  reload_model(); return;
        case SDLK_ESCAPE:   end_edit(false); return;
        case SDLK_BACKSPACE:
            while (edit_len > 0) {
                unsigned char c = (unsigned char)edit_buf[--edit_len];
                edit_buf[edit_len] = 0;
                if ((c & 0xc0) != 0x80) break;
            }
            return;
        default: return;
        }
    }

    if (confirm_kill) {                            /* enter confirms, anything
                                                    * else calls it off */
        confirm_kill = false;
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) act_close();
        rebuild_chrome();
        return;
    }

    if (query_active()) {
        switch (key) {
        case SDLK_ESCAPE:
            filtering = searching = false;
            query[0] = 0;
            qlen = 0;
            apply_filter();
            rebuild_chrome();
            return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            /* the same rule as outside a search: a window if one is picked,
             * otherwise the workspace itself. Tab leaves no window picked,
             * and enter has to keep working after it. */
            if (NWS == 0) { running = false; return; }
            if (ws_sel_win(&WSS[sel_ws])) act_focus_window(ws_sel_win(&WSS[sel_ws]));
            else                          act_goto_workspace(&WSS[sel_ws]);
            return;
        case SDLK_BACKSPACE:
            while (qlen > 0) {                     /* drop one code point */
                unsigned char c = (unsigned char)query[--qlen];
                query[qlen] = 0;
                if ((c & 0xc0) != 0x80) break;
            }
            apply_filter();
            rebuild_chrome();
            return;
        case SDLK_TAB:
            if (k->mod & SDL_KMOD_CTRL) step_ws_row(shift ? -1 : 1);
            else                        step_ws(shift ? -1 : 1);
            return;
        case SDLK_LEFT:  navigate(-1, 0); return;
        case SDLK_RIGHT: navigate( 1, 0); return;
        case SDLK_UP:    navigate(0, -1); return;
        case SDLK_DOWN:  navigate(0,  1); return;
        default: return;
        }
    }

    /* A digit only counts when it is pressed bare, or with ctrl. With shift
     * held the layout may well be producing something else entirely — on a
     * German keyboard shift+7 is "/" — and that has to reach text input. */
    int digit = digit_from_scancode(k->scancode);
    if (digit >= 0 && !shift && !(k->mod & SDL_KMOD_ALT)) {
        if (k->mod & SDL_KMOD_CTRL) {
            if (was_active) act_move_to_workspace(digit);   /* ctrl: take it there */
        } else {
            goto_workspace_number(digit);                   /* bare: go there */
        }
        return;
    }
    if (digit >= 0) return;                    /* shifted digit: let text input have it */

    switch (key) {
    case SDLK_ESCAPE:
        if (drag_active) { drag_cancel(); break; }
        running = false;
        break;
    case SDLK_Q:
        running = false;
        break;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (NWS == 0) { running = false; break; }
        if (ws_sel_win(&WSS[sel_ws])) act_focus_window(ws_sel_win(&WSS[sel_ws]));
        else                          act_goto_workspace(&WSS[sel_ws]);
        break;

    case SDLK_TAB:
        if (k->mod & SDL_KMOD_CTRL)      step_ws_row(shift ? -1 : 1);
        else if (!strcmp(C.tab, "recent") && step_recent(shift ? -1 : 1)) break;
        else                             step_ws(shift ? -1 : 1);
        break;

    case SDLK_D:                                   /* the launcher, in front */
        if (C.launcher[0] && !SERVING) serve_launcher(C.launcher);
        break;

    case SDLK_W:                                   /* window level <-> workspace */
        if (NWS > 0) {
            Ws *ws = &WSS[sel_ws];
            if (ws->sel >= 0) ws->sel = -1;
            else              ws->sel = ws_first_visible(ws);
        }
        break;

    case SDLK_LEFT:  case SDLK_H: navigate(-1, 0); break;
    case SDLK_RIGHT: case SDLK_L: navigate( 1, 0); break;
    case SDLK_UP:    case SDLK_K: navigate(0, -1); break;
    case SDLK_DOWN:  case SDLK_J: navigate(0,  1); break;

    case SDLK_SPACE:
        if (NWS > 0) {
            Ws *ws = &WSS[sel_ws];
            if (!shift && ws->sel >= 0) act_toggle_mark(ws_sel_win(ws));
            else                        toggle_ws_marks(ws);
        }
        break;

    case SDLK_A:
        if (NWS > 0) toggle_ws_marks(&WSS[sel_ws]);
        break;

    case SDLK_C:
        for (int i = 0; i < NWIN; ++i) WINS[i].marked = false;
        break;

    case SDLK_X:
    case SDLK_DELETE:
        if (NWS > 0) {                             /* ask before killing */
            int ids[MAX_WINDOWS];
            if (gather_targets(ids, MAX_WINDOWS) > 0 ||
                (WSS[sel_ws].sel < 0 && WSS[sel_ws].count > 0)) {
                confirm_kill = true;
                rebuild_chrome();
            }
        }
        break;

    case SDLK_R:
        reload_model();
        break;

    case SDLK_SLASH:
        filtering = true;
        searching = false;
        query[0] = 0;
        qlen = 0;
        swallow_next_text = true;
        rebuild_chrome();
        break;

    case SDLK_F:                                   /* find, without hiding */
        searching = true;
        filtering = false;
        query[0] = 0;
        qlen = 0;
        swallow_next_text = true;
        rebuild_chrome();
        break;

    default:
        break;
    }
}

static void begin_edit(int idx)
{
    editing = true;
    edit_ws = idx;
    str_set(edit_buf, sizeof(edit_buf), WSS[idx].label);
    edit_len = (int)strlen(edit_buf);
    swallow_next_text = false;
}

static void end_edit(bool commit)
{
    if (commit && edit_ws >= 0 && edit_ws < NWS) {
        Ws *ws = &WSS[edit_ws];
        char *to = edit_buf[0] ? (ws->num >= 0 ? fmt_alloc("%d:%s", ws->num, edit_buf)
                                               : xstrdup(edit_buf))
                               : (ws->num >= 0 ? fmt_alloc("%d", ws->num)
                                               : xstrdup(ws->name));
        if (strcmp(to, ws->name) != 0) {
            ws_rename(ws, to);
            if (C.track) {                         /* keep its history */
                usage_load();
                Usage *u = usage_find(ws->name, false);
                if (u) str_set(u->name, sizeof(u->name), to);
                if (strcmp(USAGE_CUR, ws->name) == 0) str_set(USAGE_CUR, sizeof(USAGE_CUR), to);
                usage_save();
            }
        }
        free(to);
    }
    editing = false;
    edit_ws = -1;
    edit_buf[0] = 0;
    edit_len = 0;
}

static void handle_mouse_press(const SDL_MouseButtonEvent *b)
{
    float mx = b->x * MOUSE_SCALE, my = b->y * MOUSE_SCALE;
    int ws_idx = -1;
    int win_idx = hit_test(mx, my, &ws_idx);

    if (editing) end_edit(true);                   /* a click elsewhere commits */

    /* the map of the monitors: step onto one and look at its workspaces */
    if (b->button == SDL_BUTTON_LEFT && MAP_RECT.w > 0.0f) {
        for (int i = 0; i < NOUTS; ++i) {
            SDL_FRect r = OUTS[i].box;
            if (r.w <= 0.0f || mx < r.x || mx >= r.x + r.w ||
                my < r.y || my >= r.y + r.h) continue;
            if (strcmp(OUTS[i].name, FOCUSED_OUTPUT) == 0) {
                PINNED_OUTPUT = false;             /* back to following sway */
                str_set(FOCUSED_OUTPUT, sizeof(FOCUSED_OUTPUT), HOME_OUTPUT);
            } else {
                PINNED_OUTPUT = strcmp(OUTS[i].name, HOME_OUTPUT) != 0;
                str_set(FOCUSED_OUTPUT, sizeof(FOCUSED_OUTPUT), OUTS[i].name);
            }
            reload_model();
            dirty = true;
            return;
        }
    }

    if (b->button == SDL_BUTTON_LEFT && ws_idx >= 0 && win_idx < 0) {
        SDL_FRect tb = WSS[ws_idx].title_hit;
        if (mx >= tb.x && mx < tb.x + tb.w && my >= tb.y && my < tb.y + tb.h) {
            sel_active = true;
            sel_ws = ws_idx;
            WSS[ws_idx].sel = -1;
            begin_edit(ws_idx);
            return;
        }
    }

    if (ws_idx < 0) {                       /* the empty background */
        if (b->button == SDL_BUTTON_LEFT) running = false;
        return;
    }

    sel_active = true;
    sel_ws = ws_idx;
    WSS[ws_idx].sel = (win_idx >= 0) ? win_idx - WSS[ws_idx].first : -1;

    switch (b->button) {
    case SDL_BUTTON_LEFT:                   /* arm a drag, act on release */
        press_down = true;
        press_x = mx;
        press_y = my;
        press_win = win_idx;
        press_ws  = ws_idx;
        break;
    case SDL_BUTTON_RIGHT:
        if (win_idx >= 0) act_toggle_mark(&WINS[win_idx]);
        break;
    case SDL_BUTTON_MIDDLE:
        if (win_idx >= 0) {
            sway_cmd("[con_id=%d] kill", WINS[win_idx].con_id);
            reload_model();
        }
        break;
    default: break;
    }
}

static void handle_mouse_release(const SDL_MouseButtonEvent *b)
{
    if (b->button != SDL_BUTTON_LEFT) return;

    float mx = b->x * MOUSE_SCALE, my = b->y * MOUSE_SCALE;

    if (drag_active) {
        drag_update_target(mx, my);
        drag_finish();
        press_down = false;
        return;
    }
    if (!press_down) return;
    press_down = false;

    /* a plain click: only acts when press and release landed on the same
     * window, or on the same workspace */
    int ws_idx = -1;
    int win_idx = hit_test(mx, my, &ws_idx);
    if (ws_idx < 0 || ws_idx != press_ws) { press_win = press_ws = -1; return; }

    if (win_idx >= 0 && win_idx == press_win) act_focus_window(&WINS[win_idx]);
    else if (win_idx < 0 && press_win < 0)    act_goto_workspace(&WSS[ws_idx]);

    press_win = press_ws = -1;
}

static void handle_mouse_motion(const SDL_MouseMotionEvent *mo)
{
    float mx = mo->x * MOUSE_SCALE, my = mo->y * MOUSE_SCALE;
    drag_x = mx;
    drag_y = my;

    if (press_down && !drag_active) {
        float dx = mx - press_x, dy = my - press_y;
        if (dx * dx + dy * dy > (7.0f * SC) * (7.0f * SC)) {
            drag_active = true;
            hov_ws = hov_win = -1;               /* hover is meaningless now */
            drag_src_tile = (SDL_FRect){ 0, 0, 0, 0 };
            if (press_win >= 0 && WINS[press_win].ws >= 0 && WINS[press_win].ws < NWS)
                drag_src_tile = WSS[WINS[press_win].ws].tile;
            if (press_win < 0 && press_ws >= 0 && WSS[press_ws].num >= 0)
                drag_ws_mode = true;         /* dragging the whole workspace */
            update_ghosts(mx, my);
            SDL_SetCursor(CUR_HAND);
        }
    }

    if (drag_active) {
        int dws = -1;
        lift_update(mx, my, dws, hit_test(mx, my, &dws));
        drag_update_target(mx, my);
        dirty = true;
        return;
    }

    int ws_idx = -1;
    int win_idx = hit_test(mx, my, &ws_idx);
    lift_update(mx, my, ws_idx, win_idx);
    if (ws_idx == hov_ws && win_idx == hov_win) return;

    hov_ws = ws_idx;
    hov_win = win_idx;
    SDL_SetCursor(ws_idx >= 0 ? CUR_HAND : CUR_ARROW);
    dirty = true;

    /* Pointing at something selects it, so space, x, ctrl+digit and the rest
     * act on whatever is under the pointer, exactly as they do on whatever
     * the arrow keys picked. */
    if (ws_idx >= 0) {
        sel_active = true;
        sel_ws = ws_idx;
        WSS[ws_idx].sel = (win_idx >= 0) ? win_idx - WSS[ws_idx].first : -1;
    }
}

static void handle_event(const SDL_Event *e)
{
    /* Mouse motion fires constantly; it only earns a redraw when it changes
     * what is under the cursor. Everything else is a real state change. */
    if (e->type != SDL_EVENT_MOUSE_MOTION) dirty = true;

    if (BACKDROP) {
        /* the window in front owns the pointer and the keyboard; all we do is
         * follow the size of the screen and the commands on stdin */
        switch (e->type) {
        case SDL_EVENT_QUIT:
            running = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
            if (create_target()) { layout(); rebuild_chrome(); }
            break;
        default:
            break;
        }
        return;
    }

    switch (e->type) {
    case SDL_EVENT_QUIT:
        running = false;
        break;

    case SDL_EVENT_KEY_DOWN:
        handle_key(&e->key);
        break;

    case SDL_EVENT_TEXT_INPUT:
        /* the key that opened a mode sends its own character right after */
        if (swallow_next_text) { swallow_next_text = false; break; }

        if (editing) {                             /* renaming a workspace */
            size_t add = strlen(e->text.text);
            if (edit_len + (int)add < (int)sizeof(edit_buf) - 1) {
                memcpy(edit_buf + edit_len, e->text.text, add + 1);
                edit_len += (int)add;
            }
            break;
        }

        if (!query_active()) {                     /* "/" opens the filter */
            if (strcmp(e->text.text, "/") == 0) {
                filtering = true;
                query[0] = 0;
                qlen = 0;
                rebuild_chrome();
            }
            break;
        }

        {
            size_t add = strlen(e->text.text);
            if (qlen + (int)add < (int)sizeof(query) - 1) {
                memcpy(query + qlen, e->text.text, add + 1);
                qlen += (int)add;
                apply_filter();
                rebuild_chrome();
            }
        }
        break;

    case SDL_EVENT_MOUSE_MOTION:
        handle_mouse_motion(&e->motion);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        handle_mouse_press(&e->button);
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        handle_mouse_release(&e->button);
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        if (e->wheel.y != 0.0f) step_ws(e->wheel.y > 0.0f ? -1 : 1);
        break;

    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        if (create_target()) { layout(); rebuild_chrome(); }
        break;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* Losing the keyboard normally means the overview's moment has
         * passed. Not while we are the backdrop for a launcher we started:
         * that launcher taking the focus is the whole point, and quitting
         * here is what made the overview vanish the instant swas appeared. */
        if (C.quit_on_focus_loss && !SERVING && !BACKDROP) running = false;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------- backdrop commands
 * One line in, one line out, on stdin and stdout. Coordinates are fractions
 * of the window, so the program in front never has to know how big we are or
 * which monitor we picked.
 *
 *   drag on|off      make the free numbers appear as tiles, or put them away
 *   hover FX FY      -> "target N", "target NAME" or "target none"
 *   fade in|out      blend the overview in or out
 *   quit             fade out and leave
 *   (end of input)   the same as quit: the parent is gone
 */
static void backdrop_reply(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    line[n] = '\n';
    line[n + 1] = 0;
    ssize_t w = write(OV_OUT, line, (size_t)n + 1);
    (void)w;
}

static void backdrop_hover(float fx, float fy)
{
    float x = fx * (float)RW, y = fy * (float)RH;

    int si = slot_at(x, y);
    if (si >= 0 && SLOTS[si].ws < 0) {            /* a free number */

        drop_kind  = DROP_WIN_NEWWS;              /* the ghost lights up */
        drop_num   = SLOTS[si].num;
        drag_active = true;
        dirty = true;
        backdrop_reply("target %d", SLOTS[si].num);
        return;
    }

    if (point_in_cancel(x, y)) {              /* let go here and nothing happens */
        drag_active = true;
        drop_kind = DROP_CANCEL;
        MAP_HOVER = -1;
        dirty = true;
        backdrop_reply("target none");
        return;
    }

    /* over the map of the monitors: hold still and we step onto that one */
    if (MAP_RECT.w > 0.0f) {
        bool on_map = map_dwell_hover(x, y);
        if (on_map) {

            drag_active = false;
            drop_kind = DROP_NONE;
            backdrop_reply("target none");
            return;
        }
    }

    int ws_idx = -1;
    int win_idx = hit_test(x, y, &ws_idx);
    drag_active = false;
    drop_kind = DROP_NONE;

    if (ws_idx < 0) {

        dirty = true;
        backdrop_reply("target none");
        return;
    }

    sel_active = true;                            /* the selection cursor */
    sel_ws = ws_idx;
    WSS[ws_idx].sel = -1;
    dirty = true;

    /* Where inside the tile decides what happens, exactly as dragging a
     * window inside swov does. Against an edge: beside everything on the
     * workspace. Over a window: beside that one. The bar shows which. */
    static const char *EDGE_NAME[] = { "left", "right", "top", "bottom" };
    char place[80] = "";

    SDL_FRect t = WSS[ws_idx].screen;
    float bx = SDL_clamp(t.w * 0.09f, 6.0f * SC, t.w * 0.18f);
    float by = SDL_clamp(t.h * 0.09f, 6.0f * SC, t.h * 0.18f);
    int edge = -1;
    if      (x - t.x < bx)       edge = EDGE_LEFT;
    else if (t.x + t.w - x < bx) edge = EDGE_RIGHT;
    else if (y - t.y < by)       edge = EDGE_TOP;
    else if (t.y + t.h - y < by) edge = EDGE_BOTTOM;

    if (edge >= 0 && WSS[ws_idx].count > 0) {
        bool pop = false;
        int  ref = ws_edge_ref(&WSS[ws_idx], edge, &pop);
        drag_active = true;
        drop_kind   = DROP_WS_EDGE;
        drop_ws     = ws_idx;
        drop_edge   = edge;
        if (ref >= 0 && !pop)
            snprintf(place, sizeof(place), " beside %d %s",
                     WINS[ref].con_id, EDGE_NAME[edge]);
        else if (ref >= 0)
            snprintf(place, sizeof(place), " outer %s", EDGE_NAME[edge]);
    } else if (win_idx >= 0) {
        int e = edge_at(WINS[win_idx].card, x, y);
        drag_active = true;
        drop_kind   = DROP_WIN_NEAR;
        drop_win    = win_idx;
        drop_ws     = ws_idx;
        drop_edge   = e;
        snprintf(place, sizeof(place), " beside %d %s",
                 WINS[win_idx].con_id, EDGE_NAME[e]);
    }

    const char *here = WSS[ws_idx].focused ? " current" : "";
    if (WSS[ws_idx].num >= 0)
        backdrop_reply("target %d%s%s", WSS[ws_idx].num, here, place);
    else
        backdrop_reply("target %s%s%s", WSS[ws_idx].name, here, place);
}

/* returns false when the channel is done with */
static bool backdrop_command(char *line)
{
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (BACKDROP_LOG) fprintf(stderr, "swov: <- %s\n", line);

    if (!strncmp(line, "hover ", 6)) {
        float fx = 0.0f, fy = 0.0f;
        if (sscanf(line + 6, "%f %f", &fx, &fy) == 2) backdrop_hover(fx, fy);
        return true;
    }
    if (!strncmp(line, "assign ", 7)) {
        int pid = 0; char ws[64] = "";
        if (sscanf(line + 7, "%d %63s", &pid, ws) == 2 && pid > 0 && ws[0])
            assign_pid_to_ws(pid, ws);
        return true;
    }
    if (!strcmp(line, "drag on")) {
        if (C.drop_outputs) show_all_outputs(true);
        /* The free numbers become ghost tiles to drop on, exactly as they do
         * when a window is dragged inside swov — dropping an app on a
         * workspace that does not exist yet has to work the same either way.
         * They do cost the real tiles some size; drop_ghosts=0 keeps them
         * out of a drag that comes from swas. */
        if (C.drop_ghosts) set_ghosts(0, 10);
        BLUR_WANT = 0.0f;
        return true;
    }
    if (!strcmp(line, "drag off")) {
        MAP_HOVER = -1;
        show_all_outputs(false);
        set_ghosts(-1, -1);
        BLUR_WANT = 1.0f;
        drag_active = false;
        drop_kind = DROP_NONE;
        sel_active = false;
        dirty = true;
        return true;
    }
    if (!strncmp(line, "raise ", 6) && line[6]) {
        str_set(RAISE_APP, sizeof(RAISE_APP), line + 6);
        RAISE_LEFT = 3;                       /* now, and twice more */
        RAISE_AT   = 0.0;
        /* Our window mapped on top of the one that asked for us, and on
         * Wayland that one cannot lift itself back. sway can.
         *
         * Focusing it directly is not enough: it usually still *is* the
         * focused window, thanks to `no_focus`, so sway has nothing to do and
         * the stacking never changes. Focusing us first and it second makes
         * the second a real change, which restacks. That is also why this
         * waits for our own window to exist in the tree — asking before sway
         * knows about it is the other half of the race. */
        return true;
    }
    if (!strcmp(line, "fade in"))  { BACKDROP_WANT = 1.0f; return true; }
    if (!strcmp(line, "fade out")) { BACKDROP_WANT = 0.0f; return true; }
    if (!strcmp(line, "quit"))     return false;
    return true;
}

/* read whatever is waiting, one line at a time */
static void backdrop_poll(void)
{
    static char buf[512];
    static int  len;

    struct pollfd p = { OV_IN, POLLIN, 0 };
    while (poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) {
        ssize_t n = read(OV_IN, buf + len, sizeof(buf) - 1 - (size_t)len);
        if (n <= 0) {
            /* Serving a launcher we started ourselves: it has finished, so
             * go back to being an overview rather than leaving with it. */
            if (SERVING) { serve_end(); len = 0; return; }
            BACKDROP_LEAVING = true;
            return;
        }
        len += (int)n;
        buf[len] = 0;

        char *start = buf, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = 0;
            if (!backdrop_command(start)) { BACKDROP_LEAVING = true; return; }
            start = nl + 1;
        }
        len = (int)strlen(start);
        memmove(buf, start, (size_t)len + 1);
    }
}

/* Put the window that spawned us back on top. Ours mapped over it and on
 * Wayland it cannot lift itself; sway's `focus` on a floating container always
 * raises it, so one command does the job — but only once sway knows our window
 * exists. Rather than guess when that is, say it three times over half a
 * second. Repeating is harmless: the wheel is already the focused window. */
static void backdrop_raise_tick(void)
{
    if (RAISE_LEFT <= 0) return;
    double now = now_secs();
    if (now < RAISE_AT) return;

    char *e = escape_arg(RAISE_APP);
    bool ok = sway_cmd("[app_id=\"%s\"] focus", e);
    free(e);

    if (BACKDROP_LOG)
        fprintf(stderr, "swov: raise [app_id=\"%s\"] focus -> %s\n",
                RAISE_APP, ok ? "ok" : "no match");

    RAISE_AT = now + 0.18;
    RAISE_LEFT--;
}

/* ease towards the wanted alpha; leaving means fade out, then exit */
/* the hold, checked every frame: the pointer may have stopped moving, so no
 * further hover ever arrives */
static void map_dwell_tick(void)
{
    if (MAP_HOVER < 0) return;
    dirty = true;                                  /* the fill is growing */

    double want = C.map_dwell_ms / 1000.0;
    if (want <= 0.0) want = 0.001;
    if (now_secs() - MAP_HOVER_SINCE < want) return;

    str_set(FOCUSED_OUTPUT, sizeof(FOCUSED_OUTPUT), OUTS[MAP_HOVER].name);
    PINNED_OUTPUT = strcmp(FOCUSED_OUTPUT, HOME_OUTPUT) != 0;
    MAP_HOVER = -1;

    /* A window being dragged from here is about to vanish from the model —
     * it lives on the screen we are leaving. Keep hold of its id so it can
     * still be dropped on the screen we are arriving at. */
    if (press_win >= 0) DRAG_CON = WINS[press_win].con_id;

    drop_kind = DROP_NONE;
    drop_ws = drop_win = -1;
    press_win = press_ws = -1;
    sel_active = false;
    reload_model();

    if (DRAG_CON > 0) {                            /* find it again, or keep
                                                      dragging by id alone */
        for (int i = 0; i < NWIN; ++i)
            if (WINS[i].con_id == DRAG_CON) { press_win = i; break; }
    }

    /* The drag is still in the air — a screen changed under it, nothing was
     * let go of. Say so, or a launcher waiting on the other end of the pipe
     * acts on the target it had a moment ago. */
    if (BACKDROP || SERVING) {
        drag_active = true;
        set_ghosts(C.drop_ghosts ? 0 : -1, C.drop_ghosts ? 10 : -1);
        backdrop_reply("target none");

        /* The app on the pointer is drawn by the launcher, on its own
         * surface. Redrawing everything here is enough for sway to put this
         * window in front of it, and then the icon is behind the overview
         * for the rest of the drag. Ask for it back. */
        if (RAISE_APP[0]) { RAISE_LEFT = 3; RAISE_AT = 0.0; }
    }
    dirty = true;
}

static void backdrop_step(float ms)
{
    backdrop_raise_tick();
    map_dwell_tick();

    if (BACKDROP_LEAVING) BACKDROP_WANT = 0.0f;

    float rate = C.anim_ms > 1.0f ? ms / C.anim_ms : 1.0f;
    if (BACKDROP_A < BACKDROP_WANT) {
        BACKDROP_A += rate;
        if (BACKDROP_A > BACKDROP_WANT) BACKDROP_A = BACKDROP_WANT;
        dirty = true;
    } else if (BACKDROP_A > BACKDROP_WANT) {
        BACKDROP_A -= rate;
        if (BACKDROP_A < BACKDROP_WANT) BACKDROP_A = BACKDROP_WANT;
        dirty = true;
    }
    if (BLUR_NOW != BLUR_WANT) {                  /* sharpen / soften again */
        float step = rate * 1.6f;
        if (BLUR_NOW < BLUR_WANT) { BLUR_NOW += step; if (BLUR_NOW > BLUR_WANT) BLUR_NOW = BLUR_WANT; }
        else                      { BLUR_NOW -= step; if (BLUR_NOW < BLUR_WANT) BLUR_NOW = BLUR_WANT; }
        dirty = true;
    }
    if (BACKDROP_LEAVING && BACKDROP_A <= 0.0f) running = false;
}

/* Two scaled copies of the frame we already rendered: full -> half -> small,
 * then that small one stretched back over the screen. The card does the
 * filtering, so it is three quad blits and no per-pixel work of our own. The
 * sharp frame is then drawn over it with whatever alpha is left, which is how
 * it sharpens while you drag. */
static void backdrop_blur(float alpha)
{
    static const int FACTOR[4] = { 0, 5, 9, 15 };
    int f  = FACTOR[C.blur < 0 ? 0 : (C.blur > 3 ? 3 : C.blur)];
    int w1 = SDL_max(2, RW / 2), h1 = SDL_max(2, RH / 2);
    int w2 = SDL_max(2, RW / f), h2 = SDL_max(2, RH / f);

    if (!BLUR_HALF || BLUR_W != w2 || BLUR_H != h2) {
        if (BLUR_HALF)  SDL_DestroyTexture(BLUR_HALF);
        if (BLUR_SMALL) SDL_DestroyTexture(BLUR_SMALL);
        BLUR_HALF = SDL_CreateTexture(REN, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_TARGET, w1, h1);
        BLUR_SMALL = SDL_CreateTexture(REN, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_TARGET, w2, h2);
        if (!BLUR_HALF || !BLUR_SMALL) { BLUR_W = BLUR_H = 0; return; }
        SDL_SetTextureScaleMode(BLUR_HALF,  SDL_SCALEMODE_LINEAR);
        SDL_SetTextureScaleMode(BLUR_SMALL, SDL_SCALEMODE_LINEAR);
        BLUR_W = w2;
        BLUR_H = h2;
    }

    /* copy, do not blend: the alpha of the overlay has to survive the trip */
    SDL_SetTextureBlendMode(TARGET, SDL_BLENDMODE_NONE);
    SDL_SetTextureBlendMode(BLUR_HALF, SDL_BLENDMODE_NONE);

    SDL_SetRenderTarget(REN, BLUR_HALF);
    SDL_RenderTexture(REN, TARGET, NULL, NULL);
    SDL_SetRenderTarget(REN, BLUR_SMALL);
    SDL_RenderTexture(REN, BLUR_HALF, NULL, NULL);
    SDL_SetRenderTarget(REN, NULL);

    SDL_SetTextureBlendMode(TARGET, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(BLUR_SMALL, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaModFloat(BLUR_SMALL, alpha);
    SDL_RenderTexture(REN, BLUR_SMALL, NULL, NULL);
}

static void present(void)
{
    if (TARGET) SDL_SetRenderTarget(REN, TARGET);
    render();
    if (TARGET) {
        SDL_SetRenderTarget(REN, NULL);
        SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColorFloat(REN, 0, 0, 0, 0);
        SDL_RenderClear(REN);
        SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);

        float sharp = 1.0f;
        if (BACKDROP && C.blur > 0 && BLUR_NOW > 0.0f) {
            backdrop_blur(BACKDROP_A);
            sharp = 1.0f - BLUR_NOW;
        }
        if (BACKDROP) SDL_SetTextureAlphaModFloat(TARGET, BACKDROP_A * sharp);
        if (sharp > 0.0f) SDL_RenderTexture(REN, TARGET, NULL, NULL);
    }

    if (SHOT_PATH) {
        SDL_Surface *shot = SDL_RenderReadPixels(REN, NULL);
        if (shot) {
            if (!IMG_SavePNG(shot, SHOT_PATH))
                fprintf(stderr, "swov: cannot write %s: %s\n", SHOT_PATH, SDL_GetError());
            SDL_DestroySurface(shot);
        }
        running = false;
    }
    SDL_RenderPresent(REN);
}

/* Place the overlay on the output sway is using. Matching by name only works
 * when SDL reports the connector ("DP-1"); on wayland it often reports the
 * monitor model instead, which is why the geometry is the better key. */
static void pick_bounds(SDL_Rect *out)
{
    *out = (SDL_Rect){ 0, 0, 1280, 720 };

    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    if (!ids || count <= 0) { SDL_free(ids); return; }

    SDL_DisplayID chosen = 0;

    /* what sway says the focused output covers */
    int ox = 0, oy = 0, ow = 0, oh = 0;
    JV *outs = sway_query(IPC_GET_OUTPUTS);
    if (outs && outs->type == J_ARR) {
        for (int i = 0; i < outs->count; ++i) {
            const JV *o = outs->items[i];
            bool is_it = FOCUSED_OUTPUT[0]
                           ? strcmp(jstr(o, "name", ""), FOCUSED_OUTPUT) == 0
                           : jbool(o, "focused", false);
            if (!is_it) continue;
            const JV *r = jget(o, "rect");
            ox = jint(r, "x", 0);
            oy = jint(r, "y", 0);
            ow = jint(r, "width", 0);
            oh = jint(r, "height", 0);
            break;
        }
    }
    jfree(outs);

    if (ow > 0 && oh > 0) {                        /* the display holding it */
        float cx = (float)ox + (float)ow * 0.5f;
        float cy = (float)oy + (float)oh * 0.5f;
        for (int i = 0; i < count; ++i) {
            SDL_Rect b;
            if (!SDL_GetDisplayBounds(ids[i], &b)) continue;
            if (cx >= (float)b.x && cx < (float)(b.x + b.w) &&
                cy >= (float)b.y && cy < (float)(b.y + b.h)) { chosen = ids[i]; break; }
        }
    }

    if (!chosen && FOCUSED_OUTPUT[0]) {
        for (int i = 0; i < count; ++i) {
            const char *n = SDL_GetDisplayName(ids[i]);
            if (n && strcmp(n, FOCUSED_OUTPUT) == 0) { chosen = ids[i]; break; }
        }
    }
    if (!chosen) {
        float mx = 0.0f, my = 0.0f;
        SDL_GetGlobalMouseState(&mx, &my);
        chosen = ids[0];
        for (int i = 0; i < count; ++i) {
            SDL_Rect b;
            if (!SDL_GetDisplayBounds(ids[i], &b)) continue;
            if (mx >= (float)b.x && mx < (float)(b.x + b.w) &&
                my >= (float)b.y && my < (float)(b.y + b.h)) { chosen = ids[i]; break; }
        }
    }
    SDL_GetDisplayBounds(chosen, out);
    SDL_free(ids);
}

int main(int argc, char **argv)
{
    C = cfg_defaults();

    char *cfg_path = NULL;
    bool  use_cfg = true;
    const char *go_name = NULL;
    bool  go_back = false;
    bool  show_usage_stats = false;
    bool  show_info = false;
    bool  list_ws = false;
    bool  adopt_focus = false, adopt_wait = false;
    int   adopt_pid = -1;
    const char *adopt_ws = NULL;
    double adopt_timeout = 20.0;
    int   adopt_beside = -1;
    bool  adopt_no_assign = false;
    const char *adopt_edge = NULL;
    const char *sets[64];
    int nsets = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("swov %s (build %s)\n", SWOV_VERSION, SWOV_BUILD);
            return 0;
        }
        else if ((!strcmp(a, "-g") || !strcmp(a, "--go")) && i + 1 < argc) go_name = argv[++i];
        else if (!strcmp(a, "-b") || !strcmp(a, "--back")) go_back = true;
        else if (!strcmp(a, "--workspaces")) list_ws = true;
        else if (!strcmp(a, "--backdrop")) BACKDROP = true;
        else if (!strcmp(a, "--backdrop-debug")) { BACKDROP = true; BACKDROP_LOG = true; }
        else if (!strcmp(a, "--adopt") && i + 2 < argc) {
            adopt_pid = atoi(argv[++i]);
            adopt_ws  = argv[++i];
        }
        else if (!strcmp(a, "--adopt-focus")) adopt_focus = true;
        else if (!strcmp(a, "--adopt-debug")) ADOPT_LOG = true;
        else if (!strcmp(a, "--timing")) TIMING = true;
        else if (a[0] == '-' && !strchr(a, '=') && !arg_is_known(a))
            fprintf(stderr, "swov: unknown option '%s' (see --help)\n", a);
        else if (!strcmp(a, "--no-assign")) adopt_no_assign = true;
        else if (!strcmp(a, "--beside") && i + 1 < argc) adopt_beside = atoi(argv[++i]);
        else if (!strcmp(a, "--edge") && i + 1 < argc)   adopt_edge = argv[++i];
        else if (!strcmp(a, "--adopt-wait"))  adopt_wait = true;
        else if (!strcmp(a, "--adopt-timeout") && i + 1 < argc)
            adopt_timeout = atof(argv[++i]);
        else if (!strcmp(a, "--usage")) show_usage_stats = true;
        else if (!strcmp(a, "--info")) show_info = true;
        else if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i + 1 < argc)
            cfg_path = expand_tilde(argv[++i]);
        else if (!strcmp(a, "-n") || !strcmp(a, "--no-config")) use_cfg = false;
        else if (!strcmp(a, "--shot") && i + 1 < argc) SHOT_PATH = argv[++i];
        else if (!strcmp(a, "--mouse") && i + 1 < argc) {
            float fx = 0.0f, fy = 0.0f;
            if (sscanf(argv[++i], "%f,%f", &fx, &fy) == 2) { SHOT_MX = fx; SHOT_MY = fy; }
        }
        else if ((!strcmp(a, "-s") || !strcmp(a, "--set")) && i + 1 < argc && nsets < 64)
            sets[nsets++] = argv[++i];
        else if (strncmp(a, "--", 2) == 0 && strchr(a, '=')) {
            if (nsets < 64) sets[nsets++] = a + 2;      /* --ui_scale=1.2 */
        }
        else if (a[0] != '-' && strchr(a, '=')) {
            if (nsets < 64) sets[nsets++] = a;          /* ui_scale=1.2 */
        }
        else { fprintf(stderr, "swov: unknown argument '%s'\n", a); usage(); return 2; }
    }

    if (use_cfg) {
        SHARED_LOADED = sw_shared_apply("swov", cfg_set_shared, &C) != 0;

        bool named = cfg_path != NULL;
        if (!cfg_path) cfg_path = default_config_path();
        if (cfg_path) {
            str_set(CFG_PATH, sizeof(CFG_PATH), cfg_path);
            CFG_LOADED = cfg_load_file(&C, cfg_path);
            if (!CFG_LOADED && named) fprintf(stderr, "swov: no config at %s\n", cfg_path);
        }
    }
    free(cfg_path);

    for (int i = 0; i < nsets; ++i) {               /* -s key=value */
        char *copy = xstrdup(sets[i]);
        char *eq = strchr(copy, '=');
        if (eq) { *eq = 0; cfg_set(&C, str_trim(copy), str_trim(eq + 1)); }
        free(copy);
    }

    if (show_info) {
        char exe[PATH_MAX] = {0};
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) exe[n] = 0;

        char *upath = usage_path();
        usage_load();

        printf("swov %s (build %s)\n\n", SWOV_VERSION, SWOV_BUILD);
        printf("binary        %s\n", exe[0] ? exe : "(unknown)");
        printf("shared config %s%s\n", sw_shared_path() ? sw_shared_path() : "(none)",
               SHARED_LOADED ? "  [loaded]" : "  [not present]");
        printf("config        %s%s\n", CFG_PATH[0] ? CFG_PATH : "(none)",
               CFG_PATH[0] ? (CFG_LOADED ? "  [loaded]" : "  [missing, defaults in use]") : "");
        printf("usage file    %s\n", upath ? upath : "(none)");
        printf("sway socket   %s\n", getenv("SWAYSOCK") ? getenv("SWAYSOCK") : "(unset)");

        char *theme = icon_theme_name();
        printf("icon theme    %s\n", theme);
        free(theme);

        fontconfig_resolve(C.font);
        printf("font          %s\n", FC_REGULAR ? FC_REGULAR : (C.font[0] ? C.font : "(none)"));
        printf("font bold     %s\n", FC_BOLD ? FC_BOLD : "(synthetic)");
        free(FC_REGULAR);
        free(FC_BOLD);
        FC_REGULAR = FC_BOLD = NULL;

        printf("\nrecorded usage\n");
        if (USAGE_CUR[0])
            printf("  on %s (%s), for %.0fs\n", USAGE_CUR,
                   USAGE_CUR_OUT[0] ? USAGE_CUR_OUT : "?", usage_pending());
        if (NUSAGE == 0) printf("  (nothing yet)\n");
        for (int i = 0; i < NUSAGE; ++i) {
            double secs = USAGE[i].secs +
                          (strcmp(USAGE[i].name, USAGE_CUR) == 0 ? usage_pending() : 0.0);
            printf("  %-16s %-10s %7.0fs\n", USAGE[i].name,
                   USAGE[i].output[0] ? USAGE[i].output : "-", secs);
        }

        printf("\nsettings\n");
        printf("  ssaa %d   icons %d   icon_px %d   ui_scale %.2f\n",
               C.ssaa, C.icons, C.icon_px, (double)C.ui_scale);
        printf("  ws_px %d   label_px %d   title_px %d   hint_px %d\n",
               C.ws_px, C.label_px, C.title_px, C.hint_px);
        printf("  cols %d   rows %d   margin %.0f   gap %.0f   pad %.0f\n",
               C.cols, C.rows, (double)C.margin, (double)C.gap, (double)C.pad);
        printf("  win_gap %.0f   screen_pad %.0f   radius %.0f   border %.0f\n",
               (double)C.win_gap, (double)C.screen_pad, (double)C.radius, (double)C.border);
        printf("  show_empty %d   all_outputs %d   start_selection %d\n",
               C.show_empty, C.all_outputs, C.start_selection);
        printf("  header_pos %s   hints_pos %s\n", C.header_pos, C.hints_pos);
        printf("  blur %d (backdrop only)   cpu %d (min %g, full %g)\n",
               C.blur, C.cpu, (double)C.cpu_min, (double)C.cpu_full);
        printf("  back %s\n",
               C.back_scope == 1 ? "output" : C.back_scope == 2 ? "sway" : "global");
        printf("  track %d   usage_dots %d   dot_count %d   dot_px %.0f\n",
               C.track, C.usage_dots, C.dot_count, (double)C.dot_px);
        printf("  anim_ms %.0f   shadow %d   float_alpha %.2f   vsync %d\n",
               (double)C.anim_ms, C.shadow, (double)C.float_alpha, C.vsync);

        printf("\ncolours\n");
        struct { const char *k; SDL_FColor c; } cols[] = {
            { "bg", C.bg }, { "tile", C.tile }, { "tile_sel", C.tile_sel },
            { "mini_bg", C.mini_bg }, { "card", C.card }, { "card_hover", C.card_hover },
            { "card_focus", C.card_focus }, { "hl", C.hl }, { "current", C.current },
            { "match", C.match }, { "text", C.text }, { "subtext", C.subtext },
            { "dim", C.dim }, { "accent", C.accent }, { "hint", C.hint },
            { "hltext", C.hltext }, { "urgent", C.urgent }, { "outline", C.outline }
        };
        for (size_t i = 0; i < SDL_arraysize(cols); ++i)
            printf("  %-12s %02x%02x%02x%02x\n", cols[i].k,
                   (unsigned)(cols[i].c.r * 255.0f + 0.5f), (unsigned)(cols[i].c.g * 255.0f + 0.5f),
                   (unsigned)(cols[i].c.b * 255.0f + 0.5f), (unsigned)(cols[i].c.a * 255.0f + 0.5f));

        free(upath);
        return 0;
    }

    if (show_usage_stats) {
        char *path = usage_path();
        usage_load();
        printf("usage file: %s\n", path ? path : "(none)");
        if (USAGE_CUR[0])
            printf("on:         %s on %s, for %.0fs\n", USAGE_CUR,
                   USAGE_CUR_OUT[0] ? USAGE_CUR_OUT : "?", usage_pending());
        else              printf("on:         nothing recorded yet\n");
        for (int i = 0; i < NUSAGE; ++i) {
            double secs = USAGE[i].secs +
                          (strcmp(USAGE[i].name, USAGE_CUR) == 0 ? usage_pending() : 0.0);
            int mins = (int)(secs / 60.0);
            double ago = USAGE[i].last > 0.0 ? now_secs() - USAGE[i].last : -1.0;
            printf("  %-16s %-10s %6.0fs  (%dh %02dm)", USAGE[i].name,
                   USAGE[i].output[0] ? USAGE[i].output : "-", secs, mins / 60, mins % 60);
            if (ago >= 0.0) printf("   last seen %.0fs ago", ago);
            printf("\n");
        }
        free(path);
        return 0;
    }

    if (adopt_pid > 0 && !adopt_wait) {
        /* Step into the background before touching the socket: whoever called
         * us wants to carry on, and the window may be seconds away. */
        pid_t p = fork();
        if (p < 0) die("fork: %s", strerror(errno));
        if (p > 0) return 0;
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0); dup2(devnull, 1);
            if (devnull > 2) close(devnull);
        }
    }

    if (TIMING) T0 = now_secs();
    sway_fd = sway_connect();
    if (sway_fd < 0) die("cannot reach sway (is SWAYSOCK set?)");
    mark("sway socket");

    if (list_ws) {
        int rc = print_workspaces();
        close(sway_fd);
        return rc;
    }

    if (adopt_pid > 0 && adopt_ws) {
        int rc = adopt_window(adopt_pid, adopt_ws, adopt_timeout, adopt_focus,
                              adopt_beside, adopt_edge, adopt_no_assign);
        close(sway_fd);
        return rc;
    }

    /* Switching does not need a window, a renderer or a font: connect, say it,
     * leave. This is the path a keybinding should use. */
    if (go_back || go_name) {
        usage_sync();               /* the user may have moved without us */
        bool ok;
        if (go_back) {
            /* the last workspace we used on this monitor, not sway's global
             * back_and_forth, which would jump to the other screen */
            char here[64] = {0}, out[64] = {0}, target[64] = {0};
            int  here_num = -1, target_num = -1;
            focused_workspace(here, sizeof(here), out, sizeof(out), &here_num);

            if (C.track && C.back_scope != 2 &&
                last_workspace_here(C.back_scope == 1 ? out : NULL, here,
                                    target, sizeof(target), &target_num)) {
                /* By number where there is one: the workspace may have been
                 * renamed since we saw it, and asking sway for a name that no
                 * longer exists makes it create a second workspace with the
                 * same number. */
                if (target_num >= 0) {
                    ok = sway_cmd("workspace number %d", target_num);
                } else {
                    char *e = escape_arg(target);
                    ok = sway_cmd("workspace --no-auto-back-and-forth \"%s\"", e);
                    free(e);
                }
            } else {
                ok = sway_cmd("workspace back_and_forth");
            }
        } else if (str_all_digits(go_name)) {
            ok = sway_cmd("workspace number %d", atoi(go_name));
        } else {
            char *e = escape_arg(go_name);         /* a name, "code" or "3:code" */
            ok = sway_cmd("workspace --no-auto-back-and-forth \"%s\"", e);
            free(e);
        }
        if (ok && C.track) {                       /* stamp the new workspace */
            char now[64], nowout[64];
            int  nownum = -1;
            focused_workspace(now, sizeof(now), nowout, sizeof(nowout), &nownum);
            usage_switch(now, nowout, nownum);
        }
        close(sway_fd);
        return ok ? 0 : 1;
    }

    /* the focused output is needed before the window is created */
    JV *wsr = sway_query(IPC_GET_WORKSPACES);
    if (wsr && wsr->type == J_ARR)
        for (int i = 0; i < wsr->count; ++i)
            if (jbool(wsr->items[i], "focused", false))
                str_set(FOCUSED_OUTPUT, sizeof(FOCUSED_OUTPUT), jstr(wsr->items[i], "output", ""));
    jfree(wsr);

    /* A different app id, because a backdrop must not take the keyboard from
     * the window in front of it:
     *     for_window [app_id="swov-backdrop"] no_focus                     */
    SDL_SetHint(SDL_HINT_APP_ID, BACKDROP ? APP_ID "-backdrop" : APP_ID);
    SDL_SetAppMetadata(APP_ID, SWOV_VERSION, "org.swov.overview");

    if (!SDL_Init(SDL_INIT_VIDEO)) die("SDL_Init: %s", SDL_GetError());
    mark("SDL_Init");
    if (!TTF_Init()) die("TTF_Init: %s", SDL_GetError());

    SDL_Rect bounds;
    pick_bounds(&bounds);

    SDL_WindowFlags flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT |
                            SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    WIN_HANDLE = SDL_CreateWindow(BACKDROP ? APP_ID "-backdrop" : APP_ID,
                                  bounds.w, bounds.h, flags);
    if (!WIN_HANDLE) die("SDL_CreateWindow: %s", SDL_GetError());
    SDL_SetWindowPosition(WIN_HANDLE, bounds.x, bounds.y);

    REN = SDL_CreateRenderer(WIN_HANDLE, NULL);
    if (!REN) die("SDL_CreateRenderer: %s", SDL_GetError());
    mark("window+renderer");
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);
    SDL_SetRenderVSync(REN, C.vsync ? 1 : 0);

    if (!create_target()) die("cannot size the window");
    mark("render target");
    load_fonts();

    CUR_ARROW = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    CUR_HAND  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

    usage_sync();
    sway_subscribe_events();
    if (C.output[0]) {                 /* asked to look at another screen */
        str_set(FOCUSED_OUTPUT, sizeof(FOCUSED_OUTPUT), C.output);
        PINNED_OUTPUT = true;
    }
    if (!model_reload()) die("could not read the sway tree");
    if (C.output[0] && HOME_OUTPUT[0] && !strcmp(C.output, HOME_OUTPUT))
        PINNED_OUTPUT = false;
    mark("sway tree");
    cpu_load();                  /* whatever swbr last measured */
    select_current_workspace();

    sel_active = BACKDROP ? false : (C.start_selection != 0);
    if (!BACKDROP && C.start_selection == 2 && NWS > 0)
        WSS[sel_ws].sel = ws_first_visible(&WSS[sel_ws]);
    layout();
    apply_filter();
    rebuild_chrome();

    if (SHOT_MX >= 0.0f) {                          /* pretend the pointer is there */
        hov_win = hit_test(SHOT_MX * MOUSE_SCALE, SHOT_MY * MOUSE_SCALE, &hov_ws);
    }

    Uint64 last_reload = 0;
    bool   pending_reload = false;

    if (!BACKDROP) SDL_StartTextInput(WIN_HANDLE);
    SDL_RaiseWindow(WIN_HANDLE);
    if (BACKDROP && SHOT_PATH) BACKDROP_A = 1.0f;   /* a still has nothing to fade */
    present();

    if (BACKDROP) {
        /* the window is up; whoever spawned us can put itself back on top */
        printf("ready\n");
        fflush(stdout);
    }

    Uint64 last_tick = SDL_GetTicks();

    while (running) {
        SDL_Event e;
        if (anim_running()) dirty = true;
        cpu_poll();
        if (drag_active) map_dwell_tick();

        if (BACKDROP || SERVING) {
            backdrop_poll();
            if (SERVING) backdrop_raise_tick();
            Uint64 now = SDL_GetTicks();
            if (BACKDROP) backdrop_step((float)(now - last_tick));
            last_tick = now;
            if (!running) break;
        }

        if (SDL_WaitEventTimeout(&e, (BACKDROP || SERVING) ? 8
                                    : (anim_running() || pending_reload) ? 8 : 60)) {
            handle_event(&e);
            while (running && SDL_PollEvent(&e)) handle_event(&e);   /* coalesce */
        }

        /* Sway acknowledges a command before the layout transaction has
         * committed, so the tree is only trustworthy once the events for it
         * have arrived. This also keeps the overview live. A window that
         * rewrites its title in a loop would otherwise have us rebuilding the
         * model every few milliseconds, so events are coalesced. */
        if (running && !drag_active && sway_events_pending()) pending_reload = true;

        if (running && pending_reload) {
            Uint64 now = SDL_GetTicks();
            if (now - last_reload >= 120) {
                pending_reload = false;
                last_reload = now;
                reload_model();
                dirty = true;
            }
        }
        if (running && dirty) { present(); dirty = false; }
    }

    /* cleanup */
    tex_free(&T_HEADER);
    tex_free(&T_HINTS);
    tex_free(&T_QUERY);
    model_free();
    icons_free();

    if (CUR_ARROW) SDL_DestroyCursor(CUR_ARROW);
    if (CUR_HAND)  SDL_DestroyCursor(CUR_HAND);
    if (BLUR_HALF)  SDL_DestroyTexture(BLUR_HALF);
    if (BLUR_SMALL) SDL_DestroyTexture(BLUR_SMALL);
    if (TARGET)    SDL_DestroyTexture(TARGET);
    if (F_BADGE)   TTF_CloseFont(F_BADGE);
    if (F_LABEL)   TTF_CloseFont(F_LABEL);
    if (F_TITLE)   TTF_CloseFont(F_TITLE);
    if (F_HINT)    TTF_CloseFont(F_HINT);
    TTF_Quit();
    SDL_DestroyRenderer(REN);
    SDL_DestroyWindow(WIN_HANDLE);
    fullscreen_put_back();
    SDL_Quit();
    if (sway_fd >= 0) close(sway_fd);
    if (sway_evt_fd >= 0) close(sway_evt_fd);
    return 0;
}
