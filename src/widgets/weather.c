/*
 * weather.c — Venom Desktop Widget  [v2 — Advanced]
 *
 * ┌─────────────────────────────────────────────────────┐
 * │  Features:                                          │
 * │  • Current conditions: temp, feels-like, condition  │
 * │  • SVG weather icons (8 types, hand-drawn)          │
 * │  • Wind speed + direction arrow                     │
 * │  • Humidity, UV index, visibility, pressure         │
 * │  • Hourly forecast strip (every 3 h, today)         │
 * │  • 3-day forecast row with hi/lo temps              │
 * │  • Cairo temperature curve (today's hourly)         │
 * │  • Dynamic background tint per weather condition    │
 * │  • Settings popover: change city without restart    │
 * │  • Right-click → instant refresh                    │
 * │  • Drag & Drop via VenomDesktopAPI                  │
 * │  • Auto-refresh every 10 min                        │
 * └─────────────────────────────────────────────────────┘
 *
 * Dependencies:
 *   libcurl, libcjson, libgtk-3, libcairo (usually bundled with GTK)
 *   Ubuntu/Debian: sudo apt install libcurl4-openssl-dev libcjson-dev
 *   Fedora:        sudo dnf install libcurl-devel cjson-devel
 *
 * Compile (standalone):
 *   gcc -shared -fPIC -o weather.so weather.c \
 *       $(pkg-config --cflags --libs gtk+-3.0) \
 *       -lcurl -lcjson -lm \
 *       -I/path/to/venom-desktop/include
 *
 * Inside Venom source tree:
 *   place in src/widgets/ then: make widgets
 *
 * Install:
 *   cp weather.so ~/.config/venom/widgets/
 *
 * Config  (~/.config/venom/weather.conf):
 *   location=Karbala
 *   unit=C          # C or F
 */

#include <gtk/gtk.h>
#include <glib.h>
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include "../../include/venom-widget-api.h"

/* ================================================================== */
/*  Constants                                                          */
/* ================================================================== */
#define UPDATE_MS       (10 * 60 * 1000)
#define CONF_FILE       "/.config/venom/weather.conf"
#define DEFAULT_CITY    "Baghdad"
#define WTTR_FMT        "https://wttr.in/%s?format=j1"
#define MAX_URL         256
#define MAX_CITY        64
#define MAX_STR         128
#define HOURLY_SLOTS    8      /* wttr gives 8x3h slots per day */
#define FORECAST_DAYS   3

/* ================================================================== */
/*  SVG icon table (8 conditions, 60x60)                               */
/* ================================================================== */
typedef enum {
    IC_SUNNY=0, IC_PARTLY, IC_CLOUDY, IC_FOG,
    IC_DRIZZLE, IC_RAIN, IC_SNOW, IC_THUNDER, IC_N
} IconType;

