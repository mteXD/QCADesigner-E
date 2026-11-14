#include <math.h>
#include <string.h>
#include "viewport_scroll.h"

#if defined(HAVE_XINPUT_GESTURES) && defined(GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#include <X11/extensions/XInput2.h>
#endif

#define SCROLL_PIXELS 100.0
#define WHEEL_DURATION 80000
#define STATE_KEY "qcad-viewport-scroll"

typedef struct
  {
  GtkWidget *widget ;
  ViewportPanFunc apply ;
  ViewportZoomFunc zoom ;
  gpointer data ;
  double remainder[2], pending[2] ;
  gint64 last_frame, deadline ;
  guint timer ;
  gboolean pinching ;
#if defined(HAVE_XINPUT_GESTURES) && defined(GDK_WINDOWING_X11)
  Display *display ;
  int opcode ;
  GHashTable *devices ;
  GIOChannel *channel ;
  guint watch ;
  Time consumed[64] ;
  unsigned int consumed_count ;
#endif
  } ViewportScroll ;

static void apply_delta (ViewportScroll *state, double dx, double dy)
  {
  int x, y ;
  if (!isfinite (dx) || !isfinite (dy)) return ;
  state->remainder[0] += dx ;
  state->remainder[1] += dy ;
  x = (int)CLAMP (state->remainder[0], -100000.0, 100000.0) ;
  y = (int)CLAMP (state->remainder[1], -100000.0, 100000.0) ;
  state->remainder[0] -= x ;
  state->remainder[1] -= y ;
  if (x || y) state->apply (x, y, state->data) ;
  }

void viewport_scroll_cancel (GtkWidget *widget)
  {
  ViewportScroll *state = g_object_get_data (G_OBJECT (widget), STATE_KEY) ;
  if (NULL == state) return ;
  if (state->timer) g_source_remove (state->timer) ;
  state->timer = 0 ;
  state->pending[0] = state->pending[1] = 0 ;
  state->remainder[0] = state->remainder[1] = 0 ;
  }

static void apply_zoom (ViewportScroll *state, double dy, double x, double y)
  {
  viewport_scroll_cancel (state->widget) ;
  if (state->zoom && isfinite (dy) && dy != 0)
    state->zoom (pow (1.2, CLAMP (dy / SCROLL_PIXELS, -10.0, 10.0)), x, y, state->data) ;
  }

static gboolean wheel_frame (gpointer data)
  {
  ViewportScroll *state = data ;
  gint64 now = g_get_monotonic_time () ;
  double fraction = now >= state->deadline ? 1.0 :
    (double)(now - state->last_frame) / (state->deadline - state->last_frame) ;
  double dx = state->pending[0] * fraction, dy = state->pending[1] * fraction ;
  state->pending[0] -= dx ;
  state->pending[1] -= dy ;
  state->last_frame = now ;
  apply_delta (state, dx, dy) ;
  if (fraction < 1.0) return TRUE ;
  state->timer = 0 ;
  return FALSE ;
  }

void viewport_scroll_set_pinching (GtkWidget *widget, gboolean active)
  {
  ViewportScroll *state = g_object_get_data (G_OBJECT (widget), STATE_KEY) ;
  if (state == NULL) return ;
  viewport_scroll_cancel (widget) ;
  state->pinching = active ;
  }

static void queue_wheel (ViewportScroll *state, double dx, double dy)
  {
  // Reversals should respond immediately, not fight a queued opposite step.
  if (dx * state->pending[0] < 0) state->pending[0] = 0 ;
  if (dy * state->pending[1] < 0) state->pending[1] = 0 ;
  state->pending[0] += dx ;
  state->pending[1] += dy ;
  state->deadline = g_get_monotonic_time () + WHEEL_DURATION ;
  if (!state->timer)
    {
    state->last_frame = g_get_monotonic_time () ;
    state->timer = g_timeout_add (16, wheel_frame, state) ;
    }
  }

#if defined(HAVE_XINPUT_GESTURES) && defined(GDK_WINDOWING_X11)
typedef struct
  {
  int number[2] ;
  double increment[2], value[2] ;
  gboolean valid[2], trackpad ;
  } ScrollDevice ;

