/*
 * sysmonitor-pro.c — Venom Desktop Widget
 * System Monitor: CPU · RAM · Disk · CPU Temp · Network
 *
 * All data read directly from /proc and /sys — zero dependencies beyond GTK.
 * Each metric has its own Cairo-drawn sparkline / ring / bar chart.
 *
 * Layout:
 *   ┌─────────────────────────────────────────┐
 *   │  ⬡ SYSTEM MONITOR          [hostname]   │
 *   ├──────────────┬──────────────────────────┤
 *   │  CPU Ring    │  sparkline history       │  CPU %  + temp
 *   ├──────────────┴──────────────────────────┤
 *   │  RAM  [████████░░░░░░░░]  used / total  │
 *   │  DISK [█████░░░░░░░░░░░]  used / total  │
 *   ├─────────────────────────────────────────┤
 *   │  NET ↑ tx_speed   ↓ rx_speed (sparkline)│
 *   └─────────────────────────────────────────┘
 *
 * Compile:
 *   gcc -shared -fPIC -o sysmonitor-pro.so sysmonitor-pro.c \
 *       $(pkg-config --cflags --libs gtk+-3.0) -lm \
 *       -I/path/to/venom-desktop/include
 *
 * Install:
 *   cp sysmonitor-pro.so ~/.config/venom/widgets/
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include "../../include/venom-widget-api.h"

/* ─── Dimensions ─── */
#define WIDGET_W       340
#define RING_SIZE       90     /* CPU ring canvas */
#define SPARK_W        210     /* sparkline canvas width */
#define SPARK_H         50
#define NET_SPARK_H     44
#define HISTORY_LEN     60     /* samples kept */
#define BAR_H           10     /* RAM/Disk bar height */

/* ─── Palette ─── */
/* Dark terminal / cyberpunk aesthetic */
#define C_BG0   0.055, 0.060, 0.090    /* card background */
#define C_BG1   0.075, 0.082, 0.125    /* slightly lighter panel */
#define C_GRID  0.13,  0.14,  0.22     /* grid lines */
#define C_CPU   0.20,  0.85,  0.60     /* neon green  – CPU   */
#define C_TEMP  1.00,  0.42,  0.18     /* hot orange  – Temp  */
#define C_RAM   0.30,  0.60,  1.00     /* electric blue – RAM */
#define C_DISK  0.75,  0.35,  1.00     /* violet – Disk       */
#define C_TX    0.00,  0.88,  0.88     /* cyan – upload       */
#define C_RX    1.00,  0.80,  0.10     /* amber – download    */
#define C_TEXT  0.78,  0.82,  0.96     /* main text           */
#define C_DIM   0.40,  0.43,  0.60     /* dimmed / labels     */

/* ══════════════════════════════════════════════
   Data structures
   ══════════════════════════════════════════════ */

#define NCPU 16   /* max CPU cores tracked */

typedef struct {
    /* CPU */
    gdouble cpu_pct;            /* 0-100 */
    gdouble cpu_history[HISTORY_LEN];
    gint    cpu_hist_pos;

    /* per-core raw ticks for delta */
    gulong  cpu_prev_idle;
    gulong  cpu_prev_total;

    /* Temperature */
    gdouble cpu_temp;           /* °C, or -1 if unavailable */

    /* RAM */
    gulong  ram_total_kb;
    gulong  ram_used_kb;

    /* Disk (root fs) */
    gulong  disk_total_gb;
    gulong  disk_used_gb;

    /* Network */
    gulong  net_rx_bytes_prev;
    gulong  net_tx_bytes_prev;
    gdouble net_rx_kbps;
    gdouble net_tx_kbps;
    gdouble net_rx_history[HISTORY_LEN];
    gdouble net_tx_history[HISTORY_LEN];
    gint    net_hist_pos;
    gdouble net_rx_peak;
    gdouble net_tx_peak;

    /* UI refs */
    GtkWidget *cpu_ring_area;
    GtkWidget *cpu_spark_area;
    GtkWidget *ram_bar_area;
    GtkWidget *disk_bar_area;
    GtkWidget *net_spark_area;
    GtkWidget *lbl_cpu_pct;
    GtkWidget *lbl_cpu_temp;
    GtkWidget *lbl_ram;
    GtkWidget *lbl_disk;
    GtkWidget *lbl_net_tx;
    GtkWidget *lbl_net_rx;
    GtkWidget *lbl_hostname;

    /* Drag */
    gboolean  dragging;
    gint      drag_rx, drag_ry, drag_wx, drag_wy;
    VenomDesktopAPI *api;
} SysState;

static SysState M;

/* ══════════════════════════════════════════════
   Data readers
   ══════════════════════════════════════════════ */