static const char *SVG_ICONS[IC_N] = {

[IC_SUNNY]=
"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' width='60' height='60'>"
"<circle cx='24' cy='24' r='9' fill='#FFCA28' stroke='#FFB300' stroke-width='1.2'/>"
"<g stroke='#FFCA28' stroke-width='2.2' stroke-linecap='round'>"
"<line x1='24' y1='3' x2='24' y2='8'/><line x1='24' y1='40' x2='24' y2='45'/>"
"<line x1='3' y1='24' x2='8' y2='24'/><line x1='40' y1='24' x2='45' y2='24'/>"
"<line x1='8.2' y1='8.2' x2='12' y2='12'/><line x1='36' y1='36' x2='39.8' y2='39.8'/>"
"<line x1='39.8' y1='8.2' x2='36' y2='12'/><line x1='12' y1='36' x2='8.2' y2='39.8'/>"
"</g></svg>",

[IC_PARTLY]=
"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' width='60' height='60'>"
"<circle cx='17' cy='17' r='7' fill='#FFCA28' stroke='#FFB300' stroke-width='1'/>"
"<g stroke='#FFCA28' stroke-width='1.8' stroke-linecap='round'>"
"<line x1='17' y1='6' x2='17' y2='3'/><line x1='17' y1='28' x2='17' y2='31'/>"
"<line x1='6' y1='17' x2='3' y2='17'/><line x1='28' y1='17' x2='31' y2='17'/>"
"<line x1='9.5' y1='9.5' x2='7.4' y2='7.4'/><line x1='24.5' y1='24.5' x2='26.6' y2='26.6'/>"
"</g>"
"<ellipse cx='30' cy='30' rx='11' ry='7' fill='#B0BEC5' stroke='#78909C' stroke-width='0.7'/>"
"<ellipse cx='21' cy='32' rx='8' ry='6' fill='#CFD8DC' stroke='#90A4AE' stroke-width='0.7'/>"
"<ellipse cx='35' cy='33' rx='7' ry='5' fill='#ECEFF1' stroke='#90A4AE' stroke-width='0.7'/>"
"</svg>",

[IC_CLOUDY]=
"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' width='60' height='60'>"
"<ellipse cx='27' cy='21' rx='13' ry='9' fill='#90A4AE' stroke='#607D8B' stroke-width='0.8'/>"
"<ellipse cx='16' cy='26' rx='10' ry='7' fill='#B0BEC5' stroke='#78909C' stroke-width='0.8'/>"
"<ellipse cx='32' cy='28' rx='11' ry='7' fill='#B0BEC5' stroke='#78909C' stroke-width='0.8'/>"
"<ellipse cx='22' cy='32' rx='15' ry='7' fill='#CFD8DC' stroke='#90A4AE' stroke-width='0.8'/>"
"</svg>",

[IC_FOG]=
"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' width='60' height='60'>"
"<ellipse cx='24' cy='15' rx='14' ry='7' fill='#90A4AE' stroke='#607D8B' stroke-width='0.8'/>"
"<g stroke='#78909C' stroke-width='2.5' stroke-linecap='round' opacity='0.85'>"
"<line x1='9' y1='25' x2='39' y2='25'/><line x1='12' y1='31' x2='36' y2='31'/>"
"<line x1='15' y1='37' x2='33' y2='37'/>"
"</g></svg>",

[IC_DRIZZLE]=
"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' width='60' height='60'>"
"<ellipse cx='24' cy='17' rx='13' ry='8' fill='#78909C' stroke='#546E7A' stroke-width='0.8'/>"
"<ellipse cx='15' cy='22' rx='8' ry='6' fill='#90A4AE' stroke='#607D8B' stroke-width='0.8'/>"
"<ellipse cx='33' cy='22' rx='8' ry='6' fill='#90A4AE' stroke='#607D8B' stroke-width='0.8'/>"
"<g stroke='#64B5F6' stroke-width='1.8' stroke-linecap='round'>"
"<line x1='15' y1='31' x2='13' y2='37'/><line x1='24' y1='31' x2='22' y2='37'/>"
"<line x1='33' y1='31' x2='31' y2='37'/>"
"</g></svg>",

[IC_RAIN]=
"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' width='60' height='60'>"
"<ellipse cx='24' cy='15' rx='13' ry='8' fill='#546E7A' stroke='#37474F' stroke-width='0.8'/>"
"<ellipse cx='15' cy='20' rx='9' ry='6' fill='#607D8B' stroke='#455A64' stroke-width='0.8'/>"
"<ellipse cx='33' cy='20' rx='9' ry='6' fill='#607D8B' stroke='#455A64' stroke-width='0.8'/>"
"<g stroke='#42A5F5' stroke-width='2.2' stroke-linecap='round'>"
"<line x1='13' y1='30' x2='10' y2='41'/><line x1='21' y1='30' x2='18' y2='41'/>"
"<line x1='29' y1='30' x2='26' y2='41'/><line x1='37' y1='30' x2='34' y2='41'/>"
"</g></svg>",

[IC_SNOW]=
"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' width='60' height='60'>"
"<ellipse cx='24' cy='15' rx='13' ry='7' fill='#78909C' stroke='#546E7A' stroke-width='0.8'/>"
"<g stroke='#E3F2FD' stroke-width='2' stroke-linecap='round'>"
"<line x1='24' y1='27' x2='24' y2='45'/><line x1='15' y1='36' x2='33' y2='36'/>"
"<line x1='17' y1='28.5' x2='31' y2='43.5'/><line x1='31' y1='28.5' x2='17' y2='43.5'/>"
"</g>"
"<circle cx='24' cy='27' r='2.2' fill='#E3F2FD'/><circle cx='24' cy='45' r='2.2' fill='#E3F2FD'/>"
"<circle cx='15' cy='36' r='2.2' fill='#E3F2FD'/><circle cx='33' cy='36' r='2.2' fill='#E3F2FD'/>"
"</svg>",

[IC_THUNDER]=
"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' width='60' height='60'>"
"<ellipse cx='24' cy='14' rx='13' ry='8' fill='#37474F' stroke='#263238' stroke-width='0.8'/>"
"<ellipse cx='15' cy='20' rx='9' ry='6' fill='#455A64' stroke='#37474F' stroke-width='0.8'/>"
"<ellipse cx='33' cy='20' rx='9' ry='6' fill='#455A64' stroke='#37474F' stroke-width='0.8'/>"
"<polygon points='27,28 20,39 25.5,39 22,49 34,33 28,33' fill='#FFD600' stroke='#FF8F00' stroke-width='0.8'/>"
"</svg>",
};

/* ================================================================== */
/*  Weather-code -> icon + background palette                          */
/* ================================================================== */
typedef struct { float r, g, b; } RGB;

static IconType code_to_icon(int c) {
    if (c==113)                 return IC_SUNNY;
    if (c==116)                 return IC_PARTLY;
    if (c==119||c==122)         return IC_CLOUDY;
    if (c==143||c==248||c==260) return IC_FOG;
    if (c>=263&&c<=281)         return IC_DRIZZLE;
    if (c>=293&&c<=321)         return IC_RAIN;
    if (c>=323&&c<=338)         return IC_SNOW;
    if (c>=386)                 return IC_THUNDER;
    if (c>=353&&c<=377)         return IC_RAIN;
    if (c>=339&&c<=377)         return IC_SNOW;
    return IC_CLOUDY;
}

static RGB icon_bg_tint(IconType ic) {
    switch(ic) {
        case IC_SUNNY:   return (RGB){0.18f, 0.14f, 0.02f};
        case IC_PARTLY:  return (RGB){0.10f, 0.13f, 0.22f};
        case IC_CLOUDY:  return (RGB){0.08f, 0.10f, 0.16f};
        case IC_FOG:     return (RGB){0.12f, 0.12f, 0.14f};
        case IC_DRIZZLE: return (RGB){0.04f, 0.10f, 0.22f};
        case IC_RAIN:    return (RGB){0.02f, 0.07f, 0.25f};
        case IC_SNOW:    return (RGB){0.14f, 0.18f, 0.28f};
        case IC_THUNDER: return (RGB){0.12f, 0.05f, 0.20f};
        default:         return (RGB){0.06f, 0.08f, 0.18f};
    }
}

/* ================================================================== */
/*  Data structures                                                     */
/* ================================================================== */
typedef struct {
    int hour;
    int temp_c;
    int feels_c;
    int humidity;
    int wind_kmh;
    int wind_deg;
    IconType icon;
} HourlySlot;

typedef struct {
    char     date[12];
    char     day_name[16];
    int      max_c;
    int      min_c;
    IconType icon;
    int      uv;
} DayForecast;

typedef struct {
    char     condition[MAX_STR];
    int      temp_c;
    int      feels_c;
    int      humidity;
    int      wind_kmh;
    int      wind_deg;
    int      visibility_km;
    int      pressure_mb;
    int      uv_index;
    IconType icon;
    HourlySlot hourly[HOURLY_SLOTS];
    int        n_hourly;
    DayForecast forecast[FORECAST_DAYS];
    int         n_forecast;
    gboolean success;
} WeatherData;

