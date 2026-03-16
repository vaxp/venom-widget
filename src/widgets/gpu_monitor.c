/*
 * gpu_monitor.c — Venom Desktop Widget
 *
 * Monitors GPU statistics for both NVIDIA and AMD cards.
 *
 * ┌──────────────────────────────────────────────────────────────┐
 * │  Visual layout:                                              │
 * │                                                              │
 * │  ● NVIDIA GeForce RTX 4070          [vendor badge]          │
 * │                                                              │
 * │       68°C          ← big Cairo temp gauge (colour-coded)   │
 * │                                                              │
 * │  GPU  ████████░░  68%                                        │
 * │  VRAM ██████░░░░  6.2 / 8.0 GB                              │
 * │  FAN  █████░░░░░  62%                                        │
 * │  PWR  ████░░░░░░  145 / 200 W                               │
 * │                                                              │
 * │  ▁▂▃▄▃▅▆▄▃▂▁ ← Cairo temperature history sparkline (60s)   │
 * │                                                              │
 * │  Last update: 14:32:07                                       │
 * └──────────────────────────────────────────────────────────────┘
 *
 * GPU detection (auto, in order):
 *   1. nvidia-smi   — NVIDIA cards (most complete data)
 *   2. /sys/class/hwmon/hwmonN/  where name == "amdgpu"
 *      + /sys/class/drm/card0/device/gpu_busy_percent  (AMD load)
 *      + /sys/class/drm/card0/device/mem_info_vram_*   (AMD VRAM)
 *   3. /sys/class/hwmon/hwmonN/  where name == "nouveau" (NVIDIA OSS)
 *
 * Dependencies:  libgtk-3, libcairo, libm  (all standard)
 *   NVIDIA users: nvidia-utils must be installed (provides nvidia-smi)
 *   AMD users:    kernel ≥ 4.17 (amdgpu driver exposes hwmon sysfs)
 *
 * Compile:
 *   gcc -shared -fPIC -o gpu_monitor.so gpu_monitor.c \
 *       $(pkg-config --cflags --libs gtk+-3.0) -lm \
 *       -I/path/to/venom-desktop/include
 *
 * Install:
 *   cp gpu_monitor.so ~/.config/venom/widgets/
 */

#include <gtk/gtk.h>
#include <cairo.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include "../../include/venom-widget-api.h"

/* ================================================================== */
/*  Constants                                                           */
/* ================================================================== */
#define UPDATE_MS        1000          /* 1-second refresh              */
#define HISTORY_LEN      60            /* seconds of temp history       */
#define GAUGE_SIZE       110           /* Cairo temp gauge diameter     */
#define BAR_HEIGHT       9             /* height of each metric bar     */
#define SPARKLINE_H      40            /* sparkline canvas height       */
#define SYS_PATH_MAX     256

/* ================================================================== */
/*  GPU vendor                                                          */
/* ================================================================== */
typedef enum { GPU_UNKNOWN = 0, GPU_NVIDIA, GPU_AMD, GPU_NOUVEAU } GpuVendor;

/* ================================================================== */
/*  Live GPU data (filled by the poller thread)                        */
/* ================================================================== */
typedef struct {
    int      temp_c;          /* degrees Celsius                       */
    int      load_pct;        /* GPU core utilisation 0-100            */
    double   vram_used_mb;    /* VRAM used in MiB                      */
    double   vram_total_mb;   /* VRAM total in MiB                     */
    int      fan_pct;         /* fan speed 0-100 (-1 if unavailable)   */
    double   power_w;         /* power draw in Watts (-1 if N/A)       */
    double   power_limit_w;   /* TDP / power limit (-1 if N/A)         */
    char     gpu_name[128];   /* human-readable card name              */
    GpuVendor vendor;
    gboolean valid;           /* TRUE once first successful read       */
} GpuData;

/* ================================================================== */
/*  Widget state                                                        */
/* ================================================================== */
typedef struct {
    /* GTK widgets */
    GtkWidget  *root_eb;
    GtkWidget  *frame;
    GtkWidget  *lbl_gpu_name;
    GtkWidget  *lbl_vendor_badge;
    GtkWidget  *gauge_da;          /* Cairo temp gauge                 */
    GtkWidget  *bars_da;           /* Cairo metric bars                */
    GtkWidget  *spark_da;          /* Cairo sparkline                  */
    GtkWidget  *lbl_updated;

    /* Data */
    GpuData     cur;               /* latest snapshot (main thread)    */
    GMutex      data_mutex;
    GpuData     pending;           /* written by worker, read by main  */

    /* History */
    int         hist[HISTORY_LEN];
    int         hist_n;            /* how many entries are valid       */
    int         hist_head;         /* ring-buffer head index           */

    /* GPU detection cache */
    GpuVendor   vendor;
    char        hwmon_path[SYS_PATH_MAX];   /* AMD/Nouveau hwmon dir   */
    char        drm_card[SYS_PATH_MAX];     /* AMD DRM card path       */

    /* Drag */
    gboolean    dragging;
    int         drag_sx, drag_sy;
    int         widget_sx, widget_sy;

    VenomDesktopAPI *api;
} GpuWidget;