/* CPU usage % via /proc/stat */
static gdouble read_cpu_pct(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0;
    char line[256];
    fgets(line, sizeof(line), f);
    fclose(f);

    gulong user, nice, system, idle, iowait, irq, softirq, steal;
    sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
           &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

    gulong total = user + nice + system + idle + iowait + irq + softirq + steal;
    gulong d_total = total - M.cpu_prev_total;
    gulong d_idle  = idle  - M.cpu_prev_idle;

    M.cpu_prev_total = total;
    M.cpu_prev_idle  = idle;

    if (d_total == 0) return M.cpu_pct;
    return CLAMP(100.0 * (1.0 - (gdouble)d_idle / (gdouble)d_total), 0.0, 100.0);
}

/* CPU temperature: try hwmon, then coretemp, then acpitz */
static gdouble read_cpu_temp(void) {
    /* Common paths for CPU package / core temp */
    const char *paths[] = {
        /* hwmon-based (AMD/Intel k10temp, coretemp) */
        "/sys/class/hwmon/hwmon0/temp1_input",
        "/sys/class/hwmon/hwmon1/temp1_input",
        "/sys/class/hwmon/hwmon2/temp1_input",
        "/sys/class/hwmon/hwmon3/temp1_input",
        "/sys/class/hwmon/hwmon4/temp1_input",
        /* ACPI thermal zone */
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        long raw = 0;
        fscanf(f, "%ld", &raw);
        fclose(f);
        if (raw > 0) return raw / 1000.0;
    }
    return -1.0;
}

/* RAM from /proc/meminfo */
static void read_ram(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    gulong mem_total = 0, mem_free = 0, buffers = 0, cached = 0, sreclaimable = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        gulong val;
        if      (sscanf(line, "MemTotal: %lu",      &val) == 1) mem_total     = val;
        else if (sscanf(line, "MemFree: %lu",       &val) == 1) mem_free      = val;
        else if (sscanf(line, "Buffers: %lu",       &val) == 1) buffers       = val;
        else if (sscanf(line, "Cached: %lu",        &val) == 1) cached        = val;
        else if (sscanf(line, "SReclaimable: %lu",  &val) == 1) sreclaimable  = val;
    }
    fclose(f);
    M.ram_total_kb = mem_total;
    M.ram_used_kb  = mem_total - mem_free - buffers - cached - sreclaimable;
    if ((glong)M.ram_used_kb < 0) M.ram_used_kb = 0;
}

/* Disk usage for root '/' */
static void read_disk(void) {
    struct statvfs sv;
    if (statvfs("/", &sv) != 0) return;
    gulong block = sv.f_bsize;
    M.disk_total_gb = (sv.f_blocks * block) / (1024*1024*1024UL);
    M.disk_used_gb  = ((sv.f_blocks - sv.f_bfree) * block) / (1024*1024*1024UL);
}

/* Network speed from /proc/net/dev — uses first non-loopback interface */
static void read_net(void) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[256];
    fgets(line, sizeof(line), f); /* header 1 */
    fgets(line, sizeof(line), f); /* header 2 */

    gulong rx = 0, tx = 0;
    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        gulong r_bytes, r_pkts, r_err, r_drop, r_x1, r_x2, r_x3, r_x4;
        gulong t_bytes, t_pkts, t_err, t_drop, t_x1, t_x2, t_x3, t_x4;
        if (sscanf(line,
            " %31[^:]: %lu %lu %lu %lu %lu %lu %lu %lu "
            "%lu %lu %lu %lu %lu %lu %lu %lu",
            iface,
            &r_bytes, &r_pkts, &r_err, &r_drop,
            &r_x1, &r_x2, &r_x3, &r_x4,
            &t_bytes, &t_pkts, &t_err, &t_drop,
            &t_x1, &t_x2, &t_x3, &t_x4) == 17) {
            if (strcmp(iface, "lo") == 0) continue;
            rx += r_bytes;
            tx += t_bytes;
        }
    }
    fclose(f);

    if (M.net_rx_bytes_prev > 0) {
        M.net_rx_kbps = (rx - M.net_rx_bytes_prev) / 1024.0 / 2.0; /* per sec (2s interval) */
        M.net_tx_kbps = (tx - M.net_tx_bytes_prev) / 1024.0 / 2.0;
        if (M.net_rx_kbps < 0) M.net_rx_kbps = 0;
        if (M.net_tx_kbps < 0) M.net_tx_kbps = 0;
        /* Update peaks */
        if (M.net_rx_kbps > M.net_rx_peak) M.net_rx_peak = M.net_rx_kbps;
        if (M.net_tx_kbps > M.net_tx_peak) M.net_tx_peak = M.net_tx_kbps;
    }
    M.net_rx_bytes_prev = rx;
    M.net_tx_bytes_prev = tx;
}