/* ================================================================== */
/*  Widget state                                                        */
/* ================================================================== */
typedef struct {
    char city[MAX_CITY];
    char unit;

    GtkWidget *root_eb;
    GtkWidget *frame;
    GtkWidget *svg_icon;
    GtkWidget *lbl_city;
    GtkWidget *lbl_temp;
    GtkWidget *lbl_condition;
    GtkWidget *lbl_feels;
    GtkWidget *lbl_humidity;
    GtkWidget *lbl_wind;
    GtkWidget *lbl_wind_dir;
    GtkWidget *lbl_uv;
    GtkWidget *lbl_pressure;
    GtkWidget *lbl_visibility;
    GtkWidget *lbl_updated;
    GtkWidget *spinner;

    GtkWidget *hourly_box;
    GtkWidget *hourly_lbl_time[HOURLY_SLOTS];
    GtkWidget *hourly_img[HOURLY_SLOTS];
    GtkWidget *hourly_lbl_temp[HOURLY_SLOTS];

    GtkWidget *forecast_box;
    GtkWidget *fc_lbl_day[FORECAST_DAYS];
    GtkWidget *fc_img[FORECAST_DAYS];
    GtkWidget *fc_lbl_hi[FORECAST_DAYS];
    GtkWidget *fc_lbl_lo[FORECAST_DAYS];

    GtkWidget *curve_da;
    GtkWidget *settings_btn;
    GtkWidget *popover;
    GtkWidget *entry_city;

    int  curve_temps[HOURLY_SLOTS];
    int  curve_n;
    IconType current_icon;

    gboolean dragging;
    int drag_sx, drag_sy, widget_sx, widget_sy;

    VenomDesktopAPI *api;
} WeatherWidget;

static WeatherWidget *g_ww = NULL;

/* ================================================================== */
/*  Config                                                              */
/* ================================================================== */
static void load_config(WeatherWidget *ww) {
    strncpy(ww->city, DEFAULT_CITY, MAX_CITY-1);
    ww->unit = 'C';
    const char *home = g_get_home_dir();
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONF_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L>0 && (line[L-1]=='\n'||line[L-1]=='\r'||line[L-1]==' ')) line[--L]='\0';
        if (!strncmp(line,"location=",9)) strncpy(ww->city, line+9, MAX_CITY-1);
        if (!strncmp(line,"unit=",5) && (line[5]=='F'||line[5]=='f')) ww->unit='F';
    }
    fclose(f);
}

static void save_city(WeatherWidget *ww) {
    const char *home = g_get_home_dir();
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONF_FILE);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "location=%s\nunit=%c\n", ww->city, ww->unit);
    fclose(f);
}

/* ================================================================== */
/*  Helpers                                                             */
/* ================================================================== */
static int to_display(WeatherWidget *ww, int c) {
    return (ww->unit=='F') ? (c*9/5 + 32) : c;
}

static const char *wind_arrow(int deg) {
    static const char *arr[] = {"N","NE","E","SE","S","SW","W","NW"};
    return arr[((int)((deg+22.5f)/45.0f))%8];
}

static const char *dayname_from_date(const char *date) {
    static const char *days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    struct tm t = {0};
    sscanf(date, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
    t.tm_year -= 1900; t.tm_mon -= 1;
    mktime(&t);
    return days[t.tm_wday];
}

/* ================================================================== */
/*  cURL                                                                */
/* ================================================================== */
typedef struct { char *data; size_t size; } Buf;
static size_t curl_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t total = sz*nm;
    Buf *b = (Buf*)ud;
    char *tmp = realloc(b->data, b->size+total+1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data+b->size, ptr, total);
    b->size += total;
    b->data[b->size] = '\0';
    return total;
}