static GpuWidget *g_gw = NULL;

/* ================================================================== */
/*  Colour helpers                                                      */
/* ================================================================== */
typedef struct { double r, g, b; } Col;

/* Temperature gradient: green→yellow→orange→red */
static Col temp_color(int t) {
    if (t < 50) return (Col){0.20, 0.85, 0.45};
    if (t < 65) return (Col){0.70, 0.85, 0.20};
    if (t < 75) return (Col){0.95, 0.70, 0.10};
    if (t < 85) return (Col){0.95, 0.40, 0.10};
    return             (Col){0.95, 0.15, 0.10};
}

/* Bar colour keyed on fraction 0..1 */
static Col bar_color(double f) {
    if (f < 0.5) return (Col){0.20, 0.75, 0.50};
    if (f < 0.75) return (Col){0.80, 0.72, 0.10};
    return              (Col){0.92, 0.30, 0.15};
}

/* ================================================================== */
/*  GPU DETECTION                                                       */
/* ================================================================== */

/* Try to run nvidia-smi once to confirm NVIDIA is present */
static gboolean probe_nvidia(GpuWidget *gw) {
    FILE *f = popen(
        "nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null", "r");
    if (!f) return FALSE;
    char buf[128] = {0};
    gboolean ok = (fgets(buf, sizeof(buf), f) != NULL && strlen(buf) > 2);
    pclose(f);
    if (ok) {
        /* strip trailing newline */
        buf[strcspn(buf, "\r\n")] = '\0';
        strncpy(gw->cur.gpu_name, buf, 127);
        gw->vendor = GPU_NVIDIA;
    }
    return ok;
}

/* Scan /sys/class/hwmon for amdgpu or nouveau */
static gboolean probe_hwmon(GpuWidget *gw, const char *want_name, GpuVendor v) {
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir) return FALSE;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char name_path[SYS_PATH_MAX];
        snprintf(name_path, SYS_PATH_MAX,
                 "/sys/class/hwmon/%s/name", ent->d_name);
        FILE *f = fopen(name_path, "r");
        if (!f) continue;
        char name[64] = {0};
        fgets(name, sizeof(name), f);
        fclose(f);
        name[strcspn(name, "\r\n")] = '\0';
        if (strcmp(name, want_name) == 0) {
            snprintf(gw->hwmon_path, SYS_PATH_MAX,
                     "/sys/class/hwmon/%s", ent->d_name);
            gw->vendor = v;
            /* Try to read GPU name from DRM */
            FILE *nf = fopen("/sys/class/drm/card0/device/product_name", "r");
            if (nf) {
                fgets(gw->cur.gpu_name, 127, nf);
                fclose(nf);
                gw->cur.gpu_name[strcspn(gw->cur.gpu_name, "\r\n")] = '\0';
            } else {
                strncpy(gw->cur.gpu_name,
                        v == GPU_AMD ? "AMD GPU" : "NVIDIA (nouveau)",
                        127);
            }
            closedir(dir);
            return TRUE;
        }
    }
    closedir(dir);
    return FALSE;
}

/* Find the AMD DRM card path that has gpu_busy_percent */
static void find_amd_drm(GpuWidget *gw) {
    for (int i = 0; i < 4; i++) {
        char p[SYS_PATH_MAX];
        snprintf(p, SYS_PATH_MAX,
                 "/sys/class/drm/card%d/device/gpu_busy_percent", i);
        if (access(p, R_OK) == 0) {
            snprintf(gw->drm_card, SYS_PATH_MAX,
                     "/sys/class/drm/card%d/device", i);
            return;
        }
    }
}

static void detect_gpu(GpuWidget *gw) {
    if (probe_nvidia(gw))               return;
    if (probe_hwmon(gw, "amdgpu",  GPU_AMD))    { find_amd_drm(gw); return; }
    if (probe_hwmon(gw, "nouveau", GPU_NOUVEAU)) return;
    gw->vendor = GPU_UNKNOWN;
    strncpy(gw->cur.gpu_name, "No GPU detected", 127);
}

/* ================================================================== */
/*  READ HELPERS                                                        */
/* ================================================================== */
static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    fscanf(f, "%d", &v);
    fclose(f);
    return v;
}

static long long read_ll_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long long v = -1;
    fscanf(f, "%lld", &v);
    fclose(f);
    return v;
}