/* Format bytes/s nicely */
static void fmt_speed(gdouble kbps, char *out, int len) {
    if (kbps >= 1024*1024)
        snprintf(out, len, "%.1f GB/s", kbps / (1024*1024));
    else if (kbps >= 1024)
        snprintf(out, len, "%.1f MB/s", kbps / 1024);
    else
        snprintf(out, len, "%.0f KB/s", kbps);
}

static void fmt_bytes(gulong kb, char *out, int len) {
    if (kb >= 1024*1024)
        snprintf(out, len, "%.1f GB", kb / (1024.0*1024));
    else if (kb >= 1024)
        snprintf(out, len, "%.0f MB", kb / 1024.0);
    else
        snprintf(out, len, "%lu KB", kb);
}

/* ══════════════════════════════════════════════
   Cairo drawing helpers
   ══════════════════════════════════════════════ */

static void set_rgb(cairo_t *cr, double r, double g, double b) {
    cairo_set_source_rgb(cr, r, g, b);
}
static void set_rgba(cairo_t *cr, double r, double g, double b, double a) {
    cairo_set_source_rgba(cr, r, g, b, a);
}

/* Rounded rectangle */
static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x+w-r, y+r,   r, -G_PI/2,  0);
    cairo_arc(cr, x+w-r, y+h-r, r,  0,        G_PI/2);
    cairo_arc(cr, x+r,   y+h-r, r,  G_PI/2,   G_PI);
    cairo_arc(cr, x+r,   y+r,   r,  G_PI,    3*G_PI/2);
    cairo_close_path(cr);
}

/* Draw a sparkline given an array of values (ring buffer).
 * fill=TRUE draws a filled area chart, fill=FALSE draws only the line.
 * r/g/b: line color */
static void draw_sparkline(cairo_t *cr,
                            double x, double y, double w, double h,
                            const gdouble *hist, int hist_len, int hist_pos,
                            double max_val,
                            double r, double g, double b,
                            gboolean fill) {
    if (max_val <= 0) max_val = 1.0;

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    /* Grid lines */
    cairo_set_line_width(cr, 0.5);
    set_rgba(cr, C_GRID, 1.0);
    for (int i = 1; i < 4; i++) {
        double gy = y + h * i / 4.0;
        cairo_move_to(cr, x,   gy);
        cairo_line_to(cr, x+w, gy);
        cairo_stroke(cr);
    }

    /* Compute points */
    int n = hist_len;
    double step = w / (n - 1.0);

    /* Build path */
    gboolean started = FALSE;
    for (int i = 0; i < n; i++) {
        int idx = (hist_pos + i) % n;
        double val = hist[idx];
        double px  = x + i * step;
        double py  = y + h - (val / max_val) * h;
        py = CLAMP(py, y, y + h);
        if (!started) {
            cairo_move_to(cr, px, py);
            started = TRUE;
        } else {
            cairo_line_to(cr, px, py);
        }
    }

    if (fill) {
        /* Close for fill */
        double last_x = x + (n-1) * step;
        cairo_line_to(cr, last_x, y + h);
        cairo_line_to(cr, x, y + h);
        cairo_close_path(cr);
        /* Gradient fill */
        cairo_pattern_t *pg = cairo_pattern_create_linear(0, y, 0, y+h);
        cairo_pattern_add_color_stop_rgba(pg, 0.0, r, g, b, 0.38);
        cairo_pattern_add_color_stop_rgba(pg, 1.0, r, g, b, 0.03);
        cairo_set_source(cr, pg);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(pg);

        /* Re-draw line on top */
        for (int i = 0; i < n; i++) {
            int idx = (hist_pos + i) % n;
            double val = hist[idx];
            double px  = x + i * step;
            double py  = y + h - (val / max_val) * h;
            py = CLAMP(py, y, y + h);
            if (i == 0) cairo_move_to(cr, px, py);
            else        cairo_line_to(cr, px, py);
        }
    }

    cairo_set_line_width(cr, 1.6);
    set_rgba(cr, r, g, b, 0.95);
    cairo_stroke(cr);

    /* Dot at last value */
    {
        int last_idx = (hist_pos + n - 1) % n;
        double lval = hist[last_idx];
        double lx   = x + (n-1) * step;
        double ly   = y + h - (lval / max_val) * h;
        ly = CLAMP(ly, y, y + h);
        cairo_arc(cr, lx, ly, 3.0, 0, 2*G_PI);
        set_rgb(cr, r, g, b);
        cairo_fill(cr);
    }

    cairo_restore(cr);
}