/* ================================================================== */
/*  Fetch + parse                                                       */
/* ================================================================== */
static WeatherData *fetch_weather(const char *city) {
    WeatherData *wd = g_new0(WeatherData, 1);

    char safe[MAX_CITY*3+1]; int si=0;
    for (int i=0; city[i]&&si<(int)sizeof(safe)-4; i++)
        safe[si++] = (city[i]==' ') ? '+' : city[i];
    safe[si]='\0';

    char url[MAX_URL];
    snprintf(url, sizeof(url), WTTR_FMT, safe);

    CURL *curl = curl_easy_init();
    if (!curl) return wd;
    Buf buf={NULL,0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "venom-weather/2.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res!=CURLE_OK||!buf.data) { free(buf.data); return wd; }

    cJSON *root = cJSON_Parse(buf.data);
    free(buf.data);
    if (!root) return wd;

    /* current_condition */
    cJSON *cc_arr = cJSON_GetObjectItem(root,"current_condition");
    if (cJSON_IsArray(cc_arr) && cJSON_GetArraySize(cc_arr)>0) {
        cJSON *cc = cJSON_GetArrayItem(cc_arr,0);
#define GINT(k,dst) { cJSON *j=cJSON_GetObjectItem(cc,k); if(j&&j->valuestring) dst=atoi(j->valuestring); }
        GINT("temp_C",         wd->temp_c)
        GINT("FeelsLikeC",     wd->feels_c)
        GINT("humidity",       wd->humidity)
        GINT("windspeedKmph",  wd->wind_kmh)
        GINT("winddirDegree",  wd->wind_deg)
        GINT("visibility",     wd->visibility_km)
        GINT("pressure",       wd->pressure_mb)
        GINT("uvIndex",        wd->uv_index)
#undef GINT
        int wcode=113;
        cJSON *wc_j=cJSON_GetObjectItem(cc,"weatherCode");
        if(wc_j&&wc_j->valuestring) wcode=atoi(wc_j->valuestring);
        wd->icon = code_to_icon(wcode);
        cJSON *desc=cJSON_GetObjectItem(cc,"weatherDesc");
        if(cJSON_IsArray(desc)&&cJSON_GetArraySize(desc)>0){
            cJSON *d=cJSON_GetObjectItem(cJSON_GetArrayItem(desc,0),"value");
            if(d&&d->valuestring) strncpy(wd->condition,d->valuestring,MAX_STR-1);
        }
    }

    /* weather (daily) */
    cJSON *weather_arr = cJSON_GetObjectItem(root,"weather");
    if (cJSON_IsArray(weather_arr)) {
        int nd = cJSON_GetArraySize(weather_arr);
        if (nd>FORECAST_DAYS) nd=FORECAST_DAYS;
        wd->n_forecast = nd;
        for (int di=0; di<nd; di++) {
            cJSON *day = cJSON_GetArrayItem(weather_arr,di);
            DayForecast *df = &wd->forecast[di];
            cJSON *j;
            j=cJSON_GetObjectItem(day,"date");      if(j&&j->valuestring) strncpy(df->date,j->valuestring,11);
            strncpy(df->day_name, dayname_from_date(df->date), 15);
            j=cJSON_GetObjectItem(day,"maxtempC");  if(j&&j->valuestring) df->max_c=atoi(j->valuestring);
            j=cJSON_GetObjectItem(day,"mintempC");  if(j&&j->valuestring) df->min_c=atoi(j->valuestring);
            j=cJSON_GetObjectItem(day,"uvIndex");   if(j&&j->valuestring) df->uv=atoi(j->valuestring);

            /* icon from first hourly */
            cJSON *ha=cJSON_GetObjectItem(day,"hourly");
            if(cJSON_IsArray(ha)&&cJSON_GetArraySize(ha)>0){
                cJSON *h0=cJSON_GetArrayItem(ha,0);
                int wc=113;
                cJSON *wcj=cJSON_GetObjectItem(h0,"weatherCode");
                if(wcj&&wcj->valuestring) wc=atoi(wcj->valuestring);
                df->icon=code_to_icon(wc);
            }

            /* hourly slots (today only) */
            if (di==0) {
                cJSON *ha2=cJSON_GetObjectItem(day,"hourly");
                if(cJSON_IsArray(ha2)){
                    int nh=cJSON_GetArraySize(ha2);
                    if(nh>HOURLY_SLOTS) nh=HOURLY_SLOTS;
                    wd->n_hourly=nh;
                    for(int hi=0;hi<nh;hi++){
                        cJSON *h=cJSON_GetArrayItem(ha2,hi);
                        HourlySlot *sl=&wd->hourly[hi];
                        cJSON *tj;
#define HI(k,dst) tj=cJSON_GetObjectItem(h,k); if(tj&&tj->valuestring) dst=atoi(tj->valuestring);
                        HI("time",           sl->hour)
                        HI("tempC",          sl->temp_c)
                        HI("FeelsLikeC",     sl->feels_c)
                        HI("humidity",       sl->humidity)
                        HI("windspeedKmph",  sl->wind_kmh)
                        HI("winddirDegree",  sl->wind_deg)
#undef HI
                        int wc=113;
                        cJSON *wcj=cJSON_GetObjectItem(h,"weatherCode");
                        if(wcj&&wcj->valuestring) wc=atoi(wcj->valuestring);
                        sl->icon=code_to_icon(wc);
                    }
                }
            }
        }
    }

    wd->success = TRUE;
    cJSON_Delete(root);
    return wd;
}

/* ================================================================== */
/*  SVG -> GdkPixbuf                                                   */
/* ================================================================== */
static GdkPixbuf *svg_to_pixbuf(const char *svg, int size) {
    GInputStream *s = g_memory_input_stream_new_from_data(svg,(gssize)strlen(svg),NULL);
    GdkPixbuf *pb = gdk_pixbuf_new_from_stream_at_scale(s,size,size,TRUE,NULL,NULL);
    g_object_unref(s);
    return pb;
}

/* ================================================================== */
/*  Cairo temperature curve                                             */
/* ================================================================== */
static gboolean on_curve_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
    WeatherWidget *ww=(WeatherWidget*)ud;
    if (ww->curve_n<2) return FALSE;

    int width  = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);

    int mn=ww->curve_temps[0], mx=ww->curve_temps[0];
    for(int i=1;i<ww->curve_n;i++){
        if(ww->curve_temps[i]<mn) mn=ww->curve_temps[i];
        if(ww->curve_temps[i]>mx) mx=ww->curve_temps[i];
    }
    int range=mx-mn; if(range<1) range=1;

    double px[HOURLY_SLOTS], py[HOURLY_SLOTS];
    double pad_x=16, pad_y=14;
    double w2=width-2*pad_x, h2=height-2*pad_y;
    for(int i=0;i<ww->curve_n;i++){
        px[i]=pad_x+(double)i/(ww->curve_n-1)*w2;
        py[i]=pad_y+(1.0-(double)(ww->curve_temps[i]-mn)/range)*h2;
    }

    /* fill under curve */
    cairo_move_to(cr,px[0],py[0]);
    for(int i=1;i<ww->curve_n;i++){
        double cx=(px[i-1]+px[i])/2;
        cairo_curve_to(cr,cx,py[i-1],cx,py[i],px[i],py[i]);
    }
    cairo_line_to(cr,px[ww->curve_n-1],height);
    cairo_line_to(cr,px[0],height);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr,0.25,0.55,1.0,0.15);
    cairo_fill(cr);

    /* glow line */
    cairo_set_line_width(cr,2.0);
    cairo_set_source_rgba(cr,0.40,0.75,1.0,0.85);
    cairo_move_to(cr,px[0],py[0]);
    for(int i=1;i<ww->curve_n;i++){
        double cx=(px[i-1]+px[i])/2;
        cairo_curve_to(cr,cx,py[i-1],cx,py[i],px[i],py[i]);
    }
    cairo_stroke(cr);

    /* dots + labels */
    for(int i=0;i<ww->curve_n;i++){
        cairo_set_source_rgba(cr,0.55,0.88,1.0,0.90);
        cairo_arc(cr,px[i],py[i],3.2,0,2*G_PI);
        cairo_fill(cr);
        char buf[8]; snprintf(buf,sizeof(buf),"%d°",ww->curve_temps[i]);
        cairo_set_source_rgba(cr,0.75,0.90,1.0,0.75);
        cairo_select_font_face(cr,"monospace",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr,9.5);
        cairo_text_extents_t te; cairo_text_extents(cr,buf,&te);
        double tx=px[i]-te.width/2;
        double ty=(i%2==0) ? py[i]-7 : py[i]+14;
        cairo_move_to(cr,tx,ty); cairo_show_text(cr,buf);
    }

    /* grid */
    cairo_set_source_rgba(cr,1,1,1,0.05);
    cairo_set_line_width(cr,0.5);
    for(int g=1;g<4;g++){
        double gy=pad_y+h2*g/4;
        cairo_move_to(cr,pad_x,gy); cairo_line_to(cr,pad_x+w2,gy);
    }
    cairo_stroke(cr);
    return FALSE;
}

