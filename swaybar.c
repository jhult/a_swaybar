/* swaybar - a single-process status bar for sway.
 *
 * NOTE: named swaybar on purpose, but kept at ~/.config/sway/swaybar, NOT on
 * PATH: sway spawns its own renderer as "swaybar" via PATH, so a binary of
 * this name on PATH would hijack it. Reference it by absolute path.
 *
 * Replaces a shell script that re-ran itself on a timer. Emits the i3bar JSON
 * protocol: volume, wifi, battery, cpu, clock, and one coloured square per
 * maki session, ordered by the workspace that session runs in.
 *
 * maki is the agentic TUI this machine runs all its work in (one maki process,
 * many sessions). It cannot show a status bar, and the bar must not link
 * against it, so maki-side plugin publishes one file per process and the bar
 * reads it: https://github.com/zipadoodlez/maki-session-status
 *   one square per live session, peach when working, lavender when idle or
 *   waiting for input, in workspace order.
 *
 * Volume comes from pipewire-pulse via libpulse and is read on a change
 * event, not polled: at rest pipewire-pulse does no work for us.
 *
 * Build:
 *   make
 * or:
 *   gcc -O2 -Wall -o swaybar swaybar.c $(pkg-config --cflags --libs libpulse)
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <pulse/pulseaudio.h>

/* ---- config ---------------------------------------------------------- */

#define GLYPH            "\u25a0"          /* filled square */
#define COLOR_BUSY       "#fab387"         /* peach: working */
#define COLOR_OTHER      "#b4befe"         /* lavender: idle */

#define BAT_DIR          "/sys/class/power_supply/BAT0"
#define WIFI_IFACE       "wlan0"

#define MAKI_STALE_SEC   7                 /* no heartbeat this long => gone */
#define MAKI_EXT         ".status"

#define SWAY_GET_TREE    4                 /* i3 ipc message type */

/* ---- tiny helpers ---------------------------------------------------- */

static int read_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)n, f)) { fclose(f); return 0; }
    fclose(f);
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    return 1;
}

/* ---- cpu (/proc/stat delta) ------------------------------------------ */

static long long cpu_prev_total, cpu_prev_idle;

static int cpu_percent(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[256];
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    fclose(f);

    unsigned long long v[8] = {0};
    int got = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (got < 5) return 0;

    long long total = 0;
    for (int i = 0; i < 8; i++) total += (long long)v[i];
    long long idle = (long long)v[3] + (long long)v[4];

    long long dt = total - cpu_prev_total;
    long long di = idle - cpu_prev_idle;
    cpu_prev_total = total;
    cpu_prev_idle = idle;
    if (dt <= 0) return 0;
    return (int)(100 * (dt - di) / dt);
}

/* ---- sway IPC: get_tree ---------------------------------------------- */

static char *sway_get_tree(size_t *out_len) {
    const char *sock = getenv("SWAYSOCK");
    if (!sock || !*sock) return NULL;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(sock) >= sizeof addr.sun_path) { close(fd); return NULL; }
    strcpy(addr.sun_path, sock);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return NULL; }

    unsigned char req[14];
    memcpy(req, "i3-ipc", 6);
    uint32_t plen = 0, type = SWAY_GET_TREE;
    memcpy(req + 6, &plen, 4);
    memcpy(req + 10, &type, 4);
    if (write(fd, req, sizeof req) != (ssize_t)sizeof req) { close(fd); return NULL; }

    unsigned char hdr[14];
    size_t got = 0;
    while (got < sizeof hdr) {
        ssize_t r = read(fd, hdr + got, sizeof hdr - got);
        if (r <= 0) { close(fd); return NULL; }
        got += (size_t)r;
    }
    uint32_t rlen;
    memcpy(&rlen, hdr + 6, 4);
    if (rlen == 0 || rlen > (64u << 20)) { close(fd); return NULL; }

    char *buf = malloc(rlen + 1);
    if (!buf) { close(fd); return NULL; }
    size_t g = 0;
    while (g < rlen) {
        ssize_t r = read(fd, buf + g, rlen - g);
        if (r <= 0) { free(buf); close(fd); return NULL; }
        g += (size_t)r;
    }
    buf[rlen] = '\0';
    close(fd);
    if (out_len) *out_len = rlen;
    return buf;
}

/* ---- hand-rolled JSON scan for the tree ------------------------------ */

