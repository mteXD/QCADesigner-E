#ifndef QCAD_VIEWPORT_SCROLL_H
#define QCAD_VIEWPORT_SCROLL_H

#include <gtk/gtk.h>

typedef void (*ViewportPanFunc) (int dx, int dy, gpointer data) ;

void viewport_scroll_attach (GtkWidget *widget, ViewportPanFunc apply, gpointer data) ;
void viewport_scroll_cancel (GtkWidget *widget) ;
void viewport_scroll_set_pinching (GtkWidget *widget, gboolean active) ;
gboolean viewport_scroll_handle (GtkWidget *widget, GdkEventScroll *event) ;

#endif
