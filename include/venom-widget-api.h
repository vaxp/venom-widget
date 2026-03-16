/* venom-widget-api.h */
#ifndef VENOM_WIDGET_API_H
#define VENOM_WIDGET_API_H

#include <gtk/gtk.h>

/* API exposed from the Manager to the Widget */
typedef struct {
    GtkWidget *layout_container;
    void (*save_position)(const char *widget_name, int x, int y);
} VenomDesktopAPI;

/* API struct returned by every valid venom widget (.so) */
typedef struct {
    const char *name;
    const char *description;
    const char *author;
    
    /* Initialization hook: 
     * The widget allocates its GTK UI, sets up its own internal update
     * timers (via g_timeout_add), connects mouse motion for dragging
     * via layout_container, and returns the root GtkWidget*.
     */
    GtkWidget* (*create_widget)(VenomDesktopAPI *desktop_api);
} VenomWidgetAPI;

/* The expected factory symbol used dynamically by the loader. */
/* extern VenomWidgetAPI venom_widget_init(void); */

#endif
