/* Gromit -- a program for painting on the screen
 * Copyright (C) 2000 Simon Budig <Simon.Budig@unix-ag.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <glib.h>
#include <gdk/gdk.h>
#include <gdk/gdkinput.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>

#include "paint_cursor.xbm"
#include "paint_cursor_mask.xbm"
#include "erase_cursor.xbm"
#include "erase_cursor_mask.xbm"

#define GROMIT_MOUSE_EVENTS ( GDK_PROXIMITY_IN_MASK | \
                              GDK_PROXIMITY_OUT_MASK | \
                              GDK_BUTTON_MOTION_MASK | \
                              GDK_BUTTON_PRESS_MASK | \
                              GDK_BUTTON_RELEASE_MASK )

#define GROMIT_PAINT_AREA_EVENTS  ( GROMIT_MOUSE_EVENTS | GDK_EXPOSURE_MASK )

#define GROMIT_WINDOW_EVENTS ( GROMIT_PAINT_AREA_EVENTS )

/* Atoms used to control Gromit */
#define GA_CONTROL    gdk_atom_intern ("Gromit/control", FALSE)
#define GA_STATUS     gdk_atom_intern ("Gromit/status", FALSE)
#define GA_QUIT       gdk_atom_intern ("Gromit/quit", FALSE)
#define GA_ACTIVATE   gdk_atom_intern ("Gromit/activate", FALSE)
#define GA_DEACTIVATE gdk_atom_intern ("Gromit/deactivate", FALSE)
#define GA_TOGGLE     gdk_atom_intern ("Gromit/toggle", FALSE)
#define GA_VISIBILITY gdk_atom_intern ("Gromit/visibility", FALSE)
#define GA_CLEAR      gdk_atom_intern ("Gromit/clear", FALSE)


typedef enum
{
  GROMIT_PEN,
  GROMIT_ERASER,
  GROMIT_RECOLOR
} GromitPaintType;

typedef struct
{
  GromitPaintType type;
  guint           width;
  gfloat          arrowsize;
  GdkColor       *fg_color;
  GdkGC          *paint_gc;
  GdkGC          *shape_gc;
  gdouble         pressure;
} GromitPaintContext;

typedef struct
{
  gint x;
  gint y;
  gint width;
} GromitStrokeCoordinate;


typedef struct
{
  GtkWidget   *win;
  GtkWidget   *area;
  GtkWidget   *panel;
  GtkWidget   *button;

  GdkCursor   *paint_cursor;
  GdkCursor   *erase_cursor;
  GdkPixmap   *pixmap;
  GdkDisplay  *display;
  GdkScreen   *screen;
  gboolean     xinerama;
  GdkWindow   *root;
  gchar       *hot_keyval;
  guint        hot_keycode;

  GdkColormap *cm;
  GdkColor    *white;
  GdkColor    *black;
  GdkColor    *red;

  GromitPaintContext *default_pen;
  GromitPaintContext *default_eraser;
  GromitPaintContext *cur_context;

  GHashTable  *tool_config;

  GdkBitmap   *shape;
  GdkGC       *shape_gc;
  GdkGCValues *shape_gcv;
  GdkColor    *transparent;
  GdkColor    *opaque;

  gdouble      lastx;
  gdouble      lasty;
  guint32      motion_time;
  GList       *coordlist;

  GdkDevice   *device;
  guint        state;

  guint        timeout_id;
  guint        modified;
  guint        delayed;
  guint        maxwidth;
  guint        width;
  guint        height;
  guint        hard_grab;
  guint        client;
  guint        painted;
  guint        hidden;
} GromitData;


/* I need a prototype...  */
void gromit_release_grab (GromitData *data);
void gromit_acquire_grab (GromitData *data);

GromitPaintContext *
gromit_paint_context_new (GromitData *data, GromitPaintType type,
                          GdkColor *fg_color, guint width, guint arrowsize)
{
  GromitPaintContext *context;
  GdkGCValues   shape_gcv;

  context = g_malloc (sizeof (GromitPaintContext));

  context->type = type;
  context->width = width;
  context->arrowsize = arrowsize;
  context->fg_color = fg_color;

  if (type == GROMIT_ERASER)
    {
      context->paint_gc = NULL;
    }
  else
    {
      /* GROMIT_PEN || GROMIT_RECOLOR */
      context->paint_gc = gdk_gc_new (data->pixmap);
      gdk_gc_set_foreground (context->paint_gc, fg_color);
      gdk_gc_set_line_attributes (context->paint_gc, width, GDK_LINE_SOLID,
                                  GDK_CAP_ROUND, GDK_JOIN_ROUND);
    }

  if (type == GROMIT_RECOLOR)
    {
      context->shape_gc = NULL;
    }
  else
    {
      /* GROMIT_PEN || GROMIT_ERASER */
      context->shape_gc = gdk_gc_new (data->shape);
      gdk_gc_get_values (context->shape_gc, &shape_gcv);

      if (type == GROMIT_ERASER)
         gdk_gc_set_foreground (context->shape_gc, &(shape_gcv.foreground));
      else
         /* GROMIT_PEN */
         gdk_gc_set_foreground (context->shape_gc, &(shape_gcv.background));
      gdk_gc_set_line_attributes (context->shape_gc, width, GDK_LINE_SOLID,
                                  GDK_CAP_ROUND, GDK_JOIN_ROUND);
    }

  return context;
}


void
gromit_paint_context_print (gchar *name, GromitPaintContext *context)
{
  g_printerr ("Tool name: \"%-20s\": ", name);
  switch (context->type)
  {
    case GROMIT_PEN:
      g_printerr ("Pen,     "); break;
    case GROMIT_ERASER:
      g_printerr ("Eraser,  "); break;
    case GROMIT_RECOLOR:
      g_printerr ("Recolor, "); break;
    default:
      g_printerr ("UNKNOWN, "); break;
  }

  g_printerr ("width: %3d, ", context->width);
  g_printerr ("arrowsize: %.2f, ", context->arrowsize);
  g_printerr ("color: #%02X%02X%02X\n", context->fg_color->red >> 8,
              context->fg_color->green >> 8, context->fg_color->blue >> 8);
}


void
gromit_paint_context_free (GromitPaintContext *context)
{
  g_object_unref (context->paint_gc);
  g_object_unref (context->shape_gc);
  g_free (context);
}