static void cache_devices (ViewportScroll *state)
  {
  int count = 0, device, cls, axis ;
  XIDeviceInfo *info = XIQueryDevice (state->display, XIAllDevices, &count) ;
  g_hash_table_remove_all (state->devices) ;
  for (device = 0 ; info != NULL && device < count ; device++)
    {
    ScrollDevice *scroll = g_new0 (ScrollDevice, 1) ;
    char *name = g_ascii_strdown (info[device].name, -1) ;
    scroll->trackpad = strstr (name, "touchpad") != NULL || strstr (name, "trackpad") != NULL ;
    g_free (name) ;
    scroll->number[0] = scroll->number[1] = -1 ;
    for (cls = 0 ; cls < info[device].num_classes ; cls++)
      if (info[device].classes[cls]->type == XIScrollClass)
        {
        XIScrollClassInfo *sc = (XIScrollClassInfo *)info[device].classes[cls] ;
        axis = sc->scroll_type == XIScrollTypeHorizontal ? 0 : 1 ;
        if (isfinite (sc->increment) && sc->increment != 0)
          {
          scroll->number[axis] = sc->number ;
          scroll->increment[axis] = sc->increment ;
          }
        }
    for (cls = 0 ; cls < info[device].num_classes ; cls++)
      if (info[device].classes[cls]->type == XIValuatorClass)
        {
        XIValuatorClassInfo *val = (XIValuatorClassInfo *)info[device].classes[cls] ;
        for (axis = 0 ; axis < 2 ; axis++)
          if (val->number == scroll->number[axis] && isfinite (val->value))
            {
            scroll->value[axis] = val->value ;
            scroll->valid[axis] = TRUE ;
            }
        }
    g_hash_table_insert (state->devices, GINT_TO_POINTER (info[device].deviceid), scroll) ;
    }
  if (info != NULL) XIFreeDeviceInfo (info) ;
  }

static void native_motion (ViewportScroll *state, XIDeviceEvent *motion)
  {
  ScrollDevice *device = g_hash_table_lookup (state->devices, GINT_TO_POINTER (motion->sourceid)) ;
  double delta[2] = {0, 0}, *value = motion->valuators.values ;
  int bit, axis ;
  gboolean scroll_axis = FALSE ;
  if (NULL == device) return ;
  for (bit = 0 ; bit < motion->valuators.mask_len * 8 ; bit++)
    if (XIMaskIsSet (motion->valuators.mask, bit))
      {
      for (axis = 0 ; axis < 2 ; axis++)
        if (bit == device->number[axis] && isfinite (*value))
          {
          scroll_axis = TRUE ;
          if (device->valid[axis])
            delta[axis] = -SCROLL_PIXELS * (*value - device->value[axis]) / device->increment[axis] ;
          device->value[axis] = *value ;
          device->valid[axis] = TRUE ;
          }
      value++ ;
      }
  if (motion->event != GDK_WINDOW_XID (state->widget->window) || !scroll_axis) return ;
  state->consumed[state->consumed_count++ % G_N_ELEMENTS (state->consumed)] = motion->time ;
  if (state->pinching) return ;
  if (motion->buttons.mask_len && motion->buttons.mask &&
      (XIMaskIsSet (motion->buttons.mask, 1) || XIMaskIsSet (motion->buttons.mask, 2) ||
       XIMaskIsSet (motion->buttons.mask, 3))) return ;
  if (!isfinite (delta[0]) || !isfinite (delta[1])) return ;
  if ((motion->mods.effective & GDK_CONTROL_MASK) && delta[1] != 0)
    {
    apply_zoom (state, delta[1], motion->event_x, motion->event_y) ;
    return ;
    }
  if (delta[0] == 0 && delta[1] == 0) return ;
  if (device->trackpad || (fabs (delta[0]) < SCROLL_PIXELS && fabs (delta[1]) < SCROLL_PIXELS))
    {
    // No timer and no momentum for high-resolution input.
    if (state->timer) viewport_scroll_cancel (state->widget) ;
    apply_delta (state, delta[0], delta[1]) ;
    }
  else queue_wheel (state, delta[0], delta[1]) ;
  }

static void drain_native (ViewportScroll *state)
  {
  XEvent event ;
  while (state->display != NULL && XPending (state->display))
    {
    XNextEvent (state->display, &event) ;
    if (event.type != GenericEvent || event.xcookie.extension != state->opcode ||
        !XGetEventData (state->display, &event.xcookie)) continue ;
    switch (event.xcookie.evtype)
      {
      case XI_Motion:
        native_motion (state, event.xcookie.data) ;
        break ;
      case XI_HierarchyChanged:
      case XI_DeviceChanged:
      case XI_Enter:
        // Refresh baselines after device changes or scrolling outside the viewport.
        cache_devices (state) ;
        {
        GHashTableIter iter ;
        gpointer value ;
        g_hash_table_iter_init (&iter, state->devices) ;
        while (g_hash_table_iter_next (&iter, NULL, &value))
          {
          ScrollDevice *device = value ;
          // Queries may be ahead of already queued motion; seed from the next event.
          device->valid[0] = device->valid[1] = FALSE ;
          }
        }
        break ;
      }
    XFreeEventData (state->display, &event.xcookie) ;
    }
  }

static gboolean native_ready (GIOChannel *channel, GIOCondition condition, gpointer data)
  {
  ViewportScroll *state = data ;
  if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
    {
    state->watch = 0 ;
    return FALSE ;
    }
  drain_native (state) ;
  return TRUE ;
  }