/* ── CPU Ring (arc gauge) ── */
static gboolean on_draw_cpu_ring(GtkWidget *w, cairo_t *cr, gpointer d) {
    int sz = gtk_widget_get_allocated_width(w);
    int sh = gtk_widget_get_allocated_height(w);
    double cx = sz / 2.0, cy = sh / 2.0;
    double outer_r = sz * 0.42;
    double inner_r = sz * 0.28;
    double start_a = G_PI * 0.75;
    double full_a  = G_PI * 1.5;   /* 270° sweep */
    double pct     = M.cpu_pct / 100.0;

    /* Background arc */
    cairo_set_line_width(cr, outer_r - inner_r);
    set_rgba(cr, C_GRID, 1.0);
    cairo_arc(cr, cx, cy, (outer_r+inner_r)/2.0, start_a, start_a + full_a);
    cairo_stroke(cr);

    /* Colored arc */
    if (pct > 0.001) {
        /* Color gradient: green → orange → red */
        double r, g, b;
        if (pct < 0.5) {
            r = pct * 2 * 0.9;  g = 0.85; b = 0.2 + pct * 0.1;
        } else {
            r = 0.9 + (pct-0.5)*0.2; g = 0.85 - (pct-0.5)*1.4; b = 0.2;
        }
        r = CLAMP(r,0,1); g = CLAMP(g,0,1); b = CLAMP(b,0,1);

        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        /* Glow */
        cairo_set_line_width(cr, (outer_r - inner_r) + 6);
        set_rgba(cr, r, g, b, 0.12);
        cairo_arc(cr, cx, cy, (outer_r+inner_r)/2.0,
                  start_a, start_a + full_a * pct);
        cairo_stroke(cr);
        /* Main arc */
        cairo_set_line_width(cr, outer_r - inner_r - 2);
        set_rgba(cr, r, g, b, 1.0);
        cairo_arc(cr, cx, cy, (outer_r+inner_r)/2.0,
                  start_a, start_a + full_a * pct);
        cairo_stroke(cr);
    }

    /* Tick marks */
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= 10; i++) {
        double a   = start_a + full_a * i / 10.0;
        double len = (i % 5 == 0) ? 6.0 : 3.0;
        set_rgba(cr, C_DIM, 0.7);
        cairo_move_to(cr, cx + cos(a) * (outer_r + 3),
                          cy + sin(a) * (outer_r + 3));
        cairo_line_to(cr, cx + cos(a) * (outer_r + 3 + len),
                          cy + sin(a) * (outer_r + 3 + len));
        cairo_stroke(cr);
    }

    /* Center text: percentage */
    cairo_select_font_face(cr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, sz * 0.17);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f%%", M.cpu_pct);
    cairo_text_extents_t te;
    cairo_text_extents(cr, buf, &te);
    set_rgba(cr, C_TEXT, 1.0);
    cairo_move_to(cr,
        cx - te.width/2 - te.x_bearing,
        cy - te.height/2 - te.y_bearing);
    cairo_show_text(cr, buf);

    /* Label "CPU" */
    cairo_set_font_size(cr, sz * 0.10);
    cairo_text_extents(cr, "CPU", &te);
    set_rgba(cr, C_DIM, 0.9);
    cairo_move_to(cr, cx - te.width/2 - te.x_bearing, cy + sz*0.18);
    cairo_show_text(cr, "CPU");

    return FALSE;
}