#define MAX_PIDS 512
struct pidws { long pid; char ws[128]; };
static struct pidws pids[MAX_PIDS];
static int npids;

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *parse_string(const char *p, char *out, size_t max) {
    if (*p != '"') return NULL;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4 && isxdigit((unsigned char)*p); i++) {
                        char h = *p++;
                        cp = cp * 16 + (unsigned)(h <= '9' ? h - '0' : tolower(h) - 'a' + 10);
                    }
                    if (cp < 0x80) {
                        if (out && o + 1 < max) out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (out && o + 2 < max) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
                    } else {
                        if (out && o + 3 < max) {
                            out[o++] = (char)(0xE0 | (cp >> 12));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    continue;
                }
                default: c = e; break;
            }
        }
        if (out && o + 1 < max) out[o++] = c;
    }
    if (*p == '"') p++;
    if (out) out[o] = '\0';
    return p;
}

static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"') return parse_string(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = parse_string(p, NULL, 0); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') p++;
    return p;
}

static void record_pid(long pid, const char *ws) {
    if (npids >= MAX_PIDS) return;
    pids[npids].pid = pid;
    snprintf(pids[npids].ws, sizeof pids[npids].ws, "%s", ws ? ws : "");
    npids++;
}

static const char *parse_object(const char *p, const char *inherited_ws);

static const char *parse_object_array(const char *p, const char *ws) {
    p = skip_ws(p);
    if (*p != '[') return skip_value(p);
    p++;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']') return p + 1;
        p = (*p == '{') ? parse_object(p, ws) : skip_value(p);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') return p + 1;
        return NULL;
    }
}

static const char *parse_object(const char *p, const char *inherited_ws) {
    p = skip_ws(p);
    if (*p != '{') return skip_value(p);
    p++;
    int is_ws = 0;
    char ws[128] = "";
    for (;;) {
        p = skip_ws(p);
        if (*p == '}') return p + 1;

        char key[64];
        p = parse_string(p, key, sizeof key);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);

        if (strcmp(key, "type") == 0) {
            char val[64];
            const char *q = parse_string(p, val, sizeof val);
            if (!q) return NULL;
            if (strcmp(val, "workspace") == 0) is_ws = 1;
            p = q;
        } else if (strcmp(key, "name") == 0) {
            if (is_ws) {
                const char *q = parse_string(p, ws, sizeof ws);
                if (!q) return NULL;
                p = q;
            } else {
                p = skip_value(p);
            }
        } else if (strcmp(key, "pid") == 0) {
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p) return NULL;
            record_pid(v, inherited_ws);
            p = end;
        } else if (strcmp(key, "nodes") == 0 || strcmp(key, "floating_nodes") == 0) {
            p = parse_object_array(p, is_ws ? ws : inherited_ws);
        } else {
            p = skip_value(p);
        }
        if (!p) return NULL;

        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') return p + 1;
        return NULL;
    }
}

/* ---- process tree: find a pid's workspace ---------------------------- */

static long ppid_of(long pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[512];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return -1; }
    fclose(f);
    char *rp = strrchr(buf, ')');
    if (!rp) return -1;
    long ppid = -1;
    if (sscanf(rp + 2, "%*c %ld", &ppid) != 1) return -1;
    return ppid;
}

/* The sway window pid, reached by climbing from maki's pid. Its index in the
 * tree walk is the window's layout order on its workspace. */
static struct pidws *find_pid(long pid) {
    long p = pid;
    for (int hop = 0; hop < 64 && p > 1; hop++) {
        for (int i = 0; i < npids; i++)
            if (pids[i].pid == p) return &pids[i];
        p = ppid_of(p);
        if (p <= 1) break;
    }
    return NULL;
}

/* ---- maki status files ----------------------------------------------- */

struct block { int wsnum; int seq; long pid; int idx; char state; };
static struct block blocks[512];
static int nblocks;

static int block_cmp(const void *a, const void *b) {
    const struct block *x = a, *y = b;
    if (x->wsnum != y->wsnum) return x->wsnum - y->wsnum;
    if (x->seq != y->seq) return x->seq - y->seq;      /* window layout order */
    if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
    if (x->idx != y->idx) return x->idx - y->idx;      /* stable within a window */
    return (int)x->state - (int)y->state;
}

/* Numbered workspace, or 9999 if unresolved (show, sorted last), or -1 for a
 * non-numbered workspace such as __i3_scratch (hidden, do not draw). */