/* ================================================================== */
/*  Apply data to UI (main thread)                                      */
/* ================================================================== */
static gboolean apply_data(gpointer ud) {
    WeatherData *wd=(WeatherData*)ud;
    WeatherWidget *ww=g_ww;
    if(!ww){g_free(wd);return G_SOURCE_REMOVE;}

    gtk_widget_hide(ww->spinner);
    gtk_spinner_stop(GTK_SPINNER(ww->spinner));

    if(!wd->success){
        gtk_label_set_text(GTK_LABEL(ww->lbl_condition),"Connection failed — right-click to retry");
        g_free(wd); return G_SOURCE_REMOVE;
    }

    /* background color fixed */
    ww->current_icon=wd->icon;

    /* icon */
    GdkPixbuf *pb=svg_to_pixbuf(SVG_ICONS[wd->icon],64);
    if(pb){gtk_image_set_from_pixbuf(GTK_IMAGE(ww->svg_icon),pb);g_object_unref(pb);}

    char buf[80];
    snprintf(buf,sizeof(buf),"%d°%c",to_display(ww,wd->temp_c),ww->unit);
    gtk_label_set_text(GTK_LABEL(ww->lbl_temp),buf);
    gtk_label_set_text(GTK_LABEL(ww->lbl_condition),wd->condition);
    snprintf(buf,sizeof(buf),"Feels like %d°%c",to_display(ww,wd->feels_c),ww->unit);
    gtk_label_set_text(GTK_LABEL(ww->lbl_feels),buf);
    snprintf(buf,sizeof(buf),"💧 %d%%",wd->humidity);
    gtk_label_set_text(GTK_LABEL(ww->lbl_humidity),buf);
    snprintf(buf,sizeof(buf),"💨 %d km/h",wd->wind_kmh);
    gtk_label_set_text(GTK_LABEL(ww->lbl_wind),buf);
    snprintf(buf,sizeof(buf),"%s",wind_arrow(wd->wind_deg));
    gtk_label_set_text(GTK_LABEL(ww->lbl_wind_dir),buf);
    snprintf(buf,sizeof(buf),"UV  %d",wd->uv_index);
    gtk_label_set_text(GTK_LABEL(ww->lbl_uv),buf);
    snprintf(buf,sizeof(buf),"%d mb",wd->pressure_mb);
    gtk_label_set_text(GTK_LABEL(ww->lbl_pressure),buf);
    snprintf(buf,sizeof(buf),"👁 %d km",wd->visibility_km);
    gtk_label_set_text(GTK_LABEL(ww->lbl_visibility),buf);

    /* hourly strip */
    for(int i=0;i<HOURLY_SLOTS;i++){
        if(i<wd->n_hourly){
            HourlySlot *sl=&wd->hourly[i];
            int h=sl->hour/100;
            snprintf(buf,sizeof(buf),"%02d:00",h);
            gtk_label_set_text(GTK_LABEL(ww->hourly_lbl_time[i]),buf);
            GdkPixbuf *hpb=svg_to_pixbuf(SVG_ICONS[sl->icon],28);
            if(hpb){gtk_image_set_from_pixbuf(GTK_IMAGE(ww->hourly_img[i]),hpb);g_object_unref(hpb);}
            snprintf(buf,sizeof(buf),"%d°",to_display(ww,sl->temp_c));
            gtk_label_set_text(GTK_LABEL(ww->hourly_lbl_temp[i]),buf);
        }
    }

    /* 3-day forecast */
    for(int d=0;d<FORECAST_DAYS;d++){
        if(d<wd->n_forecast){
            DayForecast *df=&wd->forecast[d];
            gtk_label_set_text(GTK_LABEL(ww->fc_lbl_day[d]),d==0?"Today":df->day_name);
            GdkPixbuf *fpb=svg_to_pixbuf(SVG_ICONS[df->icon],34);
            if(fpb){gtk_image_set_from_pixbuf(GTK_IMAGE(ww->fc_img[d]),fpb);g_object_unref(fpb);}
            snprintf(buf,sizeof(buf),"%d°",to_display(ww,df->max_c));
            gtk_label_set_text(GTK_LABEL(ww->fc_lbl_hi[d]),buf);
            snprintf(buf,sizeof(buf),"%d°",to_display(ww,df->min_c));
            gtk_label_set_text(GTK_LABEL(ww->fc_lbl_lo[d]),buf);
        }
    }

    /* curve */
    ww->curve_n=wd->n_hourly;
    for(int i=0;i<wd->n_hourly;i++)
        ww->curve_temps[i]=to_display(ww,wd->hourly[i].temp_c);
    gtk_widget_queue_draw(ww->curve_da);

    /* timestamp */
    GDateTime *now=g_date_time_new_now_local();
    char *ts=g_date_time_format(now,"Last update: %H:%M");
    gtk_label_set_text(GTK_LABEL(ww->lbl_updated),ts);
    g_free(ts); g_date_time_unref(now);

    g_free(wd);
    return G_SOURCE_REMOVE;
}