/* ── CPU sparkline ── */
static gboolean on_draw_cpu_spark(GtkWidget *w, cairo_t *cr, gpointer d) {
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    draw_sparkline(cr, 0, 0, W, H,
                   M.cpu_history, HISTORY_LEN, M.cpu_hist_pos,
                   100.0, C_CPU, TRUE);

    /* Temperature badge */
    if (M.cpu_temp > 0) {
        double r = 0.20, g = 0.85, b = 0.05;
        if (M.cpu_temp > 70) { r = 1.0; g = 0.42; b = 0.18; }
        else if (M.cpu_temp > 55) { r = 1.0; g = 0.75; b = 0.10; }

        cairo_select_font_face(cr, "monospace",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%.0f°C", M.cpu_temp);
        cairo_text_extents_t te;
        cairo_text_extents(cr, tbuf, &te);
        /* Badge background */
        double bx = W - te.width - 10, by = 4;
        rrect(cr, bx - 4, by - 2, te.width + 10, te.height + 6, 4);
        set_rgba(cr, r, g, b, 0.18);
        cairo_fill(cr);
        set_rgba(cr, r, g, b, 1.0);
        cairo_move_to(cr, bx, by - te.y_bearing);
        cairo_show_text(cr, tbuf);
    }

    /* Y-axis label */
    cairo_select_font_face(cr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9);
    set_rgba(cr, C_DIM, 0.7);
    cairo_move_to(cr, 3, 10);
    cairo_show_text(cr, "100%");
    cairo_move_to(cr, 3, H - 3);
    cairo_show_text(cr, "0%");

    return FALSE;
}

/* ── RAM bar ── */
static gboolean on_draw_ram_bar(GtkWidget *w, cairo_t *cr, gpointer d) {
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    gdouble pct = (M.ram_total_kb > 0)
        ? CLAMP((gdouble)M.ram_used_kb / M.ram_total_kb, 0, 1) : 0;

    /* Track */
    rrect(cr, 0, 0, W, H, H/2.0);
    set_rgba(cr, C_GRID, 1.0);
    cairo_fill(cr);

    /* Fill */
    if (pct > 0.01) {
        double fill_w = pct * W;
        /* Color: green → yellow → red – stays blue */

        cairo_pattern_t *pg = cairo_pattern_create_linear(0, 0, fill_w, 0);
        cairo_pattern_add_color_stop_rgb(pg, 0.0, 0.30, 0.60, 1.00);
        cairo_pattern_add_color_stop_rgb(pg, 0.7, 0.20, 0.50, 0.95);
        if (pct > 0.8) {
            cairo_pattern_add_color_stop_rgb(pg, 1.0, 1.0, 0.35, 0.20);
        } else {
            cairo_pattern_add_color_stop_rgb(pg, 1.0, 0.20, 0.50, 0.95);
        }
        rrect(cr, 0, 0, fill_w, H, H/2.0);
        cairo_set_source(cr, pg);
        cairo_fill(cr);
        cairo_pattern_destroy(pg);

        /* Glow */
        set_rgba(cr, 0.30, 0.60, 1.00, 0.20);
        rrect(cr, 0, 0, fill_w, H, H/2.0);
        cairo_fill(cr);
    }

    return FALSE;
}

/* ── Disk bar ── */
static gboolean on_draw_disk_bar(GtkWidget *w, cairo_t *cr, gpointer d) {
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    gdouble pct = (M.disk_total_gb > 0)
        ? CLAMP((gdouble)M.disk_used_gb / M.disk_total_gb, 0, 1) : 0;

    rrect(cr, 0, 0, W, H, H/2.0);
    set_rgba(cr, C_GRID, 1.0);
    cairo_fill(cr);

    if (pct > 0.01) {
        double fill_w = pct * W;
        cairo_pattern_t *pg = cairo_pattern_create_linear(0, 0, fill_w, 0);
        cairo_pattern_add_color_stop_rgb(pg, 0.0, 0.75, 0.35, 1.00);
        cairo_pattern_add_color_stop_rgb(pg, 1.0, 0.55, 0.20, 0.90);
        rrect(cr, 0, 0, fill_w, H, H/2.0);
        cairo_set_source(cr, pg);
        cairo_fill(cr);
        cairo_pattern_destroy(pg);

        set_rgba(cr, 0.75, 0.35, 1.00, 0.20);
        rrect(cr, 0, 0, fill_w, H, H/2.0);
        cairo_fill(cr);
    }

    return FALSE;
}

/* ── Network dual sparkline ── */
static gboolean on_draw_net_spark(GtkWidget *w, cairo_t *cr, gpointer d) {
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    /* Split vertically: top = TX, bottom = RX */
    int half = H / 2 - 1;

    double tx_peak = M.net_tx_peak > 0 ? M.net_tx_peak : 1.0;
    double rx_peak = M.net_rx_peak > 0 ? M.net_rx_peak : 1.0;

    /* TX — top half */
    draw_sparkline(cr, 0, 0, W, half,
                   M.net_tx_history, HISTORY_LEN, M.net_hist_pos,
                   tx_peak, C_TX, TRUE);

    /* Center divider */
    cairo_set_line_width(cr, 0.5);
    set_rgba(cr, C_GRID, 1.0);
    cairo_move_to(cr, 0,   H/2.0);
    cairo_line_to(cr, W,   H/2.0);
    cairo_stroke(cr);

    /* RX — bottom half (flip vertically) */
    cairo_save(cr);
    cairo_translate(cr, 0, H);
    cairo_scale(cr, 1, -1);
    draw_sparkline(cr, 0, 0, W, half,
                   M.net_rx_history, HISTORY_LEN, M.net_hist_pos,
                   rx_peak, C_RX, TRUE);
    cairo_restore(cr);

    /* Labels inside chart */
    cairo_select_font_face(cr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9);
    set_rgba(cr, 0.0, 0.88, 0.88, 0.70);
    cairo_move_to(cr, 4, 10);
    cairo_show_text(cr, "▲ TX");
    set_rgba(cr, 1.0, 0.80, 0.10, 0.70);
    cairo_move_to(cr, 4, H - 3);
    cairo_show_text(cr, "▼ RX");

    return FALSE;
}

/* ══════════════════════════════════════════════
   Poll / update
   ══════════════════════════════════════════════ */
static gboolean update_stats(gpointer data) {
    /* Gather data */
    M.cpu_pct  = read_cpu_pct();
    M.cpu_temp = read_cpu_temp();
    read_ram();
    read_disk();
    read_net();

    /* Push history */
    M.cpu_history[M.cpu_hist_pos]     = M.cpu_pct;
    M.net_rx_history[M.net_hist_pos]  = M.net_rx_kbps;
    M.net_tx_history[M.net_hist_pos]  = M.net_tx_kbps;
    M.cpu_hist_pos = (M.cpu_hist_pos + 1) % HISTORY_LEN;
    M.net_hist_pos = (M.net_hist_pos + 1) % HISTORY_LEN;

    /* Redraw canvases */
    gtk_widget_queue_draw(M.cpu_ring_area);
    gtk_widget_queue_draw(M.cpu_spark_area);
    gtk_widget_queue_draw(M.ram_bar_area);
    gtk_widget_queue_draw(M.disk_bar_area);
    gtk_widget_queue_draw(M.net_spark_area);

    /* RAM label */
    char used_buf[32], total_buf[32];
    fmt_bytes(M.ram_used_kb, used_buf, sizeof(used_buf));
    fmt_bytes(M.ram_total_kb, total_buf, sizeof(total_buf));
    char ram_str[64];
    snprintf(ram_str, sizeof(ram_str), "%s / %s", used_buf, total_buf);
    gtk_label_set_text(GTK_LABEL(M.lbl_ram), ram_str);

    /* Disk label */
    char disk_str[64];
    snprintf(disk_str, sizeof(disk_str), "%lu GB / %lu GB",
             M.disk_used_gb, M.disk_total_gb);
    gtk_label_set_text(GTK_LABEL(M.lbl_disk), disk_str);

    /* Network labels */
    char tx_buf[32], rx_buf[32];
    fmt_speed(M.net_tx_kbps, tx_buf, sizeof(tx_buf));
    fmt_speed(M.net_rx_kbps, rx_buf, sizeof(rx_buf));
    gtk_label_set_text(GTK_LABEL(M.lbl_net_tx), tx_buf);
    gtk_label_set_text(GTK_LABEL(M.lbl_net_rx), rx_buf);

    return TRUE;
}

/* ══════════════════════════════════════════════
   Drag & Drop
   ══════════════════════════════════════════════ */
static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button != 1) return FALSE;
    M.dragging = TRUE;
    M.drag_rx = ev->x_root; M.drag_ry = ev->y_root;
    gint wx, wy;
    gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &wx, &wy);
    M.drag_wx = wx; M.drag_wy = wy;
    return TRUE;
}
static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d) {
    if (!M.dragging || !M.api || !M.api->layout_container) return FALSE;
    gtk_layout_move(GTK_LAYOUT(M.api->layout_container), w,
        M.drag_wx + (int)(ev->x_root - M.drag_rx),
        M.drag_wy + (int)(ev->y_root - M.drag_ry));
    return TRUE;
}
static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button != 1 || !M.dragging) return FALSE;
    M.dragging = FALSE;
    if (M.api && M.api->save_position && M.api->layout_container) {
        gint x, y;
        gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &x, &y);
        M.api->save_position("sysmonitor-pro.so", x, y);
    }
    return TRUE;
}