static int ws_number(const char *ws) {
    if (!ws) return 9999;
    int n = 0, any = 0;
    for (const char *q = ws; *q >= '0' && *q <= '9'; q++) {
        n = n * 10 + (*q - '0');
        any = 1;
        if (n > 9998) return 9999;
    }
    return any ? n : -1;
}

/* maki's state dir is wherever maki.env.state_dir() points, which is not always
 * the XDG path, so probe the known candidates and read them all. */
static void collect_maki_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;

    time_t now = time(NULL);
    size_t ext = strlen(MAKI_EXT);
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len <= ext || strcmp(e->d_name + len - ext, MAKI_EXT) != 0) continue;

        char path[768];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (now - st.st_mtime > MAKI_STALE_SEC) {
            /* Dead maki process: its heartbeat never gets rewritten. Sweep it
             * so the dir stays at ~live-session size. Losing a race with maki
             * rewriting the file is harmless. */
            unlink(path);
            continue;
        }

        long pid = strtol(e->d_name, NULL, 10);
        if (pid <= 0) continue;

        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[256];
        size_t r = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[r] = '\0';

        struct pidws *rec = find_pid(pid);
        int wsnum = ws_number(rec ? rec->ws : NULL);
        if (wsnum < 0) continue;              /* __i3_scratch etc: hidden */
        int seq = rec ? (int)(rec - pids) : -1;
        for (size_t i = 0; i < r; i++) {
            if (buf[i] != '0' && buf[i] != '1') continue;
            if (nblocks >= (int)(sizeof blocks / sizeof blocks[0])) break;
            blocks[nblocks].wsnum = wsnum;
            blocks[nblocks].seq = seq;
            blocks[nblocks].pid = pid;
            blocks[nblocks].idx = (int)i;
            blocks[nblocks].state = buf[i];
            nblocks++;
        }
    }
    closedir(d);
}

static void read_maki(void) {
    nblocks = 0;
    const char *xh = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char dirs[3][512];
    int nd = 0;
    if (xh && *xh) snprintf(dirs[nd++], sizeof dirs[0], "%s/maki/statusbar", xh);
    if (home) {
        snprintf(dirs[nd++], sizeof dirs[0], "%s/.local/state/maki/statusbar", home);
        snprintf(dirs[nd++], sizeof dirs[0], "%s/.maki/statusbar", home);
    }
    for (int i = 0; i < nd; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (strcmp(dirs[i], dirs[j]) == 0) dup = 1;
        if (!dup) collect_maki_dir(dirs[i]);
    }
    qsort(blocks, (size_t)nblocks, sizeof blocks[0], block_cmp);
}

/* ---- volume via pipewire-pulse (libpulse, single-threaded) ----------- */
/*
 * One pa_mainloop, pumped by this program: no thread, no locks. At rest we ask
 * nothing; a subscribe event only sets a flag, and pulse_refresh does at most
 * one server-info and one sink-info read per tick when flagged. Every wait is
 * bounded, so a dead pipewire-pulse can never wedge the bar.
 */

static pa_mainloop *pml;
static pa_context *pctx;
static int need_name = 1;
static int need_vol = 1;
static char default_sink[256];
static double vol_frac = -1.0;
static int vol_mute = 0;
static int vol_ready = 0;

/* inotify watch on the maki status dir (set up in main). Declared here because
 * the pulse poll function below merges its fd into pulse's poll. */
static int ino_fd = -1;

static int drain_inotify(void);         /* defined below */

/* libpulse does not expose its connection fd, so we cannot poll() it alongside
 * inotify by hand. Instead we hand libpulse this poll function: it is called
 * with pulse's own fds, and we add the inotify fd before polling, so one poll()
 * wakes for either a pipewire event or a maki file change. */
static int merged_poll(struct pollfd *ufds, unsigned long nfds, int timeout, void *u) {
    (void)u;
    if (ino_fd < 0 || nfds >= 64) return poll(ufds, (nfds_t)nfds, timeout);

    struct pollfd fds[65];
    for (unsigned long i = 0; i < nfds; i++) fds[i] = ufds[i];
    fds[nfds].fd = ino_fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;

    int r = poll(fds, (nfds_t)nfds + 1, timeout);

    /* Consume any inotify events here, inside the poll, so the fd does not stay
     * readable and make the next poll return immediately. Whether a square
     * actually changed is decided by main's frame comparison, not here. */
    if (ino_fd >= 0 && (fds[nfds].revents & POLLIN)) drain_inotify();

    /* Echo pulse's own fds back. */
    for (unsigned long i = 0; i < nfds; i++) ufds[i].revents = fds[i].revents;
    return r;
}