/* ================================================================== */
/*  Thread + timer                                                      */
/* ================================================================== */
static gpointer fetch_thread(gpointer ud){
    WeatherWidget *ww=(WeatherWidget*)ud;
    WeatherData *wd=fetch_weather(ww->city);
    gdk_threads_add_idle(apply_data,wd);
    return NULL;
}
static void start_fetch(WeatherWidget *ww){
    gtk_label_set_text(GTK_LABEL(ww->lbl_condition),"Updating…");
    gtk_widget_show(ww->spinner);
    gtk_spinner_start(GTK_SPINNER(ww->spinner));
    g_thread_new("wx-fetch",fetch_thread,ww);
}
static gboolean on_timer(gpointer ud){start_fetch((WeatherWidget*)ud);return G_SOURCE_CONTINUE;}

/* ================================================================== */
/*  Drag & Drop                                                         */
/* ================================================================== */
static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer ud){
    WeatherWidget *ww=(WeatherWidget*)ud;
    if(ev->button==3){start_fetch(ww);return TRUE;}
    if(ev->button==1){
        ww->dragging=TRUE; ww->drag_sx=(int)ev->x_root; ww->drag_sy=(int)ev->y_root;
        gint wx,wy;
        gtk_widget_translate_coordinates(w,gtk_widget_get_toplevel(w),0,0,&wx,&wy);
        ww->widget_sx=wx; ww->widget_sy=wy;
        return TRUE;
    }
    return FALSE;
}
static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer ud){
    WeatherWidget *ww=(WeatherWidget*)ud;
    if(ww->dragging&&ww->api&&ww->api->layout_container){
        int nx=ww->widget_sx+(int)(ev->x_root-ww->drag_sx);
        int ny=ww->widget_sy+(int)(ev->y_root-ww->drag_sy);
        gtk_layout_move(GTK_LAYOUT(ww->api->layout_container),w,nx,ny);
        return TRUE;
    }
    return FALSE;
}
static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer ud){
    WeatherWidget *ww=(WeatherWidget*)ud;
    if(ev->button==1&&ww->dragging){
        ww->dragging=FALSE;
        if(ww->api&&ww->api->save_position&&ww->api->layout_container){
            gint x,y;
            gtk_widget_translate_coordinates(w,gtk_widget_get_toplevel(w),0,0,&x,&y);
            ww->api->save_position("weather.so",x,y);
        }
        return TRUE;
    }
    return FALSE;
}

/* ================================================================== */
/*  Settings popover                                                    */
/* ================================================================== */
static void on_settings_apply(GtkButton *btn, gpointer ud){
    WeatherWidget *ww=(WeatherWidget*)ud;
    const char *txt=gtk_entry_get_text(GTK_ENTRY(ww->entry_city));
    if(txt&&strlen(txt)>0){
        strncpy(ww->city,txt,MAX_CITY-1);
        gtk_label_set_text(GTK_LABEL(ww->lbl_city),ww->city);
        save_city(ww);
        gtk_popover_popdown(GTK_POPOVER(ww->popover));
        start_fetch(ww);
    }
}

/* ================================================================== */
/*  CSS                                                                 */
/* ================================================================== */
static const char *CSS =
    "frame#weather_frame{"
    "  border-radius:22px;"
    "  border:none;"
    "  padding:14px 16px 12px 16px;"
    "  background-color:rgba(0, 0, 0, 0.23);"
    "}"
    "#lbl_city{"
    "  color:rgba(160,190,255,0.65);"
    "  font-family:'Rajdhani','Share Tech Mono',monospace;"
    "  font-size:11px;font-weight:700;letter-spacing:2.5px;"
    "}"
    "#lbl_temp{"
    "  color:#ffffff;"
    "  font-family:'Orbitron','Rajdhani','Share Tech Mono',monospace;"
    "  font-size:54px;font-weight:800;"
    "}"
    "#lbl_condition{"
    "  color:#cce0ff;"
    "  font-family:'Rajdhani','Liberation Sans',sans-serif;"
    "  font-size:14px;font-weight:600;"
    "}"
    "#lbl_feels{color:rgba(160,200,255,0.55);font-size:11px;}"
    "#lbl_humidity,#lbl_wind,#lbl_wind_dir,#lbl_uv,#lbl_pressure,#lbl_visibility{"
    "  color:#a8c8ff;"
    "  font-family:'Share Tech Mono','Liberation Mono',monospace;"
    "  font-size:11px;"
    "  background:rgba(0, 0, 0, 0.1);"
    "  border:1px solid rgba(80,130,255,0.18);"
    "  border-radius:6px;"
    "  padding:2px 7px;"
    "}"
    "#lbl_section{color:rgba(140,170,220,0.50);font-size:9px;font-weight:700;letter-spacing:1.8px;}"
    "#lbl_h_temp{color:#90c8ff;font-size:11px;font-weight:600;}"
    "#lbl_h_time{color:rgba(160,190,230,0.55);font-size:10px;}"
    "#lbl_fc_day{color:#a0c0ff;font-size:11px;font-weight:700;}"
    "#lbl_fc_hi{color:#ffffff;font-size:12px;font-weight:700;}"
    "#lbl_fc_lo{color:rgba(130,170,220,0.55);font-size:11px;}"
    "#lbl_updated{color:rgba(120,160,200,0.40);font-size:9px;}"
    "#btn_settings{"
    "  background:rgba(255,255,255,0.06);"
    "  border:1px solid rgba(0, 0, 0, 0);"
    "  border-radius:6px;color:rgba(160,200,255,0.7);"
    "  font-size:13px;padding:1px 6px;min-width:0;"
    "}"
    "#btn_settings:hover{background:rgba(100,160,255,0.15);}"
    "separator{background:rgba(255,255,255,0.07);min-height:1px;margin:4px 0;}";