/* ══════════════════════════════════════════════
   CSS
   ══════════════════════════════════════════════ */
static const gchar *SM_CSS =
    /* Card */
    "box.sm-card {"
    "  background-color: rgba(0, 0, 0, 0.23);"
    "  border: 1px solid rgba(80,90,160,0.22);"
    "  border-radius: 14px;"
    "  padding: 12px 14px 14px 14px;"
    "}"
    /* Header */
    "label.sm-header {"
    "  color: rgba(120,200,255,0.55);"
    "  font-family: monospace;"
    "  font-size: 9px;"
    "  letter-spacing: 3px;"
    "}"
    "label.sm-hostname {"
    "  color: rgba(200,210,255,0.40);"
    "  font-family: monospace;"
    "  font-size: 9px;"
    "}"
    /* Section title */
    "label.sm-section {"
    "  color: rgba(130,140,200,0.65);"
    "  font-family: monospace;"
    "  font-size: 9px;"
    "  letter-spacing: 2px;"
    "}"
    /* Value labels */
    "label.sm-value {"
    "  color: rgba(200,210,255,0.85);"
    "  font-family: monospace;"
    "  font-size: 10.5px;"
    "}"
    "label.sm-tx {"
    "  color: rgba(0,224,224,0.90);"
    "  font-family: monospace;"
    "  font-size: 10.5px;"
    "}"
    "label.sm-rx {"
    "  color: rgba(255,204,26,0.90);"
    "  font-family: monospace;"
    "  font-size: 10.5px;"
    "}"
    /* Separator */
    "separator.sm-sep {"
    "  background-color: rgba(80,90,160,0.18);"
    "  min-height: 1px;"
    "  margin: 4px 0;"
    "}";