/* ================================================================== */
/*  NVIDIA DATA FETCH                                                   */
/* ================================================================== */
static void fetch_nvidia(GpuData *d) {
    FILE *f = popen(
        "nvidia-smi "
        "--query-gpu=temperature.gpu,utilization.gpu,"
        "memory.used,memory.total,fan.speed,power.draw,power.limit "
        "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (!f) return;

    int   temp=0, load=0, fan=0;
    float vmem=0, vtot=0, pw=0, plim=0;

    /* nvidia-smi may output "N/A" for some fields; use liberal scanf */
    char line[256] = {0};
    if (fgets(line, sizeof(line), f)) {
        /* Replace commas with spaces for easier sscanf */
        for (char *p = line; *p; p++) if (*p == ',') *p = ' ';
        sscanf(line, "%d %d %f %f %d %f %f",
               &temp, &load, &vmem, &vtot, &fan, &pw, &plim);
    }
    pclose(f);

    d->temp_c        = temp;
    d->load_pct      = load;
    d->vram_used_mb  = vmem;
    d->vram_total_mb = vtot;
    d->fan_pct       = fan;
    d->power_w       = pw;
    d->power_limit_w = plim;
    d->valid         = TRUE;
}

/* ================================================================== */
/*  AMD DATA FETCH                                                      */
/* ================================================================== */
static void fetch_amd(GpuWidget *gw, GpuData *d) {
    /* temperature: hwmon/temp1_input is in millidegrees */
    char path[SYS_PATH_MAX];
    snprintf(path, SYS_PATH_MAX, "%s/temp1_input", gw->hwmon_path);
    int raw = read_int_file(path);
    d->temp_c = (raw > 0) ? raw / 1000 : 0;

    /* GPU load */
    if (gw->drm_card[0]) {
        snprintf(path, SYS_PATH_MAX, "%s/gpu_busy_percent", gw->drm_card);
        int load = read_int_file(path);
        d->load_pct = (load >= 0) ? load : 0;

        /* VRAM */
        snprintf(path, SYS_PATH_MAX, "%s/mem_info_vram_used", gw->drm_card);
        long long vu = read_ll_file(path);
        snprintf(path, SYS_PATH_MAX, "%s/mem_info_vram_total", gw->drm_card);
        long long vt = read_ll_file(path);
        d->vram_used_mb  = (vu > 0) ? vu / (1024.0*1024.0) : 0;
        d->vram_total_mb = (vt > 0) ? vt / (1024.0*1024.0) : 0;
    }

    /* Fan speed (pwm1 → 0-255, convert to %) */
    snprintf(path, SYS_PATH_MAX, "%s/fan1_input", gw->hwmon_path);
    int fan_rpm = read_int_file(path);
    snprintf(path, SYS_PATH_MAX, "%s/fan1_max", gw->hwmon_path);
    int fan_max = read_int_file(path);
    if (fan_rpm > 0 && fan_max > 0)
        d->fan_pct = (int)(100.0 * fan_rpm / fan_max);
    else
        d->fan_pct = -1;

    /* Power: power1_average in microwatts */
    snprintf(path, SYS_PATH_MAX, "%s/power1_average", gw->hwmon_path);
    int pw_uw = read_int_file(path);
    d->power_w = (pw_uw > 0) ? pw_uw / 1e6 : -1;

    snprintf(path, SYS_PATH_MAX, "%s/power1_cap", gw->hwmon_path);
    int plim_uw = read_int_file(path);
    d->power_limit_w = (plim_uw > 0) ? plim_uw / 1e6 : -1;

    d->valid = TRUE;
}

/* ================================================================== */
/*  NOUVEAU DATA FETCH  (limited: temp only from hwmon)                */
/* ================================================================== */
static void fetch_nouveau(GpuWidget *gw, GpuData *d) {
    char path[SYS_PATH_MAX];
    snprintf(path, SYS_PATH_MAX, "%s/temp1_input", gw->hwmon_path);
    int raw = read_int_file(path);
    d->temp_c        = (raw > 0) ? raw / 1000 : 0;
    d->load_pct      = -1;
    d->vram_used_mb  = -1;
    d->vram_total_mb = -1;
    d->fan_pct       = -1;
    d->power_w       = -1;
    d->power_limit_w = -1;
    d->valid         = TRUE;
}

/* ================================================================== */
/*  WORKER THREAD                                                       */
/* ================================================================== */
static gpointer poll_thread(gpointer ud) {
    GpuWidget *gw = (GpuWidget *)ud;
    GpuData d;

    while (TRUE) {
        memset(&d, 0, sizeof(d));
        strncpy(d.gpu_name, gw->cur.gpu_name, 127);
        d.vendor = gw->vendor;
        d.fan_pct       = -1;
        d.power_w       = -1;
        d.power_limit_w = -1;

        switch (gw->vendor) {
            case GPU_NVIDIA:  fetch_nvidia(&d);           break;
            case GPU_AMD:     fetch_amd(gw, &d);          break;
            case GPU_NOUVEAU: fetch_nouveau(gw, &d);      break;
            default:          d.valid = FALSE;             break;
        }

        g_mutex_lock(&gw->data_mutex);
        gw->pending = d;
        g_mutex_unlock(&gw->data_mutex);

        g_usleep(UPDATE_MS * 1000UL);
    }
    return NULL;
}

/* ================================================================== */
/*  CAIRO DRAWING — Temperature Gauge                                   */
/*  Arc from 210° to 330° (240° sweep), filled proportionally         */
/* ================================================================== */
static gboolean draw_gauge(GtkWidget *w, cairo_t *cr, gpointer ud) {
    GpuWidget *gw = (GpuWidget *)ud;
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);
    double cx = W / 2.0, cy = H / 2.0 + 6;
    double R  = (W < H ? W : H) / 2.0 - 8;

    int temp = gw->cur.temp_c;
    Col  c   = temp_color(temp);

    /* angle constants (in radians, 0 = 3 o'clock) */
    double start_a = G_PI * (210.0 / 180.0);
    double end_a   = G_PI * (330.0 / 180.0);
    double sweep   = G_PI * (300.0 / 180.0);   /* 300° total travel   */
    double fill_a  = start_a + sweep * CLAMP(temp, 0, 110) / 110.0;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    /* ── Track (dim background arc) ── */
    cairo_set_line_width(cr, 10);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
    cairo_arc(cr, cx, cy, R, start_a, end_a);
    cairo_stroke(cr);

    /* ── Filled arc ── */
    if (temp > 0) {
        /* glow halo */
        cairo_set_line_width(cr, 18);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.12);
        cairo_arc(cr, cx, cy, R, start_a, fill_a);
        cairo_stroke(cr);
        /* main arc */
        cairo_set_line_width(cr, 10);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.90);
        cairo_arc(cr, cx, cy, R, start_a, fill_a);
        cairo_stroke(cr);
    }

    /* ── Temperature number ── */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d°", temp);
    cairo_select_font_face(cr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, R * 0.52);
    cairo_text_extents_t te;
    cairo_text_extents(cr, buf, &te);
    double tx = cx - te.width/2 - te.x_bearing;
    double ty = cy - te.height/2 - te.y_bearing + 2;
    /* subtle shadow */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.40);
    cairo_move_to(cr, tx+1, ty+1);
    cairo_show_text(cr, buf);
    /* main text */
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, buf);

    /* ── "C" unit label ── */
    cairo_set_font_size(cr, R * 0.20);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.65);
    cairo_move_to(cr, cx + te.width/2 + 3, ty + te.height * 0.35);
    cairo_show_text(cr, "C");

    /* ── Scale ticks at 0 / 55 / 110 ── */
    static const int tick_temps[] = {0, 30, 55, 80, 110};
    cairo_set_font_size(cr, R * 0.13);
    for (int i = 0; i < 5; i++) {
        double f  = tick_temps[i] / 110.0;
        double a  = start_a + sweep * f;
        double ti = R - 14, to = R - 4;
        cairo_set_line_width(cr, 1.2);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
        cairo_move_to(cr, cx + cos(a)*ti, cy + sin(a)*ti);
        cairo_line_to(cr, cx + cos(a)*to, cy + sin(a)*to);
        cairo_stroke(cr);
    }

    return FALSE;
}