static void pump(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void on_subscribe(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *u) {
    (void)c; (void)idx; (void)u;
    unsigned fac = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    if (fac == PA_SUBSCRIPTION_EVENT_SERVER) { need_name = 1; need_vol = 1; }
    else if (fac == PA_SUBSCRIPTION_EVENT_SINK) need_vol = 1;
}

static void on_server_info(pa_context *c, const pa_server_info *i, void *u) {
    (void)c; (void)u;
    if (i && i->default_sink_name)
        snprintf(default_sink, sizeof default_sink, "%s", i->default_sink_name);
}

static void on_sink_info(pa_context *c, const pa_sink_info *i, int eol, void *u) {
    (void)c; (void)u;
    if (eol || !i) return;
    vol_frac = (double)pa_cvolume_avg(&i->volume) / (double)PA_VOLUME_NORM;
    vol_mute = i->mute ? 1 : 0;
    vol_ready = 1;
}

static void on_ctx_state(pa_context *c, void *u) {
    (void)u;
    if (pa_context_get_state(c) == PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(c, on_subscribe, NULL);
        pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL);
        need_name = 1;
        need_vol = 1;
    }
}

/* Pump until op finishes, bounded to ~30 ms so a tick is never stalled.
 * Returns 1 if it completed; the caller retries next tick otherwise. */
static int wait_op(pa_operation *op) {
    int i = 0;
    while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING && i < 15) {
        pa_mainloop_iterate(pml, 0, NULL);
        pump(2);
        i++;
    }
    int done = !op || pa_operation_get_state(op) != PA_OPERATION_RUNNING;
    if (op) pa_operation_unref(op);
    return done;
}

static void pulse_init(void) {
    pml = pa_mainloop_new();
    if (!pml) return;
    pa_mainloop_set_poll_func(pml, merged_poll, NULL);
    pctx = pa_context_new(pa_mainloop_get_api(pml), "swaybar");
    if (!pctx) return;
    pa_context_set_state_callback(pctx, on_ctx_state, NULL);
    pa_context_connect(pctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
    for (int i = 0; i < 250; i++) {
        pa_context_state_t s = pa_context_get_state(pctx);
        if (s == PA_CONTEXT_READY || s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) break;
        pa_mainloop_iterate(pml, 0, NULL);
        pump(2);
    }
}

static void pulse_refresh(void) {
    if (!pml || !pctx) return;

    pa_mainloop_iterate(pml, 0, NULL);   /* drain subscribe events since last tick */

    pa_context_state_t s = pa_context_get_state(pctx);
    if (s != PA_CONTEXT_READY) {
        if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED || s == PA_CONTEXT_UNCONNECTED)
            pa_context_connect(pctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
        return;
    }
    if (need_name) {
        if (wait_op(pa_context_get_server_info(pctx, on_server_info, NULL))) {
            need_name = 0;
            need_vol = 1;
        }
    }
    if (need_vol && default_sink[0]) {
        if (wait_op(pa_context_get_sink_info_by_name(pctx, default_sink, on_sink_info, NULL)))
            need_vol = 0;
    }
}

/* ---- output ---------------------------------------------------------- */

static int first_block;

/* The frame is assembled here, then compared with the last one emitted; only a
 * changed frame is flushed, so identical ticks never force sway to re-lay-out. */
static char  frame[8192];
static size_t frame_len;
static char  last_frame[8192];
static size_t last_frame_len;

static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(frame + frame_len, sizeof frame - frame_len, fmt, ap);
    va_end(ap);
    if (n > 0) frame_len += (size_t)n < sizeof frame - frame_len ? (size_t)n : sizeof frame - frame_len - 1;
}

static void block_ex(const char *name, const char *text, const char *color,
                     int sep, int sep_width) {
    emit("%s{\"name\":\"%s\",\"full_text\":\"%s\"", first_block ? "" : ",", name, text);
    if (color) emit(",\"color\":\"%s\"", color);
    if (!sep) emit(",\"separator\":false");
    if (sep_width >= 0) emit(",\"separator_block_width\":%d", sep_width);
    emit("}");
    first_block = 0;
}

static void block(const char *name, const char *text, const char *color) {
    block_ex(name, text, color, 1, -1);
}

/* ---- event-driven wait ----------------------------------------------- */