void
gromit_coord_list_prepend (GromitData *data, gint x, gint y, gint width)
{
  GromitStrokeCoordinate *point;

  point = g_malloc (sizeof (GromitStrokeCoordinate));
  point->x = x;
  point->y = y;
  point->width = width;

  data->coordlist = g_list_prepend (data->coordlist, point);
}


void
gromit_coord_list_free (GromitData *data)
{
  GList *ptr;

  ptr = data->coordlist;

  while (ptr)
    {
      g_free (ptr->data);
      ptr = ptr->next;
    }

  g_list_free (data->coordlist);

  data->coordlist = NULL;
}


gboolean
gromit_coord_list_get_arrow_param (GromitData *data,
                                   gint        search_radius,
                                   gint       *ret_width,
                                   gfloat     *ret_direction)
{
  gint x0, y0, r2, dist;
  gboolean success = FALSE;
  GromitStrokeCoordinate  *cur_point, *valid_point;
  GList *ptr = data->coordlist;
  gfloat width;

  valid_point = NULL;

  if (ptr)
    {
      cur_point = ptr->data;
      x0 = cur_point->x;
      y0 = cur_point->y;
      r2 = search_radius * search_radius;
      dist = 0;

      while (ptr && dist < r2)
        {
          ptr = ptr->next;
          if (ptr)
            {
              cur_point = ptr->data;
              dist = (cur_point->x - x0) * (cur_point->x - x0) +
                     (cur_point->y - y0) * (cur_point->y - y0);
              width = cur_point->width * data->cur_context->arrowsize;
              if (width * 2 <= dist &&
                  (!valid_point || valid_point->width < cur_point->width))
                valid_point = cur_point;
            }
        }

      if (valid_point)
        {
          *ret_width = MAX (valid_point->width * data->cur_context->arrowsize,
                            2);
          *ret_direction = atan2 (y0 - valid_point->y, x0 - valid_point->x);
          success = TRUE;
        }
    }

  return success;
}


void
gromit_hide_window (GromitData *data)
{
  if (!data->hidden)
    {
      if (data->hard_grab)
        data->hidden = 2;
      else
        data->hidden = 1;
      gromit_release_grab (data);
      gtk_widget_hide (data->win);
    }
}


void
gromit_show_window (GromitData *data)
{
  gint oldstatus = data->hidden;

  if (data->hidden)
    {
      gtk_widget_show (data->win);
      data->hidden = 0;
      if (oldstatus == 2)
        gromit_acquire_grab (data);
    }
  gdk_window_raise (data->win->window);
}


void
gromit_toggle_visibility (GromitData *data)
{
  if (data->hidden)
    gromit_show_window (data);
  else
    gromit_hide_window (data);
}


void
gromit_release_grab (GromitData *data)
{
  if (data->hard_grab)
    {
      data->hard_grab = 0;
      gdk_display_pointer_ungrab (data->display, GDK_CURRENT_TIME);
      /* inherit cursor from root window */
      gdk_window_set_cursor (data->win->window, NULL);
    }

  if (!data->painted)
    gromit_hide_window (data);
}


void
gromit_acquire_grab (GromitData *data)
{
  GdkGrabStatus result;

  gromit_show_window (data);
  if (!data->hard_grab)
    {
      result = gdk_pointer_grab (data->area->window, FALSE,
                                 GROMIT_MOUSE_EVENTS, 0,
                                 NULL /* data->paint_cursor */,
                                 GDK_CURRENT_TIME);

      switch (result)
      {
        case GDK_GRAB_SUCCESS:
           data->hard_grab = 1;
           break;
        case GDK_GRAB_ALREADY_GRABBED:
           g_printerr ("Grabbing Pointer failed: %s\n", "AlreadyGrabbed");
           break;
        case GDK_GRAB_INVALID_TIME:
           g_printerr ("Grabbing Pointer failed: %s\n", "GrabInvalidTime");
           break;
        case GDK_GRAB_NOT_VIEWABLE:
           g_printerr ("Grabbing Pointer failed: %s\n", "GrabNotViewable");
           break;
        case GDK_GRAB_FROZEN:
           g_printerr ("Grabbing Pointer failed: %s\n", "GrabFrozen");
           break;
        default:
           g_printerr ("Grabbing Pointer failed: %s\n", "Unknown error");
      }

      if (data->cur_context->type == GROMIT_ERASER)
         gdk_window_set_cursor (data->win->window, data->erase_cursor);
      else
         gdk_window_set_cursor (data->win->window, data->paint_cursor);
    }
}


gint
reshape (gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;

  if (data->modified)
    {
      if (gtk_events_pending () && data->delayed < 5)
        {
          data->delayed++ ;
        }
      else
        {
          gtk_widget_shape_combine_mask (data->win, data->shape, 0,0);
          data->modified = 0;
          data->delayed = 0;
        }
    }
  return 1;
}


void
gromit_toggle_grab (GromitData *data)
{
  if (data->hard_grab) {
    gtk_timeout_remove (data->timeout_id);
    gromit_release_grab (data);
  } else {
    data->timeout_id = gtk_timeout_add (20, reshape, data);
    gromit_acquire_grab (data);
  }
}


void
gromit_clear_screen (GromitData *data)
{
  gdk_gc_set_foreground (data->shape_gc, data->transparent);
  gdk_draw_rectangle (data->shape, data->shape_gc, 1,
                      0, 0, data->width, data->height);
  gtk_widget_shape_combine_mask (data->win, data->shape, 0,0);
  if (!data->hard_grab)
    gromit_hide_window (data);
  data->painted = 0;
}