/* ================================================================== */
/*  CAIRO DRAWING — Metric Bars                                         */
/*  Each metric: label  [████░░░░░░]  value text                       */
/* ================================================================== */
static gboolean draw_bars(GtkWidget *w, cairo_t *cr, gpointer ud) {
    GpuWidget *gw = (GpuWidget *)ud;
    int W = gtk_widget_get_allocated_width(w);
    GpuData *d = &gw->cur;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_select_font_face(cr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);

    typedef struct {
        const char *label;
        double frac;          /* 0-1 fill fraction              */
        char   val_str[48];   /* right-side text                */
        int    available;     /* skip if 0                      */
    } Bar;

    Bar bars[4];
    int nb = 0;

    /* GPU Load */
    if (d->load_pct >= 0) {
        bars[nb].frac = d->load_pct / 100.0;
        snprintf(bars[nb].val_str, 48, "%d%%", d->load_pct);
        bars[nb].label = "GPU ";
        bars[nb].available = 1;
        nb++;
    }

    /* VRAM */
    if (d->vram_total_mb > 0) {
        double f = d->vram_used_mb / d->vram_total_mb;
        bars[nb].frac = CLAMP(f, 0, 1);
        if (d->vram_total_mb >= 1024)
            snprintf(bars[nb].val_str, 48, "%.1f / %.0f GB",
                     d->vram_used_mb/1024.0, d->vram_total_mb/1024.0);
        else
            snprintf(bars[nb].val_str, 48, "%.0f / %.0f MB",
                     d->vram_used_mb, d->vram_total_mb);
        bars[nb].label = "VRAM";
        bars[nb].available = 1;
        nb++;
    }

    /* Fan */
    if (d->fan_pct >= 0) {
        bars[nb].frac = d->fan_pct / 100.0;
        snprintf(bars[nb].val_str, 48, "%d%%", d->fan_pct);
        bars[nb].label = "FAN ";
        bars[nb].available = 1;
        nb++;
    }

    /* Power */
    if (d->power_w >= 0) {
        double denom = (d->power_limit_w > 0) ? d->power_limit_w : 250.0;
        bars[nb].frac = CLAMP(d->power_w / denom, 0, 1);
        if (d->power_limit_w > 0)
            snprintf(bars[nb].val_str, 48, "%.0f / %.0f W",
                     d->power_w, d->power_limit_w);
        else
            snprintf(bars[nb].val_str, 48, "%.0f W", d->power_w);
        bars[nb].label = "PWR ";
        bars[nb].available = 1;
        nb++;
    }

    int row_h = BAR_HEIGHT + 14;   /* bar + padding */
    double label_w = 38;
    double val_w   = 110;
    double bar_x   = label_w + 4;
    double bar_w   = W - bar_x - val_w - 6;
    if (bar_w < 20) bar_w = 20;

    for (int i = 0; i < nb; i++) {
        Bar *b  = &bars[i];
        double y = i * row_h + 4;
        double by = y + 2;

        /* label */
        cairo_set_source_rgba(cr, 0.55, 0.65, 0.80, 0.65);
        cairo_move_to(cr, 0, by + BAR_HEIGHT - 1);
        cairo_show_text(cr, b->label);

        /* track */
        cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
        cairo_rectangle(cr, bar_x, by, bar_w, BAR_HEIGHT);
        cairo_fill(cr);

        /* fill */
        Col c = bar_color(b->frac);
        double fill_w = bar_w * b->frac;

        /* glow */
        cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.15);
        cairo_rectangle(cr, bar_x, by - 1, fill_w, BAR_HEIGHT + 2);
        cairo_fill(cr);

        /* main fill — rounded right cap */
        if (fill_w > 3) {
            double r2 = BAR_HEIGHT / 2.0;
            double x0 = bar_x, y0 = by, x1 = bar_x + fill_w;
            cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.82);
            cairo_move_to(cr, x0, y0);
            cairo_line_to(cr, x1 - r2, y0);
            cairo_arc(cr, x1 - r2, y0 + r2, r2, -G_PI_2, G_PI_2);
            cairo_line_to(cr, x0, y0 + BAR_HEIGHT);
            cairo_close_path(cr);
            cairo_fill(cr);

            /* top edge highlight */
            cairo_set_line_width(cr, 0.7);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
            cairo_move_to(cr, x0, y0 + 0.5);
            cairo_line_to(cr, x1 - r2, y0 + 0.5);
            cairo_stroke(cr);
        }

        /* bar outline */
        cairo_set_line_width(cr, 0.5);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
        cairo_rectangle(cr, bar_x, by, bar_w, BAR_HEIGHT);
        cairo_stroke(cr);

        /* value text */
        cairo_set_source_rgba(cr, c.r * 0.85 + 0.15, c.g * 0.85 + 0.15,
                               c.b * 0.85 + 0.15, 0.90);
        cairo_move_to(cr, bar_x + bar_w + 6, by + BAR_HEIGHT - 1);
        cairo_show_text(cr, b->val_str);
    }

    return FALSE;
}