/* ══════════════════════════════════════════════
   Helper: section row  (label left, value right)
   ══════════════════════════════════════════════ */
static GtkWidget *make_row(const gchar *title_txt,
                            const gchar *val_class,
                            GtkWidget  **lbl_val_out) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *ltitle = gtk_label_new(title_txt);
    gtk_style_context_add_class(gtk_widget_get_style_context(ltitle), "sm-section");
    gtk_widget_set_halign(ltitle, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), ltitle, FALSE, FALSE, 0);

    *lbl_val_out = gtk_label_new("—");
    gtk_style_context_add_class(gtk_widget_get_style_context(*lbl_val_out),
                                 val_class ? val_class : "sm-value");
    gtk_widget_set_halign(*lbl_val_out, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(row), *lbl_val_out, FALSE, FALSE, 0);
    return row;
}

/* ══════════════════════════════════════════════
   Widget construction
   ══════════════════════════════════════════════ */
static GtkWidget *create_sysmon_widget(VenomDesktopAPI *api) {
    memset(&M, 0, sizeof(M));
    M.api = api;

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, SM_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Root event box */
    GtkWidget *root = gtk_event_box_new();
    gtk_widget_set_events(root,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(root), FALSE);
    gtk_widget_set_size_request(root, WIDGET_W, -1);
    g_signal_connect(root, "button-press-event",   G_CALLBACK(on_press),   NULL);
    g_signal_connect(root, "motion-notify-event",  G_CALLBACK(on_motion),  NULL);
    g_signal_connect(root, "button-release-event", G_CALLBACK(on_release), NULL);

    /* Main card */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "sm-card");
    gtk_container_add(GTK_CONTAINER(root), card);

    /* ── Header ── */
    GtkWidget *hdr_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(card), hdr_row, FALSE, FALSE, 0);

    GtkWidget *lbl_hdr = gtk_label_new("⬡ SYSTEM MONITOR");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_hdr), "sm-header");
    gtk_widget_set_halign(lbl_hdr, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hdr_row), lbl_hdr, TRUE, TRUE, 0);

    /* hostname */
    M.lbl_hostname = gtk_label_new("");
    {
        struct utsname u;
        if (uname(&u) == 0)
            gtk_label_set_text(GTK_LABEL(M.lbl_hostname), u.nodename);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(M.lbl_hostname), "sm-hostname");
    gtk_widget_set_halign(M.lbl_hostname, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(hdr_row), M.lbl_hostname, FALSE, FALSE, 0);

    /* Separator */
    GtkWidget *sep0 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(sep0), "sm-sep");
    gtk_box_pack_start(GTK_BOX(card), sep0, FALSE, FALSE, 0);

    /* ════ CPU SECTION ════ */
    /* Row: ring + sparkline side by side */
    GtkWidget *cpu_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(card), cpu_row, FALSE, FALSE, 0);

    /* Ring */
    M.cpu_ring_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(M.cpu_ring_area, RING_SIZE, RING_SIZE);
    g_signal_connect(M.cpu_ring_area, "draw", G_CALLBACK(on_draw_cpu_ring), NULL);
    gtk_box_pack_start(GTK_BOX(cpu_row), M.cpu_ring_area, FALSE, FALSE, 0);

    /* Spark column */
    GtkWidget *spark_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(spark_col, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(cpu_row), spark_col, TRUE, TRUE, 0);

    GtkWidget *cpu_title_row = make_row("CPU HISTORY", "sm-value", &M.lbl_cpu_pct);
    gtk_box_pack_start(GTK_BOX(spark_col), cpu_title_row, FALSE, FALSE, 0);

    M.cpu_spark_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(M.cpu_spark_area, SPARK_W, SPARK_H);
    g_signal_connect(M.cpu_spark_area, "draw", G_CALLBACK(on_draw_cpu_spark), NULL);
    gtk_box_pack_start(GTK_BOX(spark_col), M.cpu_spark_area, TRUE, TRUE, 0);

    /* Separator */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(sep1), "sm-sep");
    gtk_box_pack_start(GTK_BOX(card), sep1, FALSE, FALSE, 0);

    /* ════ RAM SECTION ════ */
    GtkWidget *ram_title = make_row("RAM", "sm-value", &M.lbl_ram);
    gtk_box_pack_start(GTK_BOX(card), ram_title, FALSE, FALSE, 0);

    M.ram_bar_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(M.ram_bar_area, -1, BAR_H);
    g_signal_connect(M.ram_bar_area, "draw", G_CALLBACK(on_draw_ram_bar), NULL);
    gtk_box_pack_start(GTK_BOX(card), M.ram_bar_area, FALSE, FALSE, 0);

    /* ════ DISK SECTION ════ */
    GtkWidget *disk_title = make_row("DISK  /", "sm-value", &M.lbl_disk);
    gtk_box_pack_start(GTK_BOX(card), disk_title, FALSE, FALSE, 0);

    M.disk_bar_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(M.disk_bar_area, -1, BAR_H);
    g_signal_connect(M.disk_bar_area, "draw", G_CALLBACK(on_draw_disk_bar), NULL);
    gtk_box_pack_start(GTK_BOX(card), M.disk_bar_area, FALSE, FALSE, 0);

    /* Separator */
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(sep2), "sm-sep");
    gtk_box_pack_start(GTK_BOX(card), sep2, FALSE, FALSE, 0);

    /* ════ NETWORK SECTION ════ */
    GtkWidget *net_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(card), net_hdr, FALSE, FALSE, 0);

    GtkWidget *lbl_net_title = gtk_label_new("NETWORK");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_net_title), "sm-section");
    gtk_widget_set_halign(lbl_net_title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(net_hdr), lbl_net_title, TRUE, TRUE, 0);

    /* TX + RX values */
    GtkWidget *net_val_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(net_val_col, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(net_hdr), net_val_col, FALSE, FALSE, 0);

    GtkWidget *tx_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(net_val_col), tx_row, FALSE, FALSE, 0);
    GtkWidget *lbl_tx_icon = gtk_label_new("▲");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_tx_icon), "sm-tx");
    gtk_box_pack_start(GTK_BOX(tx_row), lbl_tx_icon, FALSE, FALSE, 0);
    M.lbl_net_tx = gtk_label_new("0 KB/s");
    gtk_style_context_add_class(gtk_widget_get_style_context(M.lbl_net_tx), "sm-tx");
    gtk_box_pack_start(GTK_BOX(tx_row), M.lbl_net_tx, FALSE, FALSE, 0);

    GtkWidget *rx_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(net_val_col), rx_row, FALSE, FALSE, 0);
    GtkWidget *lbl_rx_icon = gtk_label_new("▼");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_rx_icon), "sm-rx");
    gtk_box_pack_start(GTK_BOX(rx_row), lbl_rx_icon, FALSE, FALSE, 0);
    M.lbl_net_rx = gtk_label_new("0 KB/s");
    gtk_style_context_add_class(gtk_widget_get_style_context(M.lbl_net_rx), "sm-rx");
    gtk_box_pack_start(GTK_BOX(rx_row), M.lbl_net_rx, FALSE, FALSE, 0);

    /* Network sparkline */
    M.net_spark_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(M.net_spark_area, -1, NET_SPARK_H * 2 + 8);
    g_signal_connect(M.net_spark_area, "draw", G_CALLBACK(on_draw_net_spark), NULL);
    gtk_box_pack_start(GTK_BOX(card), M.net_spark_area, FALSE, FALSE, 0);

    gtk_widget_show_all(root);

    /* Seed history with zeros */
    memset(M.cpu_history,    0, sizeof(M.cpu_history));
    memset(M.net_rx_history, 0, sizeof(M.net_rx_history));
    memset(M.net_tx_history, 0, sizeof(M.net_tx_history));

    /* Prime CPU delta counters with one read */
    read_cpu_pct();

    /* Start updating every 2 seconds */
    update_stats(NULL);
    g_timeout_add(1000, update_stats, NULL);

    return root;
}

/* ══════════════════════════════════════════════
   Venom entry point
   ══════════════════════════════════════════════ */
VenomWidgetAPI *venom_widget_init(void) {
    static VenomWidgetAPI api;
    api.name          = "System Monitor Pro";
    api.description   = "CPU · RAM · Disk · Temp · Network — live charts";
    api.author        = "Venom Community";
    api.create_widget = create_sysmon_widget;
    return &api;
}