void
gromit_select_tool (GromitData *data, GdkDevice *device, guint state)
{
  guint buttons = 0, modifier = 0, len = 0;
  guint req_buttons = 0, req_modifier = 0;
  guint i, j, success = 0;
  GromitPaintContext *context = NULL;
  guchar *name;

  if (device)
    {
      len = strlen (device->name);
      name = g_strndup (device->name, len + 3);

      /* Extract Button/Modifiers from state (see GdkModifierType) */
      req_buttons = (state >> 8) & 31;

      req_modifier = (state >> 1) & 7;
      if (state & GDK_SHIFT_MASK) req_modifier |= 1;

      name [len] = 124;
      name [len+3] = 0;

      /*  0, 1, 3, 7, 15, 31 */
      context = NULL;
      i=-1;
      do
        {
          i++;
          buttons = req_buttons & ((1 << i)-1);
          j=-1;
          do
            {
              j++;
              modifier = req_modifier & ((1 << j)-1);
              name [len+1] = buttons + 64;
              name [len+2] = modifier + 48;
              context = g_hash_table_lookup (data->tool_config, name);
              if (context)
                {
                  data->cur_context = context;
                  success = 1;
                }
            }
          while (j<=3 && req_modifier >= (1 << j));
        }
      while (i<=5 && req_buttons >= (1 << i));

      g_free (name);

      if (!success)
        {
          if (device->source == GDK_SOURCE_ERASER)
            data->cur_context = data->default_eraser;
          else
            data->cur_context = data->default_pen;
        }
    }
  else
    {
      g_printerr ("ERROR: Attempt to select nonexistent device!\n");
      data->cur_context = data->default_pen;
    }

  if (data->cur_context->type == GROMIT_ERASER)
    gdk_window_set_cursor (data->win->window, data->erase_cursor);
  else
    gdk_window_set_cursor (data->win->window, data->paint_cursor);

  data->state = state;
  data->device = device;
}


void
gromit_draw_line (GromitData *data, gint x1, gint y1,
                  gint x2, gint y2)
{
  GdkRectangle rect;

  rect.x = MIN (x1,x2) - data->maxwidth / 2;
  rect.y = MIN (y1,y2) - data->maxwidth / 2;
  rect.width = ABS (x1-x2) + data->maxwidth;
  rect.height = ABS (y1-y2) + data->maxwidth;

  if (data->cur_context->paint_gc)
    gdk_gc_set_line_attributes (data->cur_context->paint_gc,
                                data->maxwidth, GDK_LINE_SOLID,
                                GDK_CAP_ROUND, GDK_JOIN_ROUND);
  if (data->cur_context->shape_gc)
    gdk_gc_set_line_attributes (data->cur_context->shape_gc,
                                data->maxwidth, GDK_LINE_SOLID,
                                GDK_CAP_ROUND, GDK_JOIN_ROUND);

  if (data->cur_context->paint_gc)
    gdk_draw_line (data->pixmap, data->cur_context->paint_gc,
                   x1, y1, x2, y2);

  if (data->cur_context->shape_gc)
    {
      gdk_draw_line (data->shape, data->cur_context->shape_gc,
                     x1, y1, x2, y2);
      data->modified = 1;
    }

  if (data->cur_context->paint_gc)
     gtk_widget_draw (data->area, &rect);

  data->painted = 1;
}


void
gromit_draw_arrow (GromitData *data, gint x1, gint y1,
                   gint width, gfloat direction)
{
  GdkRectangle rect;
  GdkPoint arrowhead [4];

  width = width / 2;

  /* I doubt that calculating the boundary box more exact is very useful */
  rect.x = x1 - 4 * width - 1;
  rect.y = y1 - 4 * width - 1;
  rect.width = 8 * width + 2;
  rect.height = 8 * width + 2;

  arrowhead [0].x = x1 + 4 * width * cos (direction);
  arrowhead [0].y = y1 + 4 * width * sin (direction);

  arrowhead [1].x = x1 - 3 * width * cos (direction)
                       + 3 * width * sin (direction);
  arrowhead [1].y = y1 - 3 * width * cos (direction)
                       - 3 * width * sin (direction);

  arrowhead [2].x = x1 - 2 * width * cos (direction);
  arrowhead [2].y = y1 - 2 * width * sin (direction);

  arrowhead [3].x = x1 - 3 * width * cos (direction)
                       - 3 * width * sin (direction);
  arrowhead [3].y = y1 + 3 * width * cos (direction)
                       - 3 * width * sin (direction);

  if (data->cur_context->paint_gc)
    gdk_gc_set_line_attributes (data->cur_context->paint_gc,
                                0, GDK_LINE_SOLID,
                                GDK_CAP_ROUND, GDK_JOIN_ROUND);

  if (data->cur_context->shape_gc)
    gdk_gc_set_line_attributes (data->cur_context->shape_gc,
                                0, GDK_LINE_SOLID,
                                GDK_CAP_ROUND, GDK_JOIN_ROUND);

  if (data->cur_context->paint_gc)
    {
      gdk_draw_polygon (data->pixmap, data->cur_context->paint_gc,
                        TRUE, arrowhead, 4);
      gdk_gc_set_foreground (data->cur_context->paint_gc, data->black);
      gdk_draw_polygon (data->pixmap, data->cur_context->paint_gc,
                        FALSE, arrowhead, 4);
      gdk_gc_set_foreground (data->cur_context->paint_gc,
                             data->cur_context->fg_color);
    }

  if (data->cur_context->shape_gc)
    {
      gdk_draw_polygon (data->shape, data->cur_context->shape_gc,
                        TRUE, arrowhead, 4);
      gdk_draw_polygon (data->shape, data->cur_context->shape_gc,
                        FALSE, arrowhead, 4);
      data->modified = 1;
    }

  if (data->cur_context->paint_gc)
    gtk_widget_draw (data->area, &rect);

  data->painted = 1;
}


/*
 * Event-Handlers to perform the drawing
 */

gboolean
proximity_in (GtkWidget *win, GdkEventProximity *ev, gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;
  gint x, y;
  GdkModifierType state;

  gdk_window_get_pointer (data->win->window, &x, &y, &state);
  gromit_select_tool (data, ev->device, state);

  return TRUE;
}


gboolean
proximity_out (GtkWidget *win, GdkEventProximity *ev, gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;

  data->cur_context = data->default_pen;

  if (data->cur_context->type == GROMIT_ERASER)
    gdk_window_set_cursor (data->win->window, data->erase_cursor);
  else
    gdk_window_set_cursor (data->win->window, data->paint_cursor);

  data->state = 0;
  data->device = NULL;

  return FALSE;
}