/* ================================================================== */
/*  CAIRO DRAWING — Temperature Sparkline                              */
/* ================================================================== */
static gboolean draw_sparkline(GtkWidget *w, cairo_t *cr, gpointer ud) {
    GpuWidget *gw = (GpuWidget *)ud;
    if (gw->hist_n < 2) return FALSE;

    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);
    double pad_x = 4, pad_y = 8;
    double dw = W - 2*pad_x;
    double dh = H - 2*pad_y;

    /* find min/max over history */
    int mn = gw->hist[0], mx = gw->hist[0];
    for (int i = 1; i < gw->hist_n; i++) {
        int idx = (gw->hist_head - gw->hist_n + i + HISTORY_LEN) % HISTORY_LEN;
        if (gw->hist[idx] < mn) mn = gw->hist[idx];
        if (gw->hist[idx] > mx) mx = gw->hist[idx];
    }
    int range = mx - mn; if (range < 5) range = 5;

    /* build point arrays */
    double px[HISTORY_LEN], py[HISTORY_LEN];
    for (int i = 0; i < gw->hist_n; i++) {
        int idx = (gw->hist_head - gw->hist_n + i + HISTORY_LEN) % HISTORY_LEN;
        px[i] = pad_x + (double)i / (gw->hist_n - 1) * dw;
        py[i] = pad_y + (1.0 - (double)(gw->hist[idx] - mn) / range) * dh;
    }

    Col c = temp_color(gw->cur.temp_c);

    /* filled area */
    cairo_move_to(cr, px[0], py[0]);
    for (int i = 1; i < gw->hist_n; i++) {
        double cpx = (px[i-1] + px[i]) / 2;
        cairo_curve_to(cr, cpx, py[i-1], cpx, py[i], px[i], py[i]);
    }
    cairo_line_to(cr, px[gw->hist_n-1], H);
    cairo_line_to(cr, px[0], H);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.12);
    cairo_fill(cr);

    /* line */
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.80);
    cairo_move_to(cr, px[0], py[0]);
    for (int i = 1; i < gw->hist_n; i++) {
        double cpx = (px[i-1] + px[i]) / 2;
        cairo_curve_to(cr, cpx, py[i-1], cpx, py[i], px[i], py[i]);
    }
    cairo_stroke(cr);

    /* end dot */
    int last = gw->hist_n - 1;
    cairo_arc(cr, px[last], py[last], 3, 0, 2*G_PI);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.95);
    cairo_fill(cr);

    /* min/max labels */
    cairo_select_font_face(cr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 8.5);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d°", mx);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.28);
    cairo_move_to(cr, pad_x + 2, pad_y + 9);
    cairo_show_text(cr, buf);
    snprintf(buf, sizeof(buf), "%d°", mn);
    cairo_move_to(cr, pad_x + 2, H - 3);
    cairo_show_text(cr, buf);

    /* grid lines */
    cairo_set_line_width(cr, 0.4);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.04);
    for (int g = 1; g < 4; g++) {
        double gy = pad_y + dh * g / 4;
        cairo_move_to(cr, pad_x, gy);
        cairo_line_to(cr, pad_x + dw, gy);
    }
    cairo_stroke(cr);

    return FALSE;
}