/* inotify watch on the maki status dir, so a square change redraws at once
 * rather than waiting for the minute heartbeat. Best-effort: if inotify is
 * unavailable the bar still works, just with coarser latency.
 *
 * Only <pid>.status changes matter. maki publishes atomically (write .tmpXXXX,
 * then rename), so each publish also emits events for a temp file AND a burst of
 * IN_ATTRIB/IN_CLOSE_WRITE on the target; both are ignored here. Without this
 * filter the temp-file churn wakes the bar constantly. */
static int ino_wd = -1;

static void inotify_setup(const char *dir) {
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino_fd < 0) return;
    /* IN_MOVED_TO catches the atomic rename onto <pid>.status; IN_MODIFY/IN_DELETE
     * catch a plain rewrite or removal. IN_CLOSE_WRITE/IN_ATTRIB are deliberately
     * excluded — the temp file closing is not a change. */
    ino_wd = inotify_add_watch(ino_fd, dir, IN_MOVED_TO | IN_MODIFY | IN_DELETE);
    if (ino_wd < 0) { close(ino_fd); ino_fd = -1; }
}

static int is_status_name(const char *n) {
    size_t len = strlen(n), ext = strlen(MAKI_EXT);
    return len > ext && strcmp(n + len - ext, MAKI_EXT) == 0 && n[0] != '.';
}

/* Returns 1 if a real <pid>.status file changed; 0 for temp-file noise. */
static int drain_inotify(void) {
    if (ino_fd < 0) return 0;
    char buf[4096];
    int real = 0;
    for (;;) {
        ssize_t n = read(ino_fd, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t o = 0; o < n; ) {
            struct inotify_event *e = (struct inotify_event *)(buf + o);
            if (e->len && is_status_name(e->name)) real = 1;
            o += (ssize_t)sizeof *e + e->len;
        }
    }
    return real;
}

/* ---- wait: align to the wall-clock second ---------------------------- */
/*
 * Do not sleep a fixed interval: each loop costs sleep + work, so the sample
 * point drifts and the seconds clock eventually skips (e.g. :01 -> :03). This
 * is the classic i3status fix: sleep the exact remainder to the next whole
 * second, measured from now, plus (interval-1) whole seconds. At a 1 s tick
 * that is simply "sleep until .000 of the next second", and because the
 * remainder is recomputed from the clock every time, a late or early wake
 * self-corrects instead of accumulating.
 *
 * A pipewire volume event or a maki .status change may cut the wait short; the
 * caller then redraws immediately and re-enters, and the next wait simply
 * covers the (shorter) remainder to the boundary. No persisted deadline, so a
 * short wake can never cause a duplicate-then-skip.
 */

/* Microseconds from now to the next whole second, 1..1000000. */
static long usec_to_next_second(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long usec = (long)(now.tv_nsec / 1000L);
    long rem = 1000000L - usec;
    return rem < 1 ? 1 : rem;         /* never 0, so we never spin */
}

/* Seconds from CLOCK_REALTIME (time() reads the coarse clock and can lag a
 * full second at a boundary). */
static long now_sec(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (long)now.tv_sec;
}

/* Block until the next redraw is due. Returns at once for a volume change
 * (which deserves immediate feedback), otherwise at the next wall-clock second
 * boundary (the tick).
 *
 * A maki .status change does NOT return early: the square is picked up by the
 * next tick, at most one second later, so the maki heartbeat does not force a
 * redraw of its own. That keeps the bar at ~1 redraw/sec instead of ~1.3.
 *
 * The sleep is the classic i3status one — recompute the remainder to the next
 * whole second from the clock every time — so the tick always lands on the
 * boundary and never drifts or skips. The poll only ever *shortens* that wait;
 * a short wake simply loops and sleeps the remainder. */