/* ================================================================== */
/*  Build UI                                                            */
/* ================================================================== */
static GtkWidget *create_weather_ui(VenomDesktopAPI *desktop_api) {
    g_ww = g_new0(WeatherWidget,1);
    WeatherWidget *ww = g_ww;
    ww->api = desktop_api;
    load_config(ww);

    GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,CSS,-1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    ww->root_eb=gtk_event_box_new();
    gtk_widget_set_events(ww->root_eb,
        GDK_BUTTON_PRESS_MASK|GDK_BUTTON_RELEASE_MASK|GDK_POINTER_MOTION_MASK);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ww->root_eb),FALSE);
    g_signal_connect(ww->root_eb,"button-press-event",  G_CALLBACK(on_press),  ww);
    g_signal_connect(ww->root_eb,"motion-notify-event", G_CALLBACK(on_motion), ww);
    g_signal_connect(ww->root_eb,"button-release-event",G_CALLBACK(on_release),ww);

    ww->frame=gtk_frame_new(NULL);
    gtk_widget_set_name(ww->frame,"weather_frame");
    gtk_frame_set_shadow_type(GTK_FRAME(ww->frame), GTK_SHADOW_NONE);
    gtk_container_set_border_width(GTK_CONTAINER(ww->frame), 0);
    gtk_widget_set_size_request(ww->frame,440,-1);
    gtk_container_add(GTK_CONTAINER(ww->root_eb),ww->frame);

    GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
    gtk_container_add(GTK_CONTAINER(ww->frame),vbox);

    /* header row: city + settings */
    GtkWidget *hdr=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_box_pack_start(GTK_BOX(vbox),hdr,FALSE,FALSE,0);
    ww->lbl_city=gtk_label_new(ww->city);
    gtk_widget_set_name(ww->lbl_city,"lbl_city");
    gtk_widget_set_halign(ww->lbl_city,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hdr),ww->lbl_city,TRUE,TRUE,0);
    ww->settings_btn=gtk_button_new_with_label("⚙");
    gtk_widget_set_name(ww->settings_btn,"btn_settings");
    gtk_box_pack_end(GTK_BOX(hdr),ww->settings_btn,FALSE,FALSE,0);

    /* settings popover */
    ww->popover=gtk_popover_new(ww->settings_btn);
    GtkWidget *pbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);
    gtk_widget_set_margin_start(pbox,10);gtk_widget_set_margin_end(pbox,10);
    gtk_widget_set_margin_top(pbox,8);  gtk_widget_set_margin_bottom(pbox,8);
    gtk_container_add(GTK_CONTAINER(ww->popover),pbox);
    gtk_box_pack_start(GTK_BOX(pbox),gtk_label_new("City / Location"),FALSE,FALSE,0);
    ww->entry_city=gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ww->entry_city),ww->city);
    gtk_entry_set_width_chars(GTK_ENTRY(ww->entry_city),18);
    gtk_box_pack_start(GTK_BOX(pbox),ww->entry_city,FALSE,FALSE,0);
    GtkWidget *abtn=gtk_button_new_with_label("Apply");
    g_signal_connect(abtn,"clicked",G_CALLBACK(on_settings_apply),ww);
    gtk_box_pack_start(GTK_BOX(pbox),abtn,FALSE,FALSE,0);
    gtk_widget_show_all(pbox);
    g_signal_connect_swapped(ww->settings_btn,"clicked",G_CALLBACK(gtk_popover_popup),ww->popover);

    /* current: icon + temp + condition */
    GtkWidget *row1=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
    gtk_box_pack_start(GTK_BOX(vbox),row1,FALSE,FALSE,0);
    ww->svg_icon=gtk_image_new();
    gtk_widget_set_valign(ww->svg_icon,GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row1),ww->svg_icon,FALSE,FALSE,0);
    ww->spinner=gtk_spinner_new();
    gtk_widget_set_size_request(ww->spinner,56,56);
    gtk_widget_set_no_show_all(ww->spinner,TRUE);
    gtk_box_pack_start(GTK_BOX(row1),ww->spinner,FALSE,FALSE,0);
    GtkWidget *colr=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
    gtk_widget_set_valign(colr,GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row1),colr,TRUE,TRUE,0);
    ww->lbl_temp=gtk_label_new("—");
    gtk_widget_set_name(ww->lbl_temp,"lbl_temp");
    gtk_widget_set_halign(ww->lbl_temp,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(colr),ww->lbl_temp,FALSE,FALSE,0);
    ww->lbl_condition=gtk_label_new("Updating…");
    gtk_widget_set_name(ww->lbl_condition,"lbl_condition");
    gtk_widget_set_halign(ww->lbl_condition,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(colr),ww->lbl_condition,FALSE,FALSE,0);
    ww->lbl_feels=gtk_label_new("");
    gtk_widget_set_name(ww->lbl_feels,"lbl_feels");
    gtk_widget_set_halign(ww->lbl_feels,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(colr),ww->lbl_feels,FALSE,FALSE,0);

    /* detail pills row 1 */
    GtkWidget *p1=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
    gtk_box_pack_start(GTK_BOX(vbox),p1,FALSE,FALSE,0);
    ww->lbl_humidity=gtk_label_new("💧 —");     gtk_widget_set_name(ww->lbl_humidity,"lbl_humidity");
    ww->lbl_wind=gtk_label_new("💨 —");          gtk_widget_set_name(ww->lbl_wind,"lbl_wind");
    ww->lbl_wind_dir=gtk_label_new("—");         gtk_widget_set_name(ww->lbl_wind_dir,"lbl_wind_dir");
    gtk_box_pack_start(GTK_BOX(p1),ww->lbl_humidity,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(p1),ww->lbl_wind,    FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(p1),ww->lbl_wind_dir,FALSE,FALSE,0);

    /* detail pills row 2 */
    GtkWidget *p2=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
    gtk_box_pack_start(GTK_BOX(vbox),p2,FALSE,FALSE,0);
    ww->lbl_uv=gtk_label_new("UV —");            gtk_widget_set_name(ww->lbl_uv,"lbl_uv");
    ww->lbl_pressure=gtk_label_new("— mb");      gtk_widget_set_name(ww->lbl_pressure,"lbl_pressure");
    ww->lbl_visibility=gtk_label_new("👁 — km"); gtk_widget_set_name(ww->lbl_visibility,"lbl_visibility");
    gtk_box_pack_start(GTK_BOX(p2),ww->lbl_uv,        FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(p2),ww->lbl_pressure,  FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(p2),ww->lbl_visibility,FALSE,FALSE,0);

    /* separator */
    gtk_box_pack_start(GTK_BOX(vbox),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,0);

    /* temp curve */
    GtkWidget *sc=gtk_label_new("TODAY'S TEMPERATURE");
    gtk_widget_set_name(sc,"lbl_section");
    gtk_widget_set_halign(sc,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox),sc,FALSE,FALSE,0);
    ww->curve_da=gtk_drawing_area_new();
    gtk_widget_set_size_request(ww->curve_da,-1,62);
    g_signal_connect(ww->curve_da,"draw",G_CALLBACK(on_curve_draw),ww);
    gtk_box_pack_start(GTK_BOX(vbox),ww->curve_da,FALSE,FALSE,0);

    /* separator */
    gtk_box_pack_start(GTK_BOX(vbox),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,0);

    /* hourly strip */
    GtkWidget *sh=gtk_label_new("HOURLY FORECAST");
    gtk_widget_set_name(sh,"lbl_section");
    gtk_widget_set_halign(sh,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox),sh,FALSE,FALSE,0);
    ww->hourly_box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
    gtk_box_pack_start(GTK_BOX(vbox),ww->hourly_box,FALSE,FALSE,0);
    for(int i=0;i<HOURLY_SLOTS;i++){
        GtkWidget *col=gtk_box_new(GTK_ORIENTATION_VERTICAL,1);
        gtk_widget_set_hexpand(col,TRUE);
        ww->hourly_lbl_time[i]=gtk_label_new("—");
        gtk_widget_set_name(ww->hourly_lbl_time[i],"lbl_h_time");
        gtk_widget_set_halign(ww->hourly_lbl_time[i],GTK_ALIGN_CENTER);
        ww->hourly_img[i]=gtk_image_new();
        gtk_widget_set_halign(ww->hourly_img[i],GTK_ALIGN_CENTER);
        ww->hourly_lbl_temp[i]=gtk_label_new("—");
        gtk_widget_set_name(ww->hourly_lbl_temp[i],"lbl_h_temp");
        gtk_widget_set_halign(ww->hourly_lbl_temp[i],GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(col),ww->hourly_lbl_time[i],FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(col),ww->hourly_img[i],     FALSE,FALSE,2);
        gtk_box_pack_start(GTK_BOX(col),ww->hourly_lbl_temp[i],FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(ww->hourly_box),col,TRUE,TRUE,0);
    }

    /* separator */
    gtk_box_pack_start(GTK_BOX(vbox),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,0);

    /* 3-day forecast */
    GtkWidget *sf=gtk_label_new("3-DAY FORECAST");
    gtk_widget_set_name(sf,"lbl_section");
    gtk_widget_set_halign(sf,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox),sf,FALSE,FALSE,0);
    ww->forecast_box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
    gtk_box_pack_start(GTK_BOX(vbox),ww->forecast_box,FALSE,FALSE,0);
    for(int d=0;d<FORECAST_DAYS;d++){
        GtkWidget *col=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
        gtk_widget_set_hexpand(col,TRUE);
        ww->fc_lbl_day[d]=gtk_label_new("—");
        gtk_widget_set_name(ww->fc_lbl_day[d],"lbl_fc_day");
        gtk_widget_set_halign(ww->fc_lbl_day[d],GTK_ALIGN_CENTER);
        ww->fc_img[d]=gtk_image_new();
        gtk_widget_set_halign(ww->fc_img[d],GTK_ALIGN_CENTER);
        ww->fc_lbl_hi[d]=gtk_label_new("—");
        gtk_widget_set_name(ww->fc_lbl_hi[d],"lbl_fc_hi");
        gtk_widget_set_halign(ww->fc_lbl_hi[d],GTK_ALIGN_CENTER);
        ww->fc_lbl_lo[d]=gtk_label_new("—");
        gtk_widget_set_name(ww->fc_lbl_lo[d],"lbl_fc_lo");
        gtk_widget_set_halign(ww->fc_lbl_lo[d],GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(col),ww->fc_lbl_day[d],FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(col),ww->fc_img[d],    FALSE,FALSE,2);
        gtk_box_pack_start(GTK_BOX(col),ww->fc_lbl_hi[d], FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(col),ww->fc_lbl_lo[d], FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(ww->forecast_box),col,TRUE,TRUE,0);
    }

    /* footer */
    ww->lbl_updated=gtk_label_new("");
    gtk_widget_set_name(ww->lbl_updated,"lbl_updated");
    gtk_widget_set_halign(ww->lbl_updated,GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(vbox),ww->lbl_updated,FALSE,FALSE,0);

    gtk_widget_show_all(ww->root_eb);

    start_fetch(ww);
    g_timeout_add(UPDATE_MS,on_timer,ww);
    return ww->root_eb;
}

/* ================================================================== */
/*  Plugin entry point                                                  */
/* ================================================================== */
VenomWidgetAPI *venom_widget_init(void) {
    static VenomWidgetAPI api;
    api.name          = "Weather (Advanced)";
    api.description   = "wttr.in: current + hourly strip + 3-day forecast + temp curve.";
    api.author        = "Venom Community";
    api.create_widget = create_weather_ui;
    return &api;
}