/* ================================================================== */
/*  GTK TICK — apply pending data, push to history, redraw             */
/* ================================================================== */
static gboolean on_tick(gpointer ud) {
    GpuWidget *gw = (GpuWidget *)ud;

    /* Swap in latest data from worker thread */
    g_mutex_lock(&gw->data_mutex);
    GpuData fresh = gw->pending;
    g_mutex_unlock(&gw->data_mutex);

    if (!fresh.valid) return G_SOURCE_CONTINUE;

    /* preserve gpu_name if the poll didn't refill it */
    if (fresh.gpu_name[0] == '\0')
        strncpy(fresh.gpu_name, gw->cur.gpu_name, 127);
    gw->cur = fresh;

    /* push temperature into ring buffer */
    gw->hist[gw->hist_head] = fresh.temp_c;
    gw->hist_head = (gw->hist_head + 1) % HISTORY_LEN;
    if (gw->hist_n < HISTORY_LEN) gw->hist_n++;

    /* update timestamp */
    GDateTime *now = g_date_time_new_now_local();
    char *ts = g_date_time_format(now, "Updated: %H:%M:%S");
    gtk_label_set_text(GTK_LABEL(gw->lbl_updated), ts);
    g_free(ts);
    g_date_time_unref(now);

    /* resize bars canvas to fit actual number of bars */
    int nb = (fresh.load_pct  >= 0 ? 1 : 0) +
             (fresh.vram_total_mb > 0 ? 1 : 0) +
             (fresh.fan_pct    >= 0 ? 1 : 0) +
             (fresh.power_w    >= 0 ? 1 : 0);
    int bars_h = nb * (BAR_HEIGHT + 14) + 4;
    gtk_widget_set_size_request(gw->bars_da, -1, bars_h);

    /* redraw Cairo canvases */
    gtk_widget_queue_draw(gw->gauge_da);
    gtk_widget_queue_draw(gw->bars_da);
    gtk_widget_queue_draw(gw->spark_da);

    return G_SOURCE_CONTINUE;
}