static void wait_event(void) {
    for (;;) {
        long us = usec_to_next_second();
        /* While pulse has work queued, wake often so a volume change is noticed
         * promptly instead of waiting out the second. */
        if (pml && (need_name || need_vol || !vol_ready)) us = 50000;

        if (pml) {
            double before = vol_frac;
            int before_mute = vol_mute;
            if (pa_mainloop_prepare(pml, (int)us) < 0 || pa_mainloop_poll(pml) < 0)
                continue;
            pa_mainloop_dispatch(pml);
            pulse_refresh();                 /* resolve any volume request now */
            if (vol_ready && (vol_frac != before || vol_mute != before_mute))
                return;                      /* volume changed: redraw at once */
        } else if (ino_fd >= 0) {
            struct pollfd p = { ino_fd, POLLIN, 0 };
            if (poll(&p, 1, (int)((us + 999) / 1000)) > 0) drain_inotify();
        } else {
            nanosleep(&(struct timespec){ us / 1000000, (us % 1000000) * 1000 }, NULL);
            return;
        }

        /* No volume change: loop until the boundary, where a maki change (and
         * the clock) are picked up. */
        if (usec_to_next_second() > 900000L) return;
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    fputs("{\"version\":1}\n[\n", stdout);
    fflush(stdout);

    pulse_init();
    cpu_percent();   /* prime the delta */

    /* One inotify watch, on whichever maki dir exists. */
    {
        const char *xh = getenv("XDG_STATE_HOME");
        const char *home = getenv("HOME");
        char d[512];
        if (xh && *xh) { snprintf(d, sizeof d, "%s/maki/statusbar", xh); inotify_setup(d); }
        if (ino_fd < 0 && home) { snprintf(d, sizeof d, "%s/.maki/statusbar", home); inotify_setup(d); }
    }

    int cpu = 0;
    long last_sec = -1;
    char bat_level[48] = "--", bat_status[48] = "", bat_current[48] = "";
    char wifi[16] = "not connected";

    for (;;) {
        pulse_refresh();

        /* The once-per-second tick owns every slow source and the CPU delta.
         * These values change at most once a second (battery current is noisy
         * and would otherwise force a redraw every time it twitches), so they
         * are sampled on the tick, not on every event wake. */
        int tick = (now_sec() != last_sec);
        if (tick) {
            last_sec = now_sec();
            cpu = cpu_percent();
            read_line(BAT_DIR "/capacity", bat_level, sizeof bat_level);
            read_line(BAT_DIR "/status", bat_status, sizeof bat_status);
            read_line(BAT_DIR "/current_now", bat_current, sizeof bat_current);
            char wifi_state[48] = "";
            read_line("/sys/class/net/" WIFI_IFACE "/operstate", wifi_state, sizeof wifi_state);
            snprintf(wifi, sizeof wifi, "%s", strcmp(wifi_state, "up") == 0 ? "connected" : "not connected");
        }

        char vol[48];
        if (vol_ready) {
            if (vol_mute) snprintf(vol, sizeof vol, "muted");
            else snprintf(vol, sizeof vol, "%.2f", vol_frac < 0 ? 0.0 : vol_frac);
        } else {
            snprintf(vol, sizeof vol, "--");
        }

        char clock[64];
        /* Use clock_gettime, not time(NULL): time() reads the kernel's coarse
         * clock (CLOCK_REALTIME_COARSE), which at a second boundary can lag a
         * full second behind real time. Since the tick fires exactly on the
         * boundary, time() would render the previous second and the clock would
         * appear to skip one. */
        struct timespec nowts;
        clock_gettime(CLOCK_REALTIME, &nowts);
        struct tm tmv;
        localtime_r(&nowts.tv_sec, &tmv);
        strftime(clock, sizeof clock, "%F %T", &tmv);

        /* workspace map, then maki squares */
        npids = 0;
        char *tree = sway_get_tree(NULL);
        if (tree) { parse_object(tree, NULL); free(tree); }
        read_maki();

        char bat[128], cbuf[48], wbuf[64], vbuf[64];
        snprintf(bat, sizeof bat, "battery %s %s %s\u00b5A", bat_level, bat_status, bat_current);
        snprintf(cbuf, sizeof cbuf, "cpu %d%%", cpu);
        snprintf(wbuf, sizeof wbuf, "wifi %s", wifi);
        snprintf(vbuf, sizeof vbuf, "volume %s", vol);

        frame_len = 0;
        emit("[");
        first_block = 1;
        for (int i = 0; i < nblocks; i++) {
            int last = (i == nblocks - 1);
            /* group the squares: no separator between them, a small gap
             * instead; the last one keeps the separator so " | " precedes volume */
            block_ex("maki", GLYPH, blocks[i].state == '1' ? COLOR_BUSY : COLOR_OTHER,
                     last, last ? -1 : 5);
        }
        block("volume", vbuf, NULL);
        block("wifi", wbuf, NULL);
        block("battery", bat, NULL);
        block("cpu", cbuf, NULL);
        block("clock", clock, NULL);
        emit("],\n");

        if (frame_len != last_frame_len || memcmp(frame, last_frame, frame_len) != 0) {
            fwrite(frame, 1, frame_len, stdout);
            fflush(stdout);
            memcpy(last_frame, frame, frame_len);
            last_frame_len = frame_len;
        }

        wait_event();
    }
}