gboolean
paint (GtkWidget *win, GdkEventButton *ev, gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;
  gdouble pressure = 0.5;

  if (!data->hard_grab)
    return FALSE;

  /* See GdkModifierType. Am I fixing a Gtk misbehaviour???  */
  ev->state |= 1 << (ev->button + 7);
  if (ev->state != data->state || ev->device != data->device)
    gromit_select_tool (data, ev->device, ev->state);

  gdk_window_set_background (data->area->window,
                             data->cur_context->fg_color);

  data->lastx = ev->x;
  data->lasty = ev->y;
  data->motion_time = ev->time + 1;

  if (ev->device->source == GDK_SOURCE_MOUSE)
    {
      data->maxwidth = data->cur_context->width;
    }
  else
    {
      gdk_event_get_axis ((GdkEvent *) ev, GDK_AXIS_PRESSURE, &pressure);
      data->maxwidth = (CLAMP (pressure * pressure,0,1) *
                        (double) data->cur_context->width);
    }
  if (ev->button <= 5)
     gromit_draw_line (data, ev->x, ev->y, ev->x, ev->y);

  gromit_coord_list_prepend (data, ev->x, ev->y, data->maxwidth);

  /* if (data->cur_context->shape_gc && !gtk_events_pending ())
     gtk_widget_shape_combine_mask (data->win, data->shape, 0,0); */

  return TRUE;
}


gboolean
paintto (GtkWidget *win,
         GdkEventMotion *ev,
         gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;
  GdkTimeCoord **coords = NULL;
  int nevents;
  int i;
  gboolean ret;
  gdouble pressure = 0.5;

  if (!data->hard_grab)
    return FALSE;

  if (ev->state != data->state || ev->device != data->device)
     gromit_select_tool (data, ev->device, ev->state);

  ret = gdk_device_get_history (ev->device, ev->window,
                                data->motion_time, ev->time - 1,
                                &coords, &nevents);

  /* g_printerr ("Got %d coords\n", nevents); */
  if (!data->xinerama && nevents > 0)
    {
      for (i=0; i < nevents; i++)
        {
          gdouble x, y;

          gdk_device_get_axis (ev->device, coords[i]->axes,
                               GDK_AXIS_PRESSURE, &pressure);
          if (pressure > 0)
            {
              if (ev->device->source == GDK_SOURCE_MOUSE)
                data->maxwidth = data->cur_context->width;
              else
                data->maxwidth = (CLAMP (pressure * pressure, 0, 1) *
                                  (double) data->cur_context->width);

              gdk_device_get_axis(ev->device, coords[i]->axes,
                                  GDK_AXIS_X, &x);
              gdk_device_get_axis(ev->device, coords[i]->axes,
                                  GDK_AXIS_Y, &y);

              gromit_draw_line (data, data->lastx, data->lasty, x, y);

              gromit_coord_list_prepend (data, x, y, data->maxwidth);
              data->lastx = x;
              data->lasty = y;
            }
        }

      data->motion_time = coords[nevents-1]->time;
      g_free (coords);
    }

  /* always paint to the current event coordinate. */
  gdk_event_get_axis ((GdkEvent *) ev, GDK_AXIS_PRESSURE, &pressure);

  if (pressure > 0)
    {
      if (ev->device->source == GDK_SOURCE_MOUSE)
         data->maxwidth = data->cur_context->width;
      else
         data->maxwidth = (CLAMP (pressure * pressure,0,1) *
                           (double) data->cur_context->width);
      gromit_draw_line (data, data->lastx, data->lasty, ev->x, ev->y);

      gromit_coord_list_prepend (data, ev->x, ev->y, data->maxwidth);
    }

  data->lastx = ev->x;
  data->lasty = ev->y;

  return TRUE;
}


gboolean
paintend (GtkWidget *win, GdkEventButton *ev, gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;
  gint width = data->cur_context->arrowsize * data->cur_context->width / 2;
  gfloat direction = 0;

  if ((ev->x != data->lastx) ||
      (ev->y != data->lasty))
     paintto (win, (GdkEventMotion *) ev, user_data);

  if (!data->hard_grab)
    return FALSE;

  if (data->cur_context->arrowsize != 0 &&
      gromit_coord_list_get_arrow_param (data, width * 3,
                                         &width, &direction))
    gromit_draw_arrow (data, ev->x, ev->y, width, direction);

  gromit_coord_list_free (data);

  return TRUE;
}


/*
 * Functions for handling various (GTK+)-Events
 */

void
quiet_print_handler (const gchar *string)
{
  return;
}


gboolean
event_configure (GtkWidget *widget,
                 GdkEventExpose *event,
                 gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;

  data->pixmap = gdk_pixmap_new (data->area->window, data->width,
                                 data->height, -1);
  gdk_draw_rectangle (data->pixmap, data->area->style->black_gc,
                      1, 0, 0, data->width, data->height);
  gdk_window_set_transient_for (data->area->window, data->win->window);

  return TRUE;
}


gboolean
event_expose (GtkWidget *widget,
              GdkEventExpose *event,
              gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;

  gdk_draw_drawable (data->area->window,
                     data->area->style->fg_gc[GTK_WIDGET_STATE (data->area)],
                     data->pixmap,
                     event->area.x, event->area.y,
                     event->area.x, event->area.y,
                     event->area.width, event->area.height);
  return TRUE;
}


/* Keyboard control */

gint
key_press_event (GtkWidget   *grab_widget,
                 GdkEventKey *event,
                 gpointer     func_data)
{
  GromitData *data = (GromitData *) func_data;

  if (event->type == GDK_KEY_PRESS &&
      event->hardware_keycode == data->hot_keycode)
    {
      if (event->state & GDK_SHIFT_MASK)
        gromit_clear_screen (data);
      else if (event->state & GDK_CONTROL_MASK)
        gromit_toggle_visibility (data);
      else if (event->state & GDK_MOD1_MASK)
        gtk_main_quit ();
      else
        gromit_toggle_grab (data);

      return TRUE;
    }
  return FALSE;
}


void
gromit_main_do_event (GdkEventAny *event,
                      GromitData  *data)
{
  if ((event->type == GDK_KEY_PRESS ||
       event->type == GDK_KEY_RELEASE) &&
      event->window == data->root &&
      ((GdkEventKey *) event)->hardware_keycode == data->hot_keycode)
    {
      /* redirect the event to our main window, so that GTK+ doesn't
       * throw it away (there is no GtkWidget for the root window...)
       */
      event->window = data->win->window;
      g_object_ref (data->win->window);
    }

  gtk_main_do_event ((GdkEvent *) event);
}

/* Remote control */

void
event_selection_get (GtkWidget          *widget,
                     GtkSelectionData   *selection_data,
                     guint               info,
                     guint               time,
                     gpointer            data)
{
  gchar *uri = "OK";

  if (selection_data->target == GA_TOGGLE)
    gromit_toggle_grab (data);
  else if (selection_data->target == GA_VISIBILITY)
    gromit_toggle_visibility (data);
  else if (selection_data->target == GA_CLEAR)
    gromit_clear_screen (data);
  else if (selection_data->target == GA_QUIT)
    gtk_main_quit ();
  else
    uri = "NOK";

  gtk_selection_data_set (selection_data,
                          selection_data->target,
                          8, uri, strlen (uri));
}