/* ================================================================== */
/*  DRAG & DROP                                                         */
/* ================================================================== */
static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w;
    GpuWidget *gw = (GpuWidget *)ud;
    if (ev->button == 1) {
        gw->dragging = TRUE;
        gw->drag_sx  = (int)ev->x_root;
        gw->drag_sy  = (int)ev->y_root;

        gint wx = 0, wy = 0;
        if (gw->api && gw->api->layout_container) {
            gtk_widget_translate_coordinates(
                gw->root_eb, gw->api->layout_container,
                0, 0, &wx, &wy);
        }
        gw->widget_sx = wx;
        gw->widget_sy = wy;

        return TRUE;
    }
    return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer ud) {
    (void)w;
    GpuWidget *gw = (GpuWidget *)ud;
    if (gw->dragging && !(ev->state & GDK_BUTTON1_MASK)) {
        gw->dragging = FALSE;
        return FALSE;
    }
    if (gw->dragging && gw->api && gw->api->layout_container) {
        int nx = gw->widget_sx + (int)(ev->x_root - gw->drag_sx);
        int ny = gw->widget_sy + (int)(ev->y_root - gw->drag_sy);
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        gtk_layout_move(GTK_LAYOUT(gw->api->layout_container),
                        gw->root_eb, nx, ny);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w;
    GpuWidget *gw = (GpuWidget *)ud;
    if (ev->button == 1 && gw->dragging) {
        gw->dragging = FALSE;
        if (gw->api && gw->api->save_position && gw->api->layout_container) {
            gint x = 0, y = 0;
            gtk_widget_translate_coordinates(
                gw->root_eb, gw->api->layout_container,
                0, 0, &x, &y);
            gw->api->save_position("gpu_monitor.so", x, y);
        }
        return TRUE;
    }
    return FALSE;
}

/* ================================================================== */
/*  CSS                                                                 */
/* ================================================================== */
static const char *CSS =
    "frame#gpu_frame{"
    "  background-color:rgba(8,10,20,0.72);"
    "  border-radius:20px;"
    "  border:1px solid rgba(255,255,255,0.09);"
    "  padding:14px 16px 12px 16px;"
    "}"
    "#lbl_gpu_name{"
    "  color:rgba(200,215,245,0.85);"
    "  font-family:'Share Tech Mono','Liberation Mono',monospace;"
    "  font-size:12px;font-weight:700;letter-spacing:.5px;"
    "}"
    "#lbl_vendor_nvidia{"
    "  color:#76c442;font-size:9px;font-weight:700;letter-spacing:1.5px;"
    "  background:rgba(118,196,66,0.14);border-radius:4px;padding:1px 7px;"
    "}"
    "#lbl_vendor_amd{"
    "  color:#e8643c;font-size:9px;font-weight:700;letter-spacing:1.5px;"
    "  background:rgba(232,100,60,0.14);border-radius:4px;padding:1px 7px;"
    "}"
    "#lbl_vendor_other{"
    "  color:#7db8e8;font-size:9px;font-weight:700;letter-spacing:1.5px;"
    "  background:rgba(125,184,232,0.12);border-radius:4px;padding:1px 7px;"
    "}"
    "#lbl_section{"
    "  color:rgba(120,145,185,0.45);font-size:9px;"
    "  font-weight:700;letter-spacing:1.8px;"
    "}"
    "#lbl_updated{color:rgba(100,125,165,0.38);font-size:9px;}"
    "separator{background:rgba(255,255,255,0.06);min-height:1px;margin:3px 0;}";

/* ================================================================== */
/*  BUILD UI                                                            */
/* ================================================================== */
static GtkWidget *create_gpu_widget(VenomDesktopAPI *desktop_api) {
    g_gw = g_new0(GpuWidget, 1);
    GpuWidget *gw = g_gw;
    gw->api = desktop_api;
    g_mutex_init(&gw->data_mutex);

    /* detect GPU */
    detect_gpu(gw);

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /*
     * Widget hierarchy (DnD design):
     *
     *   root_eb  [GtkEventBox, visible-window=TRUE]   ← placed in GtkLayout
     *     └── drag_eb  [GtkEventBox, visible-window=FALSE]  ← catches ALL mouse events
     *           └── frame  [GtkFrame, styled]
     *                 └── vbox
     *                       ├── hdr  (labels — no events needed)
     *                       ├── gauge_da  (draw only, no event mask)
     *                       ├── bars_da   (draw only, no event mask)
     *                       └── spark_da  (draw only, no event mask)
     *
     * One drag_eb covers the whole widget surface — no dead zones on labels,
     * separators, or padding areas.  gdk_seat_grab on drag_eb's GdkWindow
     * keeps the pointer captured even during fast drags past the widget edge.
     *
     * The Cairo canvases only need their "draw" signal — no event mask.
     */

    /* root_eb: the widget the manager puts in the GtkLayout */
    gw->root_eb = gtk_event_box_new();
    gtk_widget_set_size_request(gw->root_eb, 280, -1);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(gw->root_eb), FALSE);

    /* drag_eb: transparent overlay that captures every mouse event */
    GtkWidget *drag_eb = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(drag_eb), FALSE);
    gtk_widget_set_events(drag_eb,
        GDK_BUTTON_PRESS_MASK   |
        GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK |
        GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(drag_eb, "button-press-event",   G_CALLBACK(on_press),   gw);
    g_signal_connect(drag_eb, "motion-notify-event",  G_CALLBACK(on_motion),  gw);
    g_signal_connect(drag_eb, "button-release-event", G_CALLBACK(on_release), gw);
    gtk_container_add(GTK_CONTAINER(gw->root_eb), drag_eb);

    /* frame (styled glass card) */
    gw->frame = gtk_frame_new(NULL);
    gtk_widget_set_name(gw->frame, "gpu_frame");
    gtk_frame_set_shadow_type(GTK_FRAME(gw->frame), GTK_SHADOW_NONE);
    gtk_container_set_border_width(GTK_CONTAINER(gw->frame), 0);
    gtk_container_add(GTK_CONTAINER(drag_eb), gw->frame);

    /* master vbox */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(gw->frame), vbox);

    /* ── Row 0: GPU name + vendor badge ── */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hdr, FALSE, FALSE, 0);

    gw->lbl_gpu_name = gtk_label_new(gw->cur.gpu_name);
    gtk_widget_set_name(gw->lbl_gpu_name, "lbl_gpu_name");
    gtk_widget_set_halign(gw->lbl_gpu_name, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(gw->lbl_gpu_name), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(hdr), gw->lbl_gpu_name, TRUE, TRUE, 0);

    const char *vendor_txt = "UNKNOWN";
    const char *vendor_id  = "lbl_vendor_other";
    if      (gw->vendor == GPU_NVIDIA)  { vendor_txt = "NVIDIA";  vendor_id = "lbl_vendor_nvidia"; }
    else if (gw->vendor == GPU_AMD)     { vendor_txt = "AMD";     vendor_id = "lbl_vendor_amd";    }
    else if (gw->vendor == GPU_NOUVEAU) { vendor_txt = "NOUVEAU"; }

    gw->lbl_vendor_badge = gtk_label_new(vendor_txt);
    gtk_widget_set_name(gw->lbl_vendor_badge, vendor_id);
    gtk_widget_set_valign(gw->lbl_vendor_badge, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(hdr), gw->lbl_vendor_badge, FALSE, FALSE, 0);

    /* ── Row 1: temperature gauge (Cairo — draw signal only) ── */
    gw->gauge_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(gw->gauge_da, GAUGE_SIZE, GAUGE_SIZE);
    gtk_widget_set_halign(gw->gauge_da, GTK_ALIGN_CENTER);
    g_signal_connect(gw->gauge_da, "draw", G_CALLBACK(draw_gauge), gw);
    gtk_box_pack_start(GTK_BOX(vbox), gw->gauge_da, FALSE, FALSE, 0);

    /* ── Separator ── */
    gtk_box_pack_start(GTK_BOX(vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* ── Row 2: "METRICS" section label ── */
    GtkWidget *sl = gtk_label_new("METRICS");
    gtk_widget_set_name(sl, "lbl_section");
    gtk_widget_set_halign(sl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), sl, FALSE, FALSE, 0);

    /* ── Row 3: metric bars (Cairo — draw signal only) ── */
    gw->bars_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(gw->bars_da, 240, 4 * (BAR_HEIGHT + 14) + 4);
    g_signal_connect(gw->bars_da, "draw", G_CALLBACK(draw_bars), gw);
    gtk_box_pack_start(GTK_BOX(vbox), gw->bars_da, FALSE, FALSE, 0);

    /* ── Separator ── */
    gtk_box_pack_start(GTK_BOX(vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* ── Row 4: "TEMPERATURE HISTORY" label ── */
    GtkWidget *sl2 = gtk_label_new("TEMPERATURE HISTORY  (60s)");
    gtk_widget_set_name(sl2, "lbl_section");
    gtk_widget_set_halign(sl2, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), sl2, FALSE, FALSE, 0);

    /* ── Row 5: temperature sparkline (Cairo — draw signal only) ── */
    gw->spark_da = gtk_drawing_area_new();
    gtk_widget_set_size_request(gw->spark_da, -1, SPARKLINE_H);
    g_signal_connect(gw->spark_da, "draw", G_CALLBACK(draw_sparkline), gw);
    gtk_box_pack_start(GTK_BOX(vbox), gw->spark_da, FALSE, FALSE, 0);

    /* ── Footer: last-update timestamp ── */
    gw->lbl_updated = gtk_label_new("Waiting for data…");
    gtk_widget_set_name(gw->lbl_updated, "lbl_updated");
    gtk_widget_set_halign(gw->lbl_updated, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(vbox), gw->lbl_updated, FALSE, FALSE, 0);

    gtk_widget_show_all(gw->root_eb);

    /* Start background poller + GTK tick */
    g_thread_new("gpu-poll", poll_thread, gw);
    g_timeout_add(UPDATE_MS, on_tick, gw);

    return gw->root_eb;
}

/* ================================================================== */
/*  Plugin entry point                                                  */
/* ================================================================== */
VenomWidgetAPI *venom_widget_init(void) {
    static VenomWidgetAPI api;
    api.name          = "GPU Monitor";
    api.description   = "Real-time GPU temp, load, VRAM, fan & power — NVIDIA + AMD.";
    api.author        = "Venom Community";
    api.create_widget = create_gpu_widget;
    return &api;
}