static void native_attach (ViewportScroll *state)
  {
  int event_base, error_base, major = 2, minor = 1 ;
  unsigned char bits[XIMaskLen (XI_LASTEVENT)] = {0} ;
  XIEventMask mask = {XIAllMasterDevices, sizeof (bits), bits} ;
  // A separate connection avoids stealing core pointer events from GTK2.
  gdk_display_sync (gtk_widget_get_display (state->widget)) ;
  state->display = XOpenDisplay (DisplayString (GDK_WINDOW_XDISPLAY (state->widget->window))) ;
  if (state->display == NULL) return ;
  if (!XQueryExtension (state->display, "XInputExtension", &state->opcode, &event_base, &error_base) ||
      XIQueryVersion (state->display, &major, &minor) != Success ||
      major < 2 || (major == 2 && minor < 1))
    {
    XCloseDisplay (state->display) ;
    state->display = NULL ;
    return ;
    }
  state->devices = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free) ;
  cache_devices (state) ;
  XISetMask (bits, XI_Motion) ;
  XISetMask (bits, XI_Enter) ;
  XISetMask (bits, XI_DeviceChanged) ;
  XISelectEvents (state->display, GDK_WINDOW_XID (state->widget->window), &mask, 1) ;
  memset (bits, 0, sizeof (bits)) ;
  XISetMask (bits, XI_HierarchyChanged) ;
  mask.deviceid = XIAllDevices ;
  XISelectEvents (state->display, DefaultRootWindow (state->display), &mask, 1) ;
  XFlush (state->display) ;
  state->channel = g_io_channel_unix_new (ConnectionNumber (state->display)) ;
  state->watch = g_io_add_watch (state->channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, native_ready, state) ;
  }
#endif

static void scroll_unrealize (GtkWidget *widget, gpointer data)
  {
  ViewportScroll *state = data ;
  viewport_scroll_cancel (widget) ;
#if defined(HAVE_XINPUT_GESTURES) && defined(GDK_WINDOWING_X11)
  if (state->watch) g_source_remove (state->watch) ;
  if (state->channel) g_io_channel_unref (state->channel) ;
  if (state->devices) g_hash_table_destroy (state->devices) ;
  if (state->display) XCloseDisplay (state->display) ;
#endif
  g_signal_handlers_disconnect_by_func (widget, scroll_unrealize, state) ;
  g_object_set_data (G_OBJECT (widget), STATE_KEY, NULL) ;
  g_free (state) ;
  }

void viewport_scroll_attach (GtkWidget *widget, ViewportPanFunc apply, ViewportZoomFunc zoom, gpointer data)
  {
  ViewportScroll *state ;
  if (g_object_get_data (G_OBJECT (widget), STATE_KEY)) return ;
  state = g_new0 (ViewportScroll, 1) ;
  state->widget = widget ;
  state->apply = apply ;
  state->zoom = zoom ;
  state->data = data ;
  g_object_set_data (G_OBJECT (widget), STATE_KEY, state) ;
  g_signal_connect (widget, "unrealize", (GCallback)scroll_unrealize, state) ;
#if defined(HAVE_XINPUT_GESTURES) && defined(GDK_WINDOWING_X11)
  native_attach (state) ;
#endif
  }

gboolean viewport_scroll_handle (GtkWidget *widget, GdkEventScroll *event)
  {
  ViewportScroll *state = g_object_get_data (G_OBJECT (widget), STATE_KEY) ;
  double dx = 0, dy = 0 ;
#if defined(HAVE_XINPUT_GESTURES) && defined(GDK_WINDOWING_X11)
  unsigned int Nix ;
#endif
  if (NULL == state) return FALSE ;
  if (state->pinching) return TRUE ;
#if defined(HAVE_XINPUT_GESTURES) && defined(GDK_WINDOWING_X11)
  if (state->display != NULL)
    {
    // Ensure the native motion preceding a core wheel event is processed first.
    XSync (state->display, False) ;
    drain_native (state) ;
    for (Nix = 0 ; Nix < MIN (state->consumed_count, G_N_ELEMENTS (state->consumed)) ; Nix++)
      if (event->time == state->consumed[Nix]) return TRUE ;
    }
#endif
  switch (event->direction)
    {
    case GDK_SCROLL_UP: dy = SCROLL_PIXELS ; break ;
    case GDK_SCROLL_DOWN: dy = -SCROLL_PIXELS ; break ;
    case GDK_SCROLL_LEFT: dx = SCROLL_PIXELS ; break ;
    case GDK_SCROLL_RIGHT: dx = -SCROLL_PIXELS ; break ;
    default: return FALSE ;
    }
  if ((event->state & GDK_CONTROL_MASK) && dy != 0)
    {
    apply_zoom (state, dy, event->x, event->y) ;
    return TRUE ;
    }
  queue_wheel (state, dx, dy) ;
  return TRUE ;
  }