void
event_selection_received (GtkWidget *widget,
                          GtkSelectionData *selection_data,
                          guint time,
                          gpointer user_data)
{
  GromitData *data = (GromitData *) user_data;

  /* If someone has a selection for us, Gromit is already running. */

  if (selection_data->type == GDK_NONE)
    data->client = 0;
  else
    data->client = 1;

  gtk_main_quit ();
}


/*
 * Functions for parsing the Configuration-file
 */

gchar *
parse_name (GScanner *scanner)
{
  GTokenType token;

  guint buttons = 0;
  guint modifier = 0;
  guint len = 0;
  gchar *name;

  token = g_scanner_cur_token(scanner);

  if (token != G_TOKEN_STRING)
    {
      g_scanner_unexp_token (scanner, G_TOKEN_STRING, NULL,
                             NULL, NULL, "aborting", TRUE);
      exit (1);
    }

  len = strlen (scanner->value.v_string);
  name = g_strndup (scanner->value.v_string, len + 3);

  token = g_scanner_get_next_token (scanner);

  /*
   * Are there any options to limit the scope of the definition?
   */

  if (token == G_TOKEN_LEFT_BRACE)
    {
      g_scanner_set_scope (scanner, 1);
      scanner->config->int_2_float = 0;
      modifier = buttons = 0;
      while ((token = g_scanner_get_next_token (scanner))
             != G_TOKEN_RIGHT_BRACE)
        {
          if (token == G_TOKEN_SYMBOL)
            {
              if ((guint) scanner->value.v_symbol < 11)
                 buttons |= 1 << ((guint) scanner->value.v_symbol - 1);
              else
                 modifier |= 1 << ((guint) scanner->value.v_symbol - 11);
            }
          else if (token == G_TOKEN_INT)
            {
              if (scanner->value.v_int <= 5 && scanner->value.v_int > 0)
                buttons |= 1 << (scanner->value.v_int - 1);
              else
                g_printerr ("Only Buttons 1-5 are supported!\n");
            }
          else
            {
              g_printerr ("skipped token\n");
            }
        }
      g_scanner_set_scope (scanner, 0);
      scanner->config->int_2_float = 1;
      token = g_scanner_get_next_token (scanner);
    }

  name [len] = 124;
  name [len+1] = buttons + 64;
  name [len+2] = modifier + 48;
  name [len+3] = 0;

  return name;
}

void
parse_config (GromitData *data)
{
  GromitPaintContext *context=NULL;
  GromitPaintContext *context_template=NULL;
  GScanner *scanner;
  GTokenType token;
  gchar *filename;
  int file;

  gchar *name, *copy;

  GromitPaintType type;
  GdkColor *fg_color=NULL;
  guint width, arrowsize;

  filename = g_strjoin (G_DIR_SEPARATOR_S,
                        g_get_home_dir(), ".gromitrc", NULL);
  file = open (filename, O_RDONLY);

  if (file < 0)
    {
      /* try global config file */
      g_free (filename);
      filename = g_strdup ("/etc/gromit/gromitrc");
      file = open (filename, O_RDONLY);

      if (file < 0)
        {
          g_printerr ("Could not open %s: %s\n", filename, g_strerror (errno));
          g_free (filename);
          return;
        }
    }

  scanner = g_scanner_new (NULL);
  scanner->input_name = filename;
  scanner->config->case_sensitive = 0;
  scanner->config->scan_octal = 0;
  scanner->config->identifier_2_string = 0;
  scanner->config->char_2_token = 1;
  scanner->config->numbers_2_int = 1;
  scanner->config->int_2_float = 1;

  g_scanner_scope_add_symbol (scanner, 0, "PEN",    (gpointer) GROMIT_PEN);
  g_scanner_scope_add_symbol (scanner, 0, "ERASER", (gpointer) GROMIT_ERASER);
  g_scanner_scope_add_symbol (scanner, 0, "RECOLOR",(gpointer) GROMIT_RECOLOR);

  g_scanner_scope_add_symbol (scanner, 1, "BUTTON1", (gpointer) 1);
  g_scanner_scope_add_symbol (scanner, 1, "BUTTON2", (gpointer) 2);
  g_scanner_scope_add_symbol (scanner, 1, "BUTTON3", (gpointer) 3);
  g_scanner_scope_add_symbol (scanner, 1, "BUTTON4", (gpointer) 4);
  g_scanner_scope_add_symbol (scanner, 1, "BUTTON5", (gpointer) 5);
  g_scanner_scope_add_symbol (scanner, 1, "SHIFT",   (gpointer) 11);
  g_scanner_scope_add_symbol (scanner, 1, "CONTROL", (gpointer) 12);
  g_scanner_scope_add_symbol (scanner, 1, "META",    (gpointer) 13);
  g_scanner_scope_add_symbol (scanner, 1, "ALT",     (gpointer) 13);

  g_scanner_scope_add_symbol (scanner, 2, "size",      (gpointer) 1);
  g_scanner_scope_add_symbol (scanner, 2, "color",     (gpointer) 2);
  g_scanner_scope_add_symbol (scanner, 2, "arrowsize", (gpointer) 3);

  g_scanner_set_scope (scanner, 0);
  scanner->config->scope_0_fallback = 0;

  g_scanner_input_file (scanner, file);

  token = g_scanner_get_next_token (scanner);
  while (token != G_TOKEN_EOF)
    {

      /*
       * New tool definition
       */

      if (token == G_TOKEN_STRING)
        {
          name = parse_name (scanner);
          token = g_scanner_cur_token(scanner);

          if (token != G_TOKEN_EQUAL_SIGN)
            {
              g_scanner_unexp_token (scanner, G_TOKEN_EQUAL_SIGN, NULL,
                                     NULL, NULL, "aborting", TRUE);
              exit (1);
            }

          token = g_scanner_get_next_token (scanner);

          /* defaults */

          type = GROMIT_PEN;
          width = 7;
          arrowsize = 0;
          fg_color = data->red;

          if (token == G_TOKEN_SYMBOL)
            {
              type = (GromitPaintType) scanner->value.v_symbol;
              token = g_scanner_get_next_token (scanner);
            }
          else if (token == G_TOKEN_STRING)
            {
              copy = parse_name (scanner);
              token = g_scanner_cur_token(scanner);
              context_template = g_hash_table_lookup (data->tool_config, copy);
              if (context_template)
                {
                  type = context_template->type;
                  width = context_template->width;
                  arrowsize = context_template->arrowsize;
                  fg_color = context_template->fg_color;
                }
              else
                {
                  g_printerr ("WARNING: Unable to copy \"%s\": "
                              "not yet defined!\n", copy);
                }
            }
          else
            {
              g_printerr ("Expected Tool-definition "
                          "or name of template tool\n");
              exit (1);
            }

          /* Are there any tool-options?
           */

          if (token == G_TOKEN_LEFT_PAREN)
            {
              GdkColor *color = NULL;
              g_scanner_set_scope (scanner, 2);
              scanner->config->int_2_float = 1;
              token = g_scanner_get_next_token (scanner);
              while (token != G_TOKEN_RIGHT_PAREN)
                {
                  if (token == G_TOKEN_SYMBOL)
                    {
                      if ((guint) scanner->value.v_symbol == 1)
                        {
                          token = g_scanner_get_next_token (scanner);
                          if (token != G_TOKEN_EQUAL_SIGN)
                            {
                              g_printerr ("Missing \"=\"... aborting\n");
                              exit (1);
                            }
                          token = g_scanner_get_next_token (scanner);
                          if (token != G_TOKEN_FLOAT)
                            {
                              g_printerr ("Missing Size (float)... aborting\n");
                              exit (1);
                            }
                          width = (guint) (scanner->value.v_float + 0.5);
                        }
                      else if ((guint) scanner->value.v_symbol == 2)
                        {
                          token = g_scanner_get_next_token (scanner);
                          if (token != G_TOKEN_EQUAL_SIGN)
                            {
                              g_printerr ("Missing \"=\"... aborting\n");
                              exit (1);
                            }
                          token = g_scanner_get_next_token (scanner);
                          if (token != G_TOKEN_STRING)
                            {
                              g_printerr ("Missing Color (string)... "
                                          "aborting\n");
                              exit (1);
                            }
                          color = g_malloc (sizeof (GdkColor));
                          if (gdk_color_parse (scanner->value.v_string,
                                               color))
                            {
                              if (gdk_colormap_alloc_color (data->cm,
                                                            color, 0, 1))
                                {
                                  fg_color = color;
                                }
                              else
                                {
                                  g_printerr ("Unable to allocate color. "
                                              "Keeping default!\n");
                                  g_free (color);
                                }
                            }
                          else
                            {
                              g_printerr ("Unable to parse color. "
                                          "Keeping default.\n");
                              g_free (color);
                            }
                          color = NULL;
                        }
                      else if ((guint) scanner->value.v_symbol == 3)
                        {
                          token = g_scanner_get_next_token (scanner);
                          if (token != G_TOKEN_EQUAL_SIGN)
                            {
                              g_printerr ("Missing \"=\"... aborting\n");
                              exit (1);
                            }
                          token = g_scanner_get_next_token (scanner);
                          if (token != G_TOKEN_FLOAT)
                            {
                              g_printerr ("Missing Arrowsize (float)... "
                                          "aborting\n");
                              exit (1);
                            }
                          arrowsize = scanner->value.v_float;
                        }
                      else
                        {
                          g_printerr ("Unknown tool type?????\n");
                        }
                    }
                  else
                    {
                      g_printerr ("skipped token!!!\n");
                    }
                  token = g_scanner_get_next_token (scanner);
                }
              g_scanner_set_scope (scanner, 0);
              token = g_scanner_get_next_token (scanner);
            }

          /*
           * Finally we expect a semicolon
           */

          if (token != ';')
            {
              g_printerr ("Expected \";\"\n");
              exit (1);
            }

          context = gromit_paint_context_new (data, type, fg_color, width, arrowsize);
          g_hash_table_insert (data->tool_config, name, context);
        }
      else
        {
          g_printerr ("Expected name of Tool to define\n");
          exit(1);
        }

      token = g_scanner_get_next_token (scanner);
    }
  g_scanner_destroy (scanner);
  close (file);
  g_free (filename);
}


/*
 * Functions for setting up (parts of) the application
 */

void
setup_input_devices (GromitData *data)
{
  GList     *tmp_list;
  GdkDevice *core_pointer;

  core_pointer = gdk_display_get_core_pointer (data->display);

  for (tmp_list = gdk_display_list_devices (data->display);
       tmp_list;
       tmp_list = tmp_list->next)
    {
      GdkDevice *device = (GdkDevice *) tmp_list->data;

      /* Guess "Eraser"-Type devices */
      if (strstr (device->name, "raser") ||
          strstr (device->name, "RASER"))
         gdk_device_set_source (device, GDK_SOURCE_ERASER);

      /* Dont touch devices with two or less axis - GDK apparently
       * gets confused...  */
      if (device->num_axes > 2)
        {
          g_printerr ("Enabling No. %p: \"%s\" (Type: %d)\n",
                      device, device->name, device->source);
          gdk_device_set_mode (device, GDK_MODE_SCREEN);
        }
    }
}


void
setup_client_app (GromitData *data)
{
  data->display = gdk_display_get_default ();
  data->screen = gdk_display_get_default_screen (data->display);
  data->xinerama = gdk_screen_get_n_monitors (data->screen) > 1;
  data->root = gdk_screen_get_root_window (data->screen);
  data->width = gdk_screen_get_width (data->screen);
  data->height = gdk_screen_get_height (data->screen);
  data->hard_grab = 0;

  data->win = gtk_window_new (GTK_WINDOW_POPUP);
  gtk_widget_set_usize (GTK_WIDGET (data->win), data->width, data->height);
  gtk_widget_set_uposition (GTK_WIDGET (data->win), 0, 0);

  gtk_widget_set_events (data->win, GROMIT_WINDOW_EVENTS);

  gtk_signal_connect (GTK_OBJECT (data->win), "delete-event",
                      GTK_SIGNAL_FUNC (gtk_main_quit), NULL);
  gtk_signal_connect (GTK_OBJECT (data->win), "destroy",
                      GTK_SIGNAL_FUNC (gtk_main_quit), NULL);
  gtk_signal_connect (GTK_OBJECT (data->win), "selection_get",
                      GTK_SIGNAL_FUNC (event_selection_get), (gpointer) data);
  gtk_signal_connect (GTK_OBJECT (data->win), "selection_received",
                      GTK_SIGNAL_FUNC (event_selection_received),
                      (gpointer) data);
}


void
setup_main_app (GromitData *data, gboolean activate)
{
  GdkPixmap *cursor_src, *cursor_mask;
  gboolean   have_key = FALSE;

  /* COLORMAP */
  data->cm = gdk_screen_get_default_colormap (data->screen);
  data->white = g_malloc (sizeof (GdkColor));
  data->black = g_malloc (sizeof (GdkColor));
  data->red   = g_malloc (sizeof (GdkColor));
  gdk_color_parse ("#FFFFFF", data->white);
  gdk_colormap_alloc_color (data->cm, data->white, FALSE, TRUE);
  gdk_color_parse ("#000000", data->black);
  gdk_colormap_alloc_color (data->cm, data->black, FALSE, TRUE);
  gdk_color_parse ("#FF0000", data->red);
  gdk_colormap_alloc_color (data->cm, data->red,  FALSE, TRUE);

  /* CURSORS */
  cursor_src = gdk_bitmap_create_from_data (NULL, paint_cursor_bits,
                                            paint_cursor_width,
                                            paint_cursor_height);
  cursor_mask = gdk_bitmap_create_from_data (NULL, paint_cursor_mask_bits,
                                             paint_cursor_width,
                                             paint_cursor_height);
  data->paint_cursor = gdk_cursor_new_from_pixmap (cursor_src, cursor_mask,
                                                   data->white, data->black,
                                                   paint_cursor_x_hot,
                                                   paint_cursor_y_hot);
  g_object_unref (cursor_src);
  g_object_unref (cursor_mask);

  cursor_src = gdk_bitmap_create_from_data (NULL, erase_cursor_bits,
                                            erase_cursor_width,
                                            erase_cursor_height);
  cursor_mask = gdk_bitmap_create_from_data (NULL, erase_cursor_mask_bits,
                                             erase_cursor_width,
                                             erase_cursor_height);
  data->erase_cursor = gdk_cursor_new_from_pixmap (cursor_src, cursor_mask,
                                                   data->white, data->black,
                                                   erase_cursor_x_hot,
                                                   erase_cursor_y_hot);
  g_object_unref (cursor_src);
  g_object_unref (cursor_mask);

  gdk_window_set_cursor (data->win->window, data->paint_cursor);

  /* SHAPE PIXMAP */
  data->shape = gdk_pixmap_new (NULL, data->width, data->height, 1);
  data->shape_gc = gdk_gc_new (data->shape);
  data->shape_gcv = g_malloc (sizeof (GdkGCValues));
  gdk_gc_get_values (data->shape_gc, data->shape_gcv);
  data->transparent = gdk_color_copy (&(data->shape_gcv->foreground));
  data->opaque = gdk_color_copy (&(data->shape_gcv->background));
  gdk_gc_set_foreground (data->shape_gc, data->transparent);
  gdk_draw_rectangle (data->shape, data->shape_gc,
                      1, 0, 0, data->width, data->height);

  /* DRAWING AREA */
  data->area = gtk_drawing_area_new ();
  gtk_drawing_area_size (GTK_DRAWING_AREA (data->area),
                         data->width, data->height);

  gtk_widget_set_events (data->area, GROMIT_PAINT_AREA_EVENTS);
  gtk_signal_connect (GTK_OBJECT (data->area), "expose_event",
                      (GtkSignalFunc) event_expose, (gpointer) data);
  gtk_signal_connect (GTK_OBJECT (data->area),"configure_event",
                      (GtkSignalFunc) event_configure, (gpointer) data);
  gtk_signal_connect (GTK_OBJECT (data->win), "motion_notify_event",
                      GTK_SIGNAL_FUNC (paintto), (gpointer) data);
  gtk_signal_connect (GTK_OBJECT (data->win), "button_press_event",
                      GTK_SIGNAL_FUNC (paint), (gpointer) data);
  gtk_signal_connect (GTK_OBJECT (data->win), "button_release_event",
                      GTK_SIGNAL_FUNC (paintend), (gpointer) data);
  gtk_signal_connect (GTK_OBJECT (data->win), "proximity_in_event",
                      GTK_SIGNAL_FUNC (proximity_in), (gpointer) data);
  gtk_signal_connect (GTK_OBJECT (data->win), "proximity_out_event",
                      GTK_SIGNAL_FUNC (proximity_out), (gpointer) data);
  gtk_widget_set_extension_events (data->area, GDK_EXTENSION_EVENTS_ALL);

  gtk_container_add (GTK_CONTAINER (data->win), data->area);

  gtk_widget_shape_combine_mask (data->win, data->shape, 0,0);

  gtk_widget_show_all (data->area);

  gtk_widget_realize (data->win);

  data->painted = 0;
  gromit_hide_window (data);

  /* data->timeout_id = gtk_timeout_add (20, reshape, data); */
  data->coordlist = NULL;
  data->modified = 0;

  data->default_pen = gromit_paint_context_new (data, GROMIT_PEN,
                                                data->red, 7, 0);
  data->default_eraser = gromit_paint_context_new (data, GROMIT_ERASER,
                                                   data->red, 75, 0);

  data->cur_context = data->default_pen;

  /*
   * Parse Config file
   */

  data->tool_config = g_hash_table_new (g_str_hash, g_str_equal);
  parse_config (data);
  data->state = 0;

  gtk_selection_owner_set (data->win, GA_CONTROL, GDK_CURRENT_TIME);

  gtk_selection_add_target (data->win, GA_CONTROL, GA_STATUS, 0);
  gtk_selection_add_target (data->win, GA_CONTROL, GA_QUIT, 1);
  gtk_selection_add_target (data->win, GA_CONTROL, GA_ACTIVATE, 2);
  gtk_selection_add_target (data->win, GA_CONTROL, GA_DEACTIVATE, 3);
  gtk_selection_add_target (data->win, GA_CONTROL, GA_TOGGLE, 4);
  gtk_selection_add_target (data->win, GA_CONTROL, GA_VISIBILITY, 5);
  gtk_selection_add_target (data->win, GA_CONTROL, GA_CLEAR, 6);

  setup_input_devices (data);

  /* Grab the GROMIT_HOTKEY */

  if (data->hot_keyval)
    {
      GdkKeymap    *keymap;
      GdkKeymapKey *keys;
      gint          n_keys;
      guint         keyval;

      if (strlen (data->hot_keyval) > 0 &&
          strcasecmp (data->hot_keyval, "none") != 0)
        {
          keymap = gdk_keymap_get_for_display (data->display);
          keyval = gdk_keyval_from_name (data->hot_keyval);

          if (!keyval || !gdk_keymap_get_entries_for_keyval (keymap, keyval,
                                                             &keys, &n_keys))
            {
              g_printerr ("cannot find the key \"%s\"\n", data->hot_keyval);
              exit (1);
            }

          have_key = TRUE;
          data->hot_keycode = keys[0].keycode;
          g_free (keys);
        }
    }

  if (have_key)
    {
      if (data->hot_keycode)
        {
          gdk_error_trap_push ();

          XGrabKey (GDK_DISPLAY_XDISPLAY (data->display),
                    data->hot_keycode,
                    AnyModifier,
                    GDK_WINDOW_XWINDOW (data->root),
                    TRUE,
                    GrabModeAsync,
                    GrabModeAsync);

          gdk_flush ();

          if (gdk_error_trap_pop ())
            {
              g_printerr ("could not grab Hotkey. Aborting...\n");
              exit (1);
            }
        }
      else
        {
          g_printerr ("cannot find the key #%d\n", data->hot_keycode);
          exit (1);
        }
    }

  gdk_event_handler_set ((GdkEventFunc) gromit_main_do_event, data, NULL);
  gtk_key_snooper_install (key_press_event, data);

  if (activate)
    gromit_acquire_grab (data);
}


void
parse_print_help (gpointer key, gpointer value, gpointer user_data)
{
  gromit_paint_context_print ((gchar *) key, (GromitPaintContext *) value);
}

int
app_parse_args (int argc, char **argv, GromitData *data)
{
   gint      i;
   gchar    *arg;
   gboolean  wrong_arg = FALSE;
   gboolean  activate = FALSE;
   gboolean  dump = FALSE;

   data->hot_keyval = "Pause";
   data->hot_keycode = 0;

   for (i=1; i < argc ; i++)
     {
       arg = argv[i];
       if (strcmp (arg, "-a") == 0 ||
           strcmp (arg, "--active") == 0)
         {
           activate = TRUE;
         }
       else if (strcmp (arg, "-d") == 0 ||
                strcmp (arg, "--debug") == 0)
         {
           dump = TRUE;
         }
       else if (strcmp (arg, "-k") == 0 ||
                strcmp (arg, "--key") == 0)
         {
           if (i+1 < argc)
             {
               data->hot_keyval = argv[i+1];
               data->hot_keycode = 0;
               i++;
             }
           else
             {
               g_printerr ("-k requires an Key-Name as argument\n");
               wrong_arg = TRUE;
             }
         }
       else if (strcmp (arg, "-K") == 0 ||
                strcmp (arg, "--keycode") == 0)
         {
           if (i+1 < argc && atoi (argv[i+1]) > 0)
             {
               data->hot_keyval = NULL;
               data->hot_keycode = atoi (argv[i+1]);
               i++;
             }
           else
             {
               g_printerr ("-K requires an keycode > 0 as argument\n");
               wrong_arg = TRUE;
             }
         }
       else
         {
           g_printerr ("Unknown Option for Gromit startup: \"%s\"\n", arg);
           wrong_arg = TRUE;
         }

       if (!wrong_arg)
         {
          if (dump)
            {
              g_printerr ("\n-----------------------------\n");
              g_hash_table_foreach (data->tool_config, parse_print_help, NULL);
              g_printerr ("-----------------------------\n\n");
            }
         }
       else
         {
           g_printerr ("Please see the Gromit manpage for the correct usage\n");
           exit (1);
         }
     }

   return activate;
}


/*
 * Main programs
 */

int
main_client (int argc, char **argv, GromitData *data)
{
   GdkAtom   action = GDK_NONE;
   gint      i;
   gchar    *arg;
   gboolean  wrong_arg = FALSE;

   for (i=1; i < argc ; i++)
     {
       arg = argv[i];
       action = GDK_NONE;
       if (strcmp (arg, "-t") == 0 ||
           strcmp (arg, "--toggle") == 0)
         {
           action = GA_TOGGLE;
         }
       else if (strcmp (arg, "-v") == 0 ||
                strcmp (arg, "--visibility") == 0)
         {
           action = GA_VISIBILITY;
         }
       else if (strcmp (arg, "-q") == 0 ||
                strcmp (arg, "--quit") == 0)
         {
           action = GA_QUIT;
         }
       else if (strcmp (arg, "-c") == 0 ||
                strcmp (arg, "--clear") == 0)
         {
           action = GA_CLEAR;
         }
       else
         {
           g_printerr ("Unknown Option to control a running Gromit process: \"%s\"\n", arg);
           wrong_arg = TRUE;
         }

       if (!wrong_arg && action != GDK_NONE)
         {
           gtk_selection_convert (data->win, GA_CONTROL,
                                  action, GDK_CURRENT_TIME);
           gtk_main ();  /* Wait for the response */
         }
       else
         {
           g_printerr ("Please see the Gromit manpage for the correct usage\n");
           return 1;
         }
     }

   return 0;
}

int
main (int argc, char **argv)
{
  GromitData *data;

  gtk_init (&argc, &argv);
  data = g_malloc (sizeof (GromitData));

  /* g_set_printerr_handler (quiet_print_handler); */

  setup_client_app (data);

  /* Try to get a status message. If there is a response gromit
   * is already acive.
   */

  gtk_selection_convert (data->win, GA_CONTROL, GA_STATUS,
                         GDK_CURRENT_TIME);
  gtk_main ();  /* Wait for the response */

  if (data->client)
    return main_client (argc, argv, data);

  /* Main application */
  setup_main_app (data, app_parse_args (argc, argv, data));
  gtk_main ();
  gdk_display_pointer_ungrab (data->display, GDK_CURRENT_TIME);
  gdk_cursor_unref (data->paint_cursor);
  gdk_cursor_unref (data->erase_cursor);
  g_free (data);
  return 0;
}

/*    vim: sw=3 ts=8 cindent noai bs=2 cinoptions=(0
 */
