/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* caja-icon-dnd.c - Drag & drop handling for the icon container widget.

   Copyright (C) 1999, 2000 Free Software Foundation
   Copyright (C) 2000 Eazel, Inc.

   The Mate Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Mate Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Mate Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
   Boston, MA 02110-1301, USA.

   Authors: Ettore Perazzoli <ettore@gnu.org>,
            Darin Adler <darin@bentspoon.com>,
	    Andy Hertzfeld <andy@eazel.com>
	    Pavel Cisler <pavel@eazel.com>

   XDS support: Benedikt Meurer <benny@xfce.org> (adapted by Amos Brocco <amos.brocco@unifr.ch>)

*/

#include <config.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include <eel/eel-background.h>
#include <eel/eel-gdk-pixbuf-extensions.h>
#include <eel/eel-glib-extensions.h>
#include <eel/eel-mate-extensions.h>
#include <eel/eel-graphic-effects.h>
#include <eel/eel-gtk-extensions.h>
#include <eel/eel-gtk-macros.h>
#include <eel/eel-stock-dialogs.h>
#include <eel/eel-string.h>
#include <eel/eel-vfs-extensions.h>
#include <eel/eel-canvas-rect-ellipse.h>

#include "caja-icon-dnd.h"
#include "caja-debug-log.h"
#include "caja-file-dnd.h"
#include "caja-icon-private.h"
#include "caja-link.h"
#include "caja-metadata.h"
#include "caja-file-utilities.h"
#include "caja-file-changes-queue.h"

static const GtkTargetEntry drag_types [] =
{
    { CAJA_ICON_DND_MATE_ICON_LIST_TYPE, 0, CAJA_ICON_DND_MATE_ICON_LIST },
    { CAJA_ICON_DND_URI_LIST_TYPE, 0, CAJA_ICON_DND_URI_LIST },
};

static const GtkTargetEntry drop_types [] =
{
    { CAJA_ICON_DND_MATE_ICON_LIST_TYPE, 0, CAJA_ICON_DND_MATE_ICON_LIST },
    /* prefer "_NETSCAPE_URL" over "text/uri-list" to satisfy web browsers. */
    { CAJA_ICON_DND_NETSCAPE_URL_TYPE, 0, CAJA_ICON_DND_NETSCAPE_URL },
    { CAJA_ICON_DND_URI_LIST_TYPE, 0, CAJA_ICON_DND_URI_LIST },
    { CAJA_ICON_DND_COLOR_TYPE, 0, CAJA_ICON_DND_COLOR },
    { CAJA_ICON_DND_BGIMAGE_TYPE, 0, CAJA_ICON_DND_BGIMAGE },
    { CAJA_ICON_DND_KEYWORD_TYPE, 0, CAJA_ICON_DND_KEYWORD },
    { CAJA_ICON_DND_RESET_BACKGROUND_TYPE,  0, CAJA_ICON_DND_RESET_BACKGROUND },
    { CAJA_ICON_DND_XDNDDIRECTSAVE_TYPE, 0, CAJA_ICON_DND_XDNDDIRECTSAVE }, /* XDS Protocol Type */
    { CAJA_ICON_DND_RAW_TYPE, 0, CAJA_ICON_DND_RAW },
    /* Must be last: */
    { CAJA_ICON_DND_ROOTWINDOW_DROP_TYPE,  0, CAJA_ICON_DND_ROOTWINDOW_DROP }
};
static void     stop_dnd_highlight         (GtkWidget      *widget);
static void     dnd_highlight_queue_redraw (GtkWidget      *widget);

static GtkTargetList *drop_types_list = NULL;
static GtkTargetList *drop_types_list_root = NULL;

static char * caja_icon_container_find_drop_target (CajaIconContainer *container,
        GdkDragContext *context,
        int x, int y, gboolean *icon_hit,
        gboolean rewrite_desktop);

static EelCanvasItem *
create_selection_shadow (CajaIconContainer *container,
                         GList *list)
{
    EelCanvasGroup *group;
    EelCanvas *canvas;
    int max_x, max_y;
    int min_x, min_y;
    GList *p;
    GtkAllocation allocation;

    if (list == NULL)
    {
        return NULL;
    }

    /* if we're only dragging a single item, don't worry about the shadow */
    if (list->next == NULL)
    {
        return NULL;
    }

    canvas = EEL_CANVAS (container);
    gtk_widget_get_allocation (GTK_WIDGET (container), &allocation);

    /* Creating a big set of rectangles in the canvas can be expensive, so
           we try to be smart and only create the maximum number of rectangles
           that we will need, in the vertical/horizontal directions.  */

    max_x = allocation.width;
    min_x = -max_x;

    max_y = allocation.height;
    min_y = -max_y;

    /* Create a group, so that it's easier to move all the items around at
           once.  */
    group = EEL_CANVAS_GROUP
            (eel_canvas_item_new (EEL_CANVAS_GROUP (canvas->root),
                                  eel_canvas_group_get_type (),
                                  NULL));

    for (p = list; p != NULL; p = p->next)
    {
        CajaDragSelectionItem *item;
        int x1, y1, x2, y2;
        GdkRGBA black = { 0, 0, 0, 1 };

        item = p->data;

        if (!item->got_icon_position)
        {
            continue;
        }

        x1 = item->icon_x;
        y1 = item->icon_y;
        x2 = x1 + item->icon_width;
        y2 = y1 + item->icon_height;

        if (x2 >= min_x && x1 <= max_x && y2 >= min_y && y1 <= max_y)
            eel_canvas_item_new
            (group,
             eel_canvas_rect_get_type (),
             "x1", (double) x1,
             "y1", (double) y1,
             "x2", (double) x2,
             "y2", (double) y2,
             "outline-color-rgba", &black,
             "outline-stippling", TRUE,
             "width_pixels", 1,
             NULL);
    }

    return EEL_CANVAS_ITEM (group);
}

/* Set the affine instead of the x and y position.
 * Simple, and setting x and y was broken at one point.
 */
static void
set_shadow_position (EelCanvasItem *shadow,
                     double x, double y)
{
    eel_canvas_item_set (shadow,
                         "x", x, "y", y,
                         NULL);
}

/* Source-side handling of the drag. */

/* iteration glue struct */
typedef struct
{
    gpointer iterator_context;
    CajaDragEachSelectedItemDataGet iteratee;
    gpointer iteratee_data;
} IconGetDataBinderContext;

static void
canvas_rect_world_to_widget (EelCanvas *canvas,
                             EelDRect *world_rect,
                             EelIRect *widget_rect)
{
    EelDRect window_rect;
    GtkAdjustment *hadj, *vadj;

    hadj = gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (canvas));
    vadj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (canvas));

    eel_canvas_world_to_window (canvas,
                                world_rect->x0, world_rect->y0,
                                &window_rect.x0, &window_rect.y0);
    eel_canvas_world_to_window (canvas,
                                world_rect->x1, world_rect->y1,
                                &window_rect.x1, &window_rect.y1);
    widget_rect->x0 = (int) window_rect.x0 - gtk_adjustment_get_value (hadj);
    widget_rect->y0 = (int) window_rect.y0 - gtk_adjustment_get_value (vadj);
    widget_rect->x1 = (int) window_rect.x1 - gtk_adjustment_get_value (hadj);
    widget_rect->y1 = (int) window_rect.y1 - gtk_adjustment_get_value (vadj);
}

static void
canvas_widget_to_world (EelCanvas *canvas,
                        double widget_x, double widget_y,
                        double *world_x, double *world_y)
{
    eel_canvas_window_to_world (canvas,
    			    widget_x + gtk_adjustment_get_value (gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (canvas))),
    			    widget_y + gtk_adjustment_get_value (gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (canvas))),
                                world_x, world_y);
}

static gboolean
icon_get_data_binder (CajaIcon *icon, gpointer data)
{
    IconGetDataBinderContext *context;
    EelDRect world_rect;
    EelIRect widget_rect;
    char *uri;
    CajaIconContainer *container;

    context = (IconGetDataBinderContext *)data;

    g_assert (CAJA_IS_ICON_CONTAINER (context->iterator_context));

    container = CAJA_ICON_CONTAINER (context->iterator_context);

    world_rect = caja_icon_canvas_item_get_icon_rectangle (icon->item);

    canvas_rect_world_to_widget (EEL_CANVAS (container), &world_rect, &widget_rect);

    uri = caja_icon_container_get_icon_uri (container, icon);
    if (uri == NULL)
    {
        g_warning ("no URI for one of the iterated icons");
        return TRUE;
    }

    widget_rect = eel_irect_offset_by (widget_rect,
                                       - container->details->dnd_info->drag_info.start_x,
                                       - container->details->dnd_info->drag_info.start_y);

    widget_rect = eel_irect_scale_by (widget_rect,
                                      1 / EEL_CANVAS (container)->pixels_per_unit);

    /* pass the uri, mouse-relative x/y and icon width/height */
    context->iteratee (uri,
                       (int) widget_rect.x0,
                       (int) widget_rect.y0,
                       widget_rect.x1 - widget_rect.x0,
                       widget_rect.y1 - widget_rect.y0,
                       context->iteratee_data);

    g_free (uri);

    return TRUE;
}

/* Iterate over each selected icon in a CajaIconContainer,
 * calling each_function on each.
 */
static void
caja_icon_container_each_selected_icon (CajaIconContainer *container,
                                        gboolean (*each_function) (CajaIcon *, gpointer), gpointer data)
{
    GList *p;
    CajaIcon *icon = NULL;

    for (p = container->details->icons; p != NULL; p = p->next)
    {
        icon = p->data;
        if (!icon->is_selected)
        {
            continue;
        }
        if (!each_function (icon, data))
        {
            return;
        }
    }
}

/* Adaptor function used with caja_icon_container_each_selected_icon
 * to help iterate over all selected items, passing uris, x, y, w and h
 * values to the iteratee
 */
static void
each_icon_get_data_binder (CajaDragEachSelectedItemDataGet iteratee,
                           gpointer iterator_context, gpointer data)
{
    IconGetDataBinderContext context;
    CajaIconContainer *container;

    g_assert (CAJA_IS_ICON_CONTAINER (iterator_context));
    container = CAJA_ICON_CONTAINER (iterator_context);

    context.iterator_context = iterator_context;
    context.iteratee = iteratee;
    context.iteratee_data = data;
    caja_icon_container_each_selected_icon (container, icon_get_data_binder, &context);
}

/* Called when the data for drag&drop is needed */
static void
drag_data_get_callback (GtkWidget *widget,
                        GdkDragContext *context,
                        GtkSelectionData *selection_data,
                        guint info,
                        guint32 time,
                        gpointer data)
{
    g_assert (widget != NULL);
    g_assert (CAJA_IS_ICON_CONTAINER (widget));
    g_return_if_fail (context != NULL);

    /* Call common function from caja-drag that set's up
     * the selection data in the right format. Pass it means to
     * iterate all the selected icons.
     */
    caja_drag_drag_data_get (widget, context, selection_data,
                             info, time, widget, each_icon_get_data_binder);
}

/* Target-side handling of the drag.  */

static void
caja_icon_container_position_shadow (CajaIconContainer *container,
                                     int x, int y)
{
    EelCanvasItem *shadow;
    double world_x, world_y;

    shadow = container->details->dnd_info->shadow;
    if (shadow == NULL)
    {
        return;
    }

    canvas_widget_to_world (EEL_CANVAS (container), x, y,
                            &world_x, &world_y);

    set_shadow_position (shadow, world_x, world_y);
    eel_canvas_item_show (shadow);
}

static void
caja_icon_container_dropped_icon_feedback (GtkWidget *widget,
        GtkSelectionData *data,
        int x, int y)
{
    CajaIconContainer *container;
    CajaIconDndInfo *dnd_info;

    container = CAJA_ICON_CONTAINER (widget);
    dnd_info = container->details->dnd_info;

    /* Delete old selection list. */
    caja_drag_destroy_selection_list (dnd_info->drag_info.selection_list);
    dnd_info->drag_info.selection_list = NULL;

    /* Delete old shadow if any. */
    if (dnd_info->shadow != NULL)
    {
        /* FIXME bugzilla.gnome.org 42484:
         * Is a destroy really sufficient here? Who does the unref? */
        eel_canvas_item_destroy (dnd_info->shadow);
    }

    /* Build the selection list and the shadow. */
    dnd_info->drag_info.selection_list = caja_drag_build_selection_list (data);
    dnd_info->shadow = create_selection_shadow (container, dnd_info->drag_info.selection_list);
    caja_icon_container_position_shadow (container, x, y);
}

static char *
get_direct_save_filename (GdkDragContext *context)
{
    guchar *prop_text;
    gint prop_len;

    if (!gdk_property_get (gdk_drag_context_get_source_window (context), gdk_atom_intern (CAJA_ICON_DND_XDNDDIRECTSAVE_TYPE, FALSE),
                           gdk_atom_intern ("text/plain", FALSE), 0, 1024, FALSE, NULL, NULL,
                           &prop_len, &prop_text))
    {
        return NULL;
    }

    /* Zero-terminate the string */
    prop_text = g_realloc (prop_text, prop_len + 1);
    prop_text[prop_len] = '\0';

    /* Verify that the file name provided by the source is valid */
    if (*prop_text == '\0' ||
            strchr ((const gchar *) prop_text, G_DIR_SEPARATOR) != NULL)
    {
        caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
                        "Invalid filename provided by XDS drag site");
        g_free (prop_text);
        return NULL;
    }

    return prop_text;
}

static void
set_direct_save_uri (GtkWidget *widget, GdkDragContext *context, CajaDragInfo *drag_info, int x, int y)
{
    char *filename, *drop_target;
    gchar *uri;

    drag_info->got_drop_data_type = TRUE;
    drag_info->data_type = CAJA_ICON_DND_XDNDDIRECTSAVE;

    uri = NULL;

    filename = get_direct_save_filename (context);
    drop_target = caja_icon_container_find_drop_target (CAJA_ICON_CONTAINER (widget),
                  context, x, y, NULL, TRUE);

    if (drop_target && eel_uri_is_trash (drop_target))
    {
        g_free (drop_target);
        drop_target = NULL; /* Cannot save to trash ...*/
    }

    if (filename != NULL && drop_target != NULL)
    {
        GFile *base, *child;

        /* Resolve relative path */
        base = g_file_new_for_uri (drop_target);
        child = g_file_get_child (base, filename);
        uri = g_file_get_uri (child);
        g_object_unref (base);
        g_object_unref (child);

        /* Change the uri property */
        gdk_property_change (gdk_drag_context_get_source_window (context),
                             gdk_atom_intern (CAJA_ICON_DND_XDNDDIRECTSAVE_TYPE, FALSE),
                             gdk_atom_intern ("text/plain", FALSE), 8,
                             GDK_PROP_MODE_REPLACE, (const guchar *) uri,
                             strlen (uri));

        drag_info->direct_save_uri = uri;
    }

    g_free (filename);
    g_free (drop_target);
}

/* FIXME bugzilla.gnome.org 47445: Needs to become a shared function */
static void
get_data_on_first_target_we_support (GtkWidget *widget, GdkDragContext *context, guint32 time, int x, int y)
{
    GtkTargetList *list;
    GdkAtom target;

    if (drop_types_list == NULL)
    {
        drop_types_list = gtk_target_list_new (drop_types,
                                               G_N_ELEMENTS (drop_types) - 1);
        gtk_target_list_add_text_targets (drop_types_list, CAJA_ICON_DND_TEXT);
    }
    if (drop_types_list_root == NULL)
    {
        drop_types_list_root = gtk_target_list_new (drop_types,
                               G_N_ELEMENTS (drop_types));
        gtk_target_list_add_text_targets (drop_types_list_root, CAJA_ICON_DND_TEXT);
    }

    if (caja_icon_container_get_is_desktop (CAJA_ICON_CONTAINER (widget)))
    {
        list = drop_types_list_root;
    }
    else
    {
        list = drop_types_list;
    }

    target = gtk_drag_dest_find_target (widget, context, list);
    if (target != GDK_NONE)
    {
        guint info;
        CajaDragInfo *drag_info;
        gboolean found;

        drag_info = &(CAJA_ICON_CONTAINER (widget)->details->dnd_info->drag_info);

        found = gtk_target_list_find (list, target, &info);
        g_assert (found);

        /* Don't get_data for destructive ops */
        if ((info == CAJA_ICON_DND_ROOTWINDOW_DROP ||
                info == CAJA_ICON_DND_XDNDDIRECTSAVE) &&
                !drag_info->drop_occured)
        {
            /* We can't call get_data here, because that would
               make the source execute the rootwin action or the direct save */
            drag_info->got_drop_data_type = TRUE;
            drag_info->data_type = info;
        }
        else
        {
            if (info == CAJA_ICON_DND_XDNDDIRECTSAVE)
            {
                set_direct_save_uri (widget, context, drag_info, x, y);
            }
            gtk_drag_get_data (GTK_WIDGET (widget), context,
                               target, time);
        }
    }
}

static void
caja_icon_container_ensure_drag_data (CajaIconContainer *container,
                                      GdkDragContext *context,
                                      guint32 time)
{
    CajaIconDndInfo *dnd_info;

    dnd_info = container->details->dnd_info;

    if (!dnd_info->drag_info.got_drop_data_type)
    {
        get_data_on_first_target_we_support (GTK_WIDGET (container), context, time, 0, 0);
    }
}

static void
drag_end_callback (GtkWidget *widget,
                   GdkDragContext *context,
                   gpointer data)
{
    CajaIconContainer *container;
    CajaIconDndInfo *dnd_info;

    container = CAJA_ICON_CONTAINER (widget);
    dnd_info = container->details->dnd_info;

    caja_drag_destroy_selection_list (dnd_info->drag_info.selection_list);
    dnd_info->drag_info.selection_list = NULL;
}

static CajaIcon *
caja_icon_container_item_at (CajaIconContainer *container,
                             int x, int y)
{
    GList *p;
    int size;
    EelDRect point;
    EelIRect canvas_point;

    /* build the hit-test rectangle. Base the size on the scale factor to ensure that it is
     * non-empty even at the smallest scale factor
     */

    size = MAX (1, 1 + (1 / EEL_CANVAS (container)->pixels_per_unit));
    point.x0 = x;
    point.y0 = y;
    point.x1 = x + size;
    point.y1 = y + size;

    for (p = container->details->icons; p != NULL; p = p->next)
    {
        CajaIcon *icon;
        icon = p->data;

        eel_canvas_w2c (EEL_CANVAS (container),
                        point.x0,
                        point.y0,
                        &canvas_point.x0,
                        &canvas_point.y0);
        eel_canvas_w2c (EEL_CANVAS (container),
                        point.x1,
                        point.y1,
                        &canvas_point.x1,
                        &canvas_point.y1);
        if (caja_icon_canvas_item_hit_test_rectangle (icon->item, canvas_point))
        {
            return icon;
        }
    }

    return NULL;
}

static char *
get_container_uri (CajaIconContainer *container)
{
    char *uri;

    /* get the URI associated with the container */
    uri = NULL;
    g_signal_emit_by_name (container, "get_container_uri", &uri);
    return uri;
}

static gboolean
caja_icon_container_selection_items_local (CajaIconContainer *container,
        GList *items)
{
    char *container_uri_string;
    gboolean result;

    /* must have at least one item */
    g_assert (items);

    /* get the URI associated with the container */
    container_uri_string = get_container_uri (container);

    if (eel_uri_is_desktop (container_uri_string))
    {
        result = caja_drag_items_on_desktop (items);
    }
    else
    {
        result = caja_drag_items_local (container_uri_string, items);
    }
    g_free (container_uri_string);

    return result;
}

static GdkDragAction
get_background_drag_action (CajaIconContainer *container,
                            GdkDragAction action)
{
    /* FIXME: This function is very FMDirectoryView specific, and
     * should be moved out of caja-icon-dnd.c */
    GdkDragAction valid_actions;

    if (action == GDK_ACTION_ASK)
    {
        valid_actions = CAJA_DND_ACTION_SET_AS_FOLDER_BACKGROUND;
        if (!eel_background_is_desktop (eel_get_widget_background (GTK_WIDGET (container))))
        {
            valid_actions |= CAJA_DND_ACTION_SET_AS_GLOBAL_BACKGROUND;
        }

        action = caja_drag_drop_background_ask
                 (GTK_WIDGET (container), valid_actions);
    }

    return action;
}

static void
receive_dropped_color (CajaIconContainer *container,
                       int x, int y,
                       GdkDragAction action,
                       GtkSelectionData *data)
{
    action = get_background_drag_action (container, action);

    if (action > 0)
    {
        char *uri;

        uri = get_container_uri (container);
        caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
                        "dropped color on icon container displaying %s", uri);
        g_free (uri);

        eel_background_set_dropped_color (eel_get_widget_background (GTK_WIDGET (container)),
        				  GTK_WIDGET (container),
        				  action, x, y, data);
    }
}

/* handle dropped tile images */
static void
receive_dropped_tile_image (CajaIconContainer *container, GdkDragAction action, GtkSelectionData *data)
{
    g_assert (data != NULL);

    action = get_background_drag_action (container, action);

    if (action > 0)
    {
        char *uri;

        uri = get_container_uri (container);
        caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
                        "dropped tile image on icon container displaying %s", uri);
        g_free (uri);

        eel_background_set_dropped_image (eel_get_widget_background (GTK_WIDGET (container)),
                                          action, gtk_selection_data_get_data (data));
    }
}

/* handle dropped keywords */
static void
receive_dropped_keyword (CajaIconContainer *container, const char *keyword, int x, int y)
{
    char *uri;
    double world_x, world_y;

    CajaIcon *drop_target_icon;
    CajaFile *file;

    g_assert (keyword != NULL);

    /* find the item we hit with our drop, if any */
    canvas_widget_to_world (EEL_CANVAS (container), x, y, &world_x, &world_y);
    drop_target_icon = caja_icon_container_item_at (container, world_x, world_y);
    if (drop_target_icon == NULL)
    {
        return;
    }

    /* FIXME bugzilla.gnome.org 42485:
     * This does not belong in the icon code.
     * It has to be in the file manager.
     * The icon code has no right to deal with the file directly.
     * But luckily there's no issue of not getting a file object,
     * so we don't have to worry about async. issues here.
     */
    uri = caja_icon_container_get_icon_uri (container, drop_target_icon);

    caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
                    "dropped emblem '%s' on icon container URI: %s",
                    keyword, uri);

    file = caja_file_get_by_uri (uri);
    g_free (uri);

    caja_drag_file_receive_dropped_keyword (file, keyword);

    caja_file_unref (file);
    caja_icon_container_update_icon (container, drop_target_icon);
}

/* handle dropped url */
static void
receive_dropped_netscape_url (CajaIconContainer *container, const char *encoded_url, GdkDragContext *context, int x, int y)
{
    char *drop_target;

    if (encoded_url == NULL)
    {
        return;
    }

    drop_target = caja_icon_container_find_drop_target (container, context, x, y, NULL, TRUE);

    g_signal_emit_by_name (container, "handle_netscape_url",
                           encoded_url,
                           drop_target,
                           gdk_drag_context_get_selected_action (context),
                           x, y);

    g_free (drop_target);
}

/* handle dropped uri list */
static void
receive_dropped_uri_list (CajaIconContainer *container, const char *uri_list, GdkDragContext *context, int x, int y)
{
    char *drop_target;

    if (uri_list == NULL)
    {
        return;
    }

    drop_target = caja_icon_container_find_drop_target (container, context, x, y, NULL, TRUE);

    g_signal_emit_by_name (container, "handle_uri_list",
                           uri_list,
                           drop_target,
                           gdk_drag_context_get_selected_action (context),
                           x, y);

    g_free (drop_target);
}

/* handle dropped text */
static void
receive_dropped_text (CajaIconContainer *container, const char *text, GdkDragContext *context, int x, int y)
{
    char *drop_target;

    if (text == NULL)
    {
        return;
    }

    drop_target = caja_icon_container_find_drop_target (container, context, x, y, NULL, TRUE);

    g_signal_emit_by_name (container, "handle_text",
                           text,
                           drop_target,
                           gdk_drag_context_get_selected_action (context),
                           x, y);

    g_free (drop_target);
}

/* handle dropped raw data */
static void
receive_dropped_raw (CajaIconContainer *container, const char *raw_data, int length, const char *direct_save_uri, GdkDragContext *context, int x, int y)
{
    char *drop_target;

    if (raw_data == NULL)
    {
        return;
    }

    drop_target = caja_icon_container_find_drop_target (container, context, x, y, NULL, TRUE);

    g_signal_emit_by_name (container, "handle_raw",
                           raw_data,
                           length,
                           drop_target,
                           direct_save_uri,
                           gdk_drag_context_get_selected_action (context),
                           x, y);

    g_free (drop_target);
}

static int
auto_scroll_timeout_callback (gpointer data)
{
    CajaIconContainer *container;
    GtkWidget *widget;
    float x_scroll_delta, y_scroll_delta;
    GdkRectangle exposed_area;
    GtkAllocation allocation;

    g_assert (CAJA_IS_ICON_CONTAINER (data));
    widget = GTK_WIDGET (data);
    container = CAJA_ICON_CONTAINER (widget);

    if (container->details->dnd_info->drag_info.waiting_to_autoscroll
            && container->details->dnd_info->drag_info.start_auto_scroll_in > g_get_monotonic_time())
    {
        /* not yet */
        return TRUE;
    }

    container->details->dnd_info->drag_info.waiting_to_autoscroll = FALSE;

    caja_drag_autoscroll_calculate_delta (widget, &x_scroll_delta, &y_scroll_delta);
    if (x_scroll_delta == 0 && y_scroll_delta == 0)
    {
        /* no work */
        return TRUE;
    }

    /* Clear the old dnd highlight frame */
    dnd_highlight_queue_redraw (widget);

    if (!caja_icon_container_scroll (container, (int)x_scroll_delta, (int)y_scroll_delta))
    {
        /* the scroll value got pinned to a min or max adjustment value,
         * we ended up not scrolling
         */
        return TRUE;
    }

    /* Make sure the dnd highlight frame is redrawn */
    dnd_highlight_queue_redraw (widget);

    /* update cached drag start offsets */
    container->details->dnd_info->drag_info.start_x -= x_scroll_delta;
    container->details->dnd_info->drag_info.start_y -= y_scroll_delta;

    /* Due to a glitch in GtkLayout, whe need to do an explicit draw of the exposed
     * area.
     * Calculate the size of the area we need to draw
     */
    gtk_widget_get_allocation (widget, &allocation);
    exposed_area.x = allocation.x;
    exposed_area.y = allocation.y;
    exposed_area.width = allocation.width;
    exposed_area.height = allocation.height;

    if (x_scroll_delta > 0)
    {
        exposed_area.x = exposed_area.width - x_scroll_delta;
    }
    else if (x_scroll_delta < 0)
    {
        exposed_area.width = -x_scroll_delta;
    }

    if (y_scroll_delta > 0)
    {
        exposed_area.y = exposed_area.height - y_scroll_delta;
    }
    else if (y_scroll_delta < 0)
    {
        exposed_area.height = -y_scroll_delta;
    }

    /* offset it to 0, 0 */
    exposed_area.x -= allocation.x;
    exposed_area.y -= allocation.y;

    gtk_widget_queue_draw_area (widget,
                                exposed_area.x,
                                exposed_area.y,
                                exposed_area.width,
                                exposed_area.height);

    return TRUE;
}

static void
set_up_auto_scroll_if_needed (CajaIconContainer *container)
{
    caja_drag_autoscroll_start (&container->details->dnd_info->drag_info,
                                GTK_WIDGET (container),
                                auto_scroll_timeout_callback,
                                container);
}

static void
stop_auto_scroll (CajaIconContainer *container)
{
    caja_drag_autoscroll_stop (&container->details->dnd_info->drag_info);
}

static void
handle_local_move (CajaIconContainer *container,
                   double world_x, double world_y)
{
    GList *moved_icons, *p;
    CajaFile *file;
    char screen_string[32];
    GdkScreen *screen;
    time_t now;
    CajaDragSelectionItem *item = NULL;
    CajaIcon *icon = NULL;

    if (container->details->auto_layout)
    {
        return;
    }

    time (&now);

    /* Move and select the icons. */
    moved_icons = NULL;
    for (p = container->details->dnd_info->drag_info.selection_list; p != NULL; p = p->next)
    {
        item = p->data;

        icon = caja_icon_container_get_icon_by_uri
               (container, item->uri);

        if (icon == NULL)
        {
            /* probably dragged from another screen.  Add it to
             * this screen
             */

            file = caja_file_get_by_uri (item->uri);

            screen = gtk_widget_get_screen (GTK_WIDGET (container));
            g_snprintf (screen_string, sizeof (screen_string), "%d",
                        gdk_x11_screen_get_screen_number (screen));
            caja_file_set_metadata (file,
                                    CAJA_METADATA_KEY_SCREEN,
                                    NULL, screen_string);
            caja_file_set_time_metadata (file,
                                         CAJA_METADATA_KEY_ICON_POSITION_TIMESTAMP, now);

            caja_icon_container_add (container, CAJA_ICON_CONTAINER_ICON_DATA (file));

            icon = caja_icon_container_get_icon_by_uri
                   (container, item->uri);
        }

        if (item->got_icon_position)
        {
            caja_icon_container_move_icon
            (container, icon,
             world_x + item->icon_x, world_y + item->icon_y,
             icon->scale,
             TRUE, TRUE, TRUE);
        }
        moved_icons = g_list_prepend (moved_icons, icon);
    }
    caja_icon_container_select_list_unselect_others
    (container, moved_icons);
    /* Might have been moved in a way that requires adjusting scroll region. */
    caja_icon_container_update_scroll_region (container);
    g_list_free (moved_icons);
}

static void
handle_nonlocal_move (CajaIconContainer *container,
                      GdkDragAction action,
                      int x, int y,
                      const char *target_uri,
                      gboolean icon_hit)
{
    GList *source_uris, *p;
    GArray *source_item_locations;
    gboolean free_target_uri, is_rtl;
    GtkAllocation allocation;

    if (container->details->dnd_info->drag_info.selection_list == NULL)
    {
        return;
    }

    source_uris = NULL;
    for (p = container->details->dnd_info->drag_info.selection_list; p != NULL; p = p->next)
    {
        /* do a shallow copy of all the uri strings of the copied files */
        source_uris = g_list_prepend (source_uris, ((CajaDragSelectionItem *)p->data)->uri);
    }
    source_uris = g_list_reverse (source_uris);

    is_rtl = caja_icon_container_is_layout_rtl (container);

    source_item_locations = g_array_new (FALSE, TRUE, sizeof (GdkPoint));
    if (!icon_hit)
    {
        int index;

        /* Drop onto a container. Pass along the item points to allow placing
         * the items in their same relative positions in the new container.
         */
        source_item_locations = g_array_set_size (source_item_locations,
                                g_list_length (container->details->dnd_info->drag_info.selection_list));

        for (index = 0, p = container->details->dnd_info->drag_info.selection_list;
                p != NULL; index++, p = p->next)
        {
            int item_x;

            item_x = ((CajaDragSelectionItem *)p->data)->icon_x;
            if (is_rtl)
                item_x = -item_x - ((CajaDragSelectionItem *)p->data)->icon_width;
            g_array_index (source_item_locations, GdkPoint, index).x = item_x;
            g_array_index (source_item_locations, GdkPoint, index).y =
                ((CajaDragSelectionItem *)p->data)->icon_y;
        }
    }

    free_target_uri = FALSE;
    /* Rewrite internal desktop URIs to the normal target uri */
    if (eel_uri_is_desktop (target_uri))
    {
        target_uri = caja_get_desktop_directory_uri ();
        free_target_uri = TRUE;
    }

    if (is_rtl)
    {
        gtk_widget_get_allocation (GTK_WIDGET (container), &allocation);
        x = CANVAS_WIDTH (container, allocation) - x;
    }

    /* start the copy */
    g_signal_emit_by_name (container, "move_copy_items",
                           source_uris,
                           source_item_locations,
                           target_uri,
                           action,
                           x, y);

    if (free_target_uri)
    {
        g_free ((char *)target_uri);
    }

    g_list_free (source_uris);
    g_array_free (source_item_locations, TRUE);
}

static char *
caja_icon_container_find_drop_target (CajaIconContainer *container,
                                      GdkDragContext *context,
                                      int x, int y,
                                      gboolean *icon_hit,
                                      gboolean rewrite_desktop)
{
    CajaIcon *drop_target_icon;
    double world_x, world_y;

    if (icon_hit)
    {
        *icon_hit = FALSE;
    }

    if (!container->details->dnd_info->drag_info.got_drop_data_type)
    {
        return NULL;
    }

    canvas_widget_to_world (EEL_CANVAS (container), x, y, &world_x, &world_y);

    /* FIXME bugzilla.gnome.org 42485:
     * These "can_accept_items" tests need to be done by
     * the icon view, not here. This file is not supposed to know
     * that the target is a file.
     */

    /* Find the item we hit with our drop, if any */
    drop_target_icon = caja_icon_container_item_at (container, world_x, world_y);
    if (drop_target_icon != NULL)
    {
        char *icon_uri;

        icon_uri = caja_icon_container_get_icon_uri (container, drop_target_icon);
        if (icon_uri != NULL)
        {
            CajaFile *file;

            file = caja_file_get_by_uri (icon_uri);

            if (!caja_drag_can_accept_info (file,
                                            container->details->dnd_info->drag_info.data_type,
                                            container->details->dnd_info->drag_info.selection_list))
            {
                /* the item we dropped our selection on cannot accept the items,
                 * do the same thing as if we just dropped the items on the canvas
                 */
                drop_target_icon = NULL;
            }

            g_free (icon_uri);
            caja_file_unref (file);
        }
    }

    if (drop_target_icon == NULL)
    {
        char *container_uri;

        if (icon_hit)
        {
            *icon_hit = FALSE;
        }

        container_uri = get_container_uri (container);

        if (rewrite_desktop &&
                container_uri != NULL &&
                eel_uri_is_desktop (container_uri))
        {
            g_free (container_uri);
            container_uri = caja_get_desktop_directory_uri ();
        }

        return container_uri;
    }

    if (icon_hit)
    {
        *icon_hit = TRUE;
    }
    return caja_icon_container_get_icon_drop_target_uri (container, drop_target_icon);
}

static gboolean
selection_is_image_file (GList *selection_list)
{
    const char *mime_type;
    CajaDragSelectionItem *selected_item;
    gboolean result;
    GFile *location;
    GFileInfo *info;

    /* Make sure only one item is selected */
    if (selection_list == NULL ||
            selection_list->next != NULL)
    {
        return FALSE;
    }

    selected_item = selection_list->data;

    mime_type = NULL;

    location = g_file_new_for_uri (selected_item->uri);
    info = g_file_query_info (location,
                              G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                              0, NULL, NULL);
    if (info)
    {
        mime_type = g_file_info_get_content_type (info);
    }

    result = eel_istr_has_prefix (mime_type, "image/");

    if (info)
    {
        g_object_unref (info);
    }
    g_object_unref (location);

    return result;
}

static void
caja_icon_container_receive_dropped_icons (CajaIconContainer *container,
        GdkDragContext *context,
        int x, int y)
{
    char *drop_target;
    gboolean local_move_only;
    double world_x, world_y;
    gboolean icon_hit;
    GdkDragAction action, real_action;

    drop_target = NULL;

    if (container->details->dnd_info->drag_info.selection_list == NULL)
    {
        return;
    }

    real_action = gdk_drag_context_get_selected_action (context);

    if (real_action == GDK_ACTION_ASK)
    {
        /* FIXME bugzilla.gnome.org 42485: This belongs in FMDirectoryView, not here. */
        /* Check for special case items in selection list */
        if (caja_drag_selection_includes_special_link (container->details->dnd_info->drag_info.selection_list))
        {
            /* We only want to move the trash */
            action = GDK_ACTION_MOVE;
        }
        else
        {
            action = GDK_ACTION_MOVE | GDK_ACTION_COPY | GDK_ACTION_LINK;

            if (selection_is_image_file (container->details->dnd_info->drag_info.selection_list))
            {
                action |= CAJA_DND_ACTION_SET_AS_BACKGROUND;
            }
        }
        real_action = caja_drag_drop_action_ask (GTK_WIDGET (container), action);
    }

    if (real_action == (GdkDragAction) CAJA_DND_ACTION_SET_AS_BACKGROUND)
    {
        CajaDragSelectionItem *selected_item;

        selected_item = container->details->dnd_info->drag_info.selection_list->data;
        eel_background_set_dropped_image (eel_get_widget_background (GTK_WIDGET (container)),
                                          real_action, selected_item->uri);
        return;
    }

    if (real_action > 0)
    {
        eel_canvas_window_to_world (EEL_CANVAS (container),
    				    x + gtk_adjustment_get_value (gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (container))),
    				    y + gtk_adjustment_get_value (gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (container))),
                                    &world_x, &world_y);

        drop_target = caja_icon_container_find_drop_target (container,
                      context, x, y, &icon_hit, FALSE);

        local_move_only = FALSE;
        if (!icon_hit && real_action == GDK_ACTION_MOVE)
        {
            /* we can just move the icon positions if the move ended up in
             * the item's parent container
             */
            local_move_only = caja_icon_container_selection_items_local
                              (container, container->details->dnd_info->drag_info.selection_list);
        }

        if (local_move_only)
        {
            handle_local_move (container, world_x, world_y);
        }
        else
        {
            handle_nonlocal_move (container, real_action, world_x, world_y, drop_target, icon_hit);
        }
    }

    g_free (drop_target);
    caja_drag_destroy_selection_list (container->details->dnd_info->drag_info.selection_list);
    container->details->dnd_info->drag_info.selection_list = NULL;
}

static void
caja_icon_container_get_drop_action (CajaIconContainer *container,
                                     GdkDragContext *context,
                                     int x, int y,
                                     int *action)
{
    char *drop_target;
    gboolean icon_hit;
    CajaIcon *icon;
    double world_x, world_y;

    icon_hit = FALSE;
    if (!container->details->dnd_info->drag_info.got_drop_data_type)
    {
        /* drag_data_received_callback didn't get called yet */
        return;
    }

    /* find out if we're over an icon */
    canvas_widget_to_world (EEL_CANVAS (container), x, y, &world_x, &world_y);

    icon = caja_icon_container_item_at (container, world_x, world_y);

    *action = 0;

    /* case out on the type of object being dragged */
    switch (container->details->dnd_info->drag_info.data_type)
    {
    case CAJA_ICON_DND_MATE_ICON_LIST:
        if (container->details->dnd_info->drag_info.selection_list == NULL)
        {
            return;
        }
        drop_target = caja_icon_container_find_drop_target (container,
                      context, x, y, &icon_hit, FALSE);
        if (!drop_target)
        {
            return;
        }
        caja_drag_default_drop_action_for_icons (context, drop_target,
                container->details->dnd_info->drag_info.selection_list,
                action);
        g_free (drop_target);
        break;
    case CAJA_ICON_DND_URI_LIST:
        drop_target = caja_icon_container_find_drop_target (container,
                      context, x, y, &icon_hit, FALSE);
        *action = caja_drag_default_drop_action_for_uri_list (context, drop_target);

        g_free (drop_target);
        break;

        /* handle emblems by setting the action if we're over an object */
    case CAJA_ICON_DND_KEYWORD:
        if (icon != NULL)
        {
            *action = gdk_drag_context_get_suggested_action (context);
        }
        break;

    case CAJA_ICON_DND_NETSCAPE_URL:
        *action = caja_drag_default_drop_action_for_netscape_url (context);
        break;

    case CAJA_ICON_DND_COLOR:
    case CAJA_ICON_DND_BGIMAGE:
    case CAJA_ICON_DND_RESET_BACKGROUND:
    case CAJA_ICON_DND_ROOTWINDOW_DROP:
        *action = gdk_drag_context_get_suggested_action (context);
        break;

    case CAJA_ICON_DND_TEXT:
    case CAJA_ICON_DND_XDNDDIRECTSAVE:
    case CAJA_ICON_DND_RAW:
        *action = GDK_ACTION_COPY;
        break;
    }
}

static void
set_drop_target (CajaIconContainer *container,
                 CajaIcon *icon)
{
    CajaIcon *old_icon;

    /* Check if current drop target changed, update icon drop
     * higlight if needed.
     */
    old_icon = container->details->drop_target;
    if (icon == old_icon)
    {
        return;
    }

    /* Remember the new drop target for the next round. */
    container->details->drop_target = icon;
    caja_icon_container_update_icon (container, old_icon);
    caja_icon_container_update_icon (container, icon);
}

static void
caja_icon_dnd_update_drop_target (CajaIconContainer *container,
                                  GdkDragContext *context,
                                  int x, int y)
{
    CajaIcon *icon;
    double world_x, world_y;

    g_assert (CAJA_IS_ICON_CONTAINER (container));

    canvas_widget_to_world (EEL_CANVAS (container), x, y, &world_x, &world_y);

    /* Find the item we hit with our drop, if any. */
    icon = caja_icon_container_item_at (container, world_x, world_y);

    /* FIXME bugzilla.gnome.org 42485:
     * These "can_accept_items" tests need to be done by
     * the icon view, not here. This file is not supposed to know
     * that the target is a file.
     */

    /* Find if target icon accepts our drop. */
    if (icon != NULL && (container->details->dnd_info->drag_info.data_type != CAJA_ICON_DND_KEYWORD))
    {
        CajaFile *file;
        char *uri;

        uri = caja_icon_container_get_icon_uri (container, icon);
        file = caja_file_get_by_uri (uri);
        g_free (uri);

        if (!caja_drag_can_accept_info (file,
                                        container->details->dnd_info->drag_info.data_type,
                                        container->details->dnd_info->drag_info.selection_list))
        {
            icon = NULL;
        }

        caja_file_unref (file);
    }

    set_drop_target (container, icon);
}

static void
caja_icon_container_free_drag_data (CajaIconContainer *container)
{
    CajaIconDndInfo *dnd_info;

    dnd_info = container->details->dnd_info;

    dnd_info->drag_info.got_drop_data_type = FALSE;

    if (dnd_info->shadow != NULL)
    {
        eel_canvas_item_destroy (dnd_info->shadow);
        dnd_info->shadow = NULL;
    }

    if (dnd_info->drag_info.selection_data != NULL)
    {
        gtk_selection_data_free (dnd_info->drag_info.selection_data);
        dnd_info->drag_info.selection_data = NULL;
    }

    if (dnd_info->drag_info.direct_save_uri != NULL)
    {
        g_free (dnd_info->drag_info.direct_save_uri);
        dnd_info->drag_info.direct_save_uri = NULL;
    }
}

static void
drag_leave_callback (GtkWidget *widget,
                     GdkDragContext *context,
                     guint32 time,
                     gpointer data)
{
    CajaIconDndInfo *dnd_info;

    dnd_info = CAJA_ICON_CONTAINER (widget)->details->dnd_info;

    if (dnd_info->shadow != NULL)
        eel_canvas_item_hide (dnd_info->shadow);

    stop_dnd_highlight (widget);

    set_drop_target (CAJA_ICON_CONTAINER (widget), NULL);
    stop_auto_scroll (CAJA_ICON_CONTAINER (widget));
    caja_icon_container_free_drag_data(CAJA_ICON_CONTAINER (widget));
}

static void
drag_begin_callback (GtkWidget      *widget,
                     GdkDragContext *context,
                     gpointer        data)
{
    CajaIconContainer *container;
    cairo_surface_t *surface;
    double x1, y1, x2, y2, winx, winy;
    int x_offset, y_offset;
    int start_x, start_y;

    container = CAJA_ICON_CONTAINER (widget);

    start_x = container->details->dnd_info->drag_info.start_x +
    	gtk_adjustment_get_value (gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (container)));
    start_y = container->details->dnd_info->drag_info.start_y +
    	gtk_adjustment_get_value (gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (container)));

    /* create a pixmap and mask to drag with */
    surface = caja_icon_canvas_item_get_drag_surface (container->details->drag_icon->item);

    /* compute the image's offset */
    eel_canvas_item_get_bounds (EEL_CANVAS_ITEM (container->details->drag_icon->item),
                                &x1, &y1, &x2, &y2);
    eel_canvas_world_to_window (EEL_CANVAS (container),
                                x1, y1,  &winx, &winy);
    x_offset = start_x - winx;
    y_offset = start_y - winy;

    cairo_surface_set_device_offset (surface, -x_offset, -y_offset);
    gtk_drag_set_icon_surface (context, surface);
    cairo_surface_destroy (surface);
}

void
caja_icon_dnd_begin_drag (CajaIconContainer *container,
                          GdkDragAction actions,
                          int button,
                          GdkEventMotion *event,
                          int                    start_x,
                          int                    start_y)
{
    CajaIconDndInfo *dnd_info;

    g_return_if_fail (CAJA_IS_ICON_CONTAINER (container));
    g_return_if_fail (event != NULL);

    dnd_info = container->details->dnd_info;
    g_return_if_fail (dnd_info != NULL);

    /* Notice that the event is in bin_window coordinates, because of
           the way the canvas handles events.
    */
    dnd_info->drag_info.start_x = start_x -
    	gtk_adjustment_get_value (gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (container)));
    dnd_info->drag_info.start_y = start_y -
    	gtk_adjustment_get_value (gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (container)));

    /* start the drag */
    gtk_drag_begin_with_coordinates (GTK_WIDGET (container),
                                    dnd_info->drag_info.target_list,
                                    actions,
                                    button,
                                    (GdkEvent *) event,
                                    dnd_info->drag_info.start_x,
                                    dnd_info->drag_info.start_y);
}

static gboolean
drag_highlight_draw (GtkWidget *widget,
                     cairo_t   *cr,
                     gpointer   user_data)
{
    gint width, height;
    GdkWindow *window;
    GtkStyleContext *style;

    window = gtk_widget_get_window (widget);
    width = gdk_window_get_width (window);
    height = gdk_window_get_height (window);

    style = gtk_widget_get_style_context (widget);

    gtk_style_context_save (style);
    gtk_style_context_add_class (style, GTK_STYLE_CLASS_DND);
    gtk_style_context_set_state (style, GTK_STATE_FLAG_FOCUSED);

    gtk_render_frame (style,
                      cr,
                      0, 0, width, height);

    gtk_style_context_restore (style);

    return FALSE;
}

/* Queue a redraw of the dnd highlight rect */
static void
dnd_highlight_queue_redraw (GtkWidget *widget)
{
    CajaIconDndInfo *dnd_info;
    int width, height;
    GtkAllocation allocation;

    dnd_info = CAJA_ICON_CONTAINER (widget)->details->dnd_info;

    if (!dnd_info->highlighted)
    {
        return;
    }

    gtk_widget_get_allocation (widget, &allocation);
    width = allocation.width;
    height = allocation.height;

    /* we don't know how wide the shadow is exactly,
     * so we expose a 10-pixel wide border
     */
    gtk_widget_queue_draw_area (widget,
                                0, 0,
                                width, 10);
    gtk_widget_queue_draw_area (widget,
                                0, 0,
                                10, height);
    gtk_widget_queue_draw_area (widget,
                                0, height - 10,
                                width, 10);
    gtk_widget_queue_draw_area (widget,
                                width - 10, 0,
                                10, height);
}

static void
start_dnd_highlight (GtkWidget *widget)
{
    CajaIconDndInfo *dnd_info;
    GtkWidget *toplevel;

    dnd_info = CAJA_ICON_CONTAINER (widget)->details->dnd_info;

    toplevel = gtk_widget_get_toplevel (widget);
    if (toplevel != NULL &&
            g_object_get_data (G_OBJECT (toplevel), "is_desktop_window"))
    {
        return;
    }

    if (!dnd_info->highlighted)
    {
        dnd_info->highlighted = TRUE;
        g_signal_connect_after (widget, "draw",
                                G_CALLBACK (drag_highlight_draw),
                                NULL);
        dnd_highlight_queue_redraw (widget);
    }
}

static void
stop_dnd_highlight (GtkWidget *widget)
{
    CajaIconDndInfo *dnd_info;

    dnd_info = CAJA_ICON_CONTAINER (widget)->details->dnd_info;

    if (dnd_info->highlighted)
    {
        g_signal_handlers_disconnect_by_func (widget,
                                              drag_highlight_draw,
                                              NULL);
        dnd_highlight_queue_redraw (widget);
        dnd_info->highlighted = FALSE;
    }
}

static gboolean
drag_motion_callback (GtkWidget *widget,
                      GdkDragContext *context,
                      int x, int y,
                      guint32 time)
{
    int action;

    caja_icon_container_ensure_drag_data (CAJA_ICON_CONTAINER (widget), context, time);
    caja_icon_container_position_shadow (CAJA_ICON_CONTAINER (widget), x, y);
    caja_icon_dnd_update_drop_target (CAJA_ICON_CONTAINER (widget), context, x, y);
    set_up_auto_scroll_if_needed (CAJA_ICON_CONTAINER (widget));
    /* Find out what the drop actions are based on our drag selection and
     * the drop target.
     */
    action = 0;
    caja_icon_container_get_drop_action (CAJA_ICON_CONTAINER (widget), context, x, y,
                                         &action);
    if (action != 0)
    {
        start_dnd_highlight (widget);
    }

    gdk_drag_status (context, action, time);

    return TRUE;
}

static gboolean
drag_drop_callback (GtkWidget *widget,
                    GdkDragContext *context,
                    int x,
                    int y,
                    guint32 time,
                    gpointer data)
{
    CajaIconDndInfo *dnd_info;

    dnd_info = CAJA_ICON_CONTAINER (widget)->details->dnd_info;

    /* tell the drag_data_received callback that
       the drop occured and that it can actually
       process the actions.
       make sure it is going to be called at least once.
    */
    dnd_info->drag_info.drop_occured = TRUE;

    get_data_on_first_target_we_support (widget, context, time, x, y);

    return TRUE;
}

void
caja_icon_dnd_end_drag (CajaIconContainer *container)
{
    CajaIconDndInfo *dnd_info;

    g_return_if_fail (CAJA_IS_ICON_CONTAINER (container));

    dnd_info = container->details->dnd_info;
    g_return_if_fail (dnd_info != NULL);
    stop_auto_scroll (container);
    /* Do nothing.
     * Can that possibly be right?
     */
}

/** this callback is called in 2 cases.
    It is called upon drag_motion events to get the actual data
    In that case, it just makes sure it gets the data.
    It is called upon drop_drop events to execute the actual
    actions on the received action. In that case, it actually first makes sure
    that we have got the data then processes it.
*/

static void
drag_data_received_callback (GtkWidget *widget,
                             GdkDragContext *context,
                             int x,
                             int y,
                             GtkSelectionData *data,
                             guint info,
                             guint32 time,
                             gpointer user_data)
{
    CajaDragInfo *drag_info;
    gboolean success;

    drag_info = &(CAJA_ICON_CONTAINER (widget)->details->dnd_info->drag_info);

    drag_info->got_drop_data_type = TRUE;
    drag_info->data_type = info;

    switch (info)
    {
    case CAJA_ICON_DND_MATE_ICON_LIST:
        caja_icon_container_dropped_icon_feedback (widget, data, x, y);
        break;
    case CAJA_ICON_DND_COLOR:
    case CAJA_ICON_DND_BGIMAGE:
    case CAJA_ICON_DND_KEYWORD:
    case CAJA_ICON_DND_URI_LIST:
    case CAJA_ICON_DND_TEXT:
    case CAJA_ICON_DND_RESET_BACKGROUND:
    case CAJA_ICON_DND_XDNDDIRECTSAVE:
    case CAJA_ICON_DND_RAW:
        /* Save the data so we can do the actual work on drop. */
        if (drag_info->selection_data != NULL)
        {
            gtk_selection_data_free (drag_info->selection_data);
        }
        drag_info->selection_data = gtk_selection_data_copy (data);
        break;

        /* Netscape keeps sending us the data, even though we accept the first drag */
    case CAJA_ICON_DND_NETSCAPE_URL:
        if (drag_info->selection_data != NULL)
        {
            gtk_selection_data_free (drag_info->selection_data);
            drag_info->selection_data = gtk_selection_data_copy (data);
        }
        break;
    case CAJA_ICON_DND_ROOTWINDOW_DROP:
        /* Do nothing, this won't even happen, since we don't want to call get_data twice */
        break;
    }

    /* this is the second use case of this callback.
     * we have to do the actual work for the drop.
     */
    if (drag_info->drop_occured)
    {
        EelBackground *background;
        char *tmp;
        const char *tmp_raw;
        int length;

        success = FALSE;
        switch (info)
        {
        case CAJA_ICON_DND_MATE_ICON_LIST:
            caja_icon_container_receive_dropped_icons
            (CAJA_ICON_CONTAINER (widget),
             context, x, y);
            break;
        case CAJA_ICON_DND_COLOR:
            receive_dropped_color (CAJA_ICON_CONTAINER (widget),
                                   x, y,
                                   gdk_drag_context_get_selected_action (context),
                                   data);
            success = TRUE;
            break;
        case CAJA_ICON_DND_BGIMAGE:
            receive_dropped_tile_image
            (CAJA_ICON_CONTAINER (widget),
             gdk_drag_context_get_selected_action (context),
             data);
            break;
        case CAJA_ICON_DND_KEYWORD:
            receive_dropped_keyword
            (CAJA_ICON_CONTAINER (widget),
             (char *) gtk_selection_data_get_data (data), x, y);
            break;
        case CAJA_ICON_DND_NETSCAPE_URL:
            receive_dropped_netscape_url
            (CAJA_ICON_CONTAINER (widget),
             (char *) gtk_selection_data_get_data (data), context, x, y);
            success = TRUE;
            break;
        case CAJA_ICON_DND_URI_LIST:
            receive_dropped_uri_list
            (CAJA_ICON_CONTAINER (widget),
             (char *) gtk_selection_data_get_data (data), context, x, y);
            success = TRUE;
            break;
        case CAJA_ICON_DND_TEXT:
            tmp = gtk_selection_data_get_text (data);
            receive_dropped_text
            (CAJA_ICON_CONTAINER (widget),
             (char *) tmp, context, x, y);
            success = TRUE;
            g_free (tmp);
            break;
        case CAJA_ICON_DND_RAW:
            length = gtk_selection_data_get_length (data);
            tmp_raw = gtk_selection_data_get_data (data);
            receive_dropped_raw
            (CAJA_ICON_CONTAINER (widget),
             tmp_raw, length, drag_info->direct_save_uri,
             context, x, y);
            success = TRUE;
            break;
        case CAJA_ICON_DND_RESET_BACKGROUND:
            background = eel_get_widget_background (widget);
            if (background != NULL)
            {
                eel_background_reset (background);
            }
            gtk_drag_finish (context, FALSE, FALSE, time);
            break;
        case CAJA_ICON_DND_ROOTWINDOW_DROP:
            /* Do nothing, everything is done by the sender */
            break;
        case CAJA_ICON_DND_XDNDDIRECTSAVE:
        {
            const guchar *selection_data;
            gint selection_length;
            gint selection_format;

            selection_data = gtk_selection_data_get_data (drag_info->selection_data);
            selection_length = gtk_selection_data_get_length (drag_info->selection_data);
            selection_format = gtk_selection_data_get_format (drag_info->selection_data);

            if (selection_format == 8 &&
                    selection_length == 1 &&
                    selection_data[0] == 'F')
            {
                gtk_drag_get_data (widget, context,
                                   gdk_atom_intern (CAJA_ICON_DND_RAW_TYPE,
                                                    FALSE),
                                   time);
                return;
            }
            else if (selection_format == 8 &&
                     selection_length == 1 &&
                     selection_data[0] == 'F' &&
                     drag_info->direct_save_uri != NULL)
            {
                GdkPoint p;
                GFile *location;

                location = g_file_new_for_uri (drag_info->direct_save_uri);

                caja_file_changes_queue_file_added (location);
                p.x = x;
                p.y = y;
                caja_file_changes_queue_schedule_position_set (
                    location,
                    p,
                    gdk_x11_screen_get_screen_number (
                        gtk_widget_get_screen (widget)));
                g_object_unref (location);
                caja_file_changes_consume_changes (TRUE);
                success = TRUE;
            }
            break;
        } /* CAJA_ICON_DND_XDNDDIRECTSAVE */
        }
        gtk_drag_finish (context, success, FALSE, time);

        caja_icon_container_free_drag_data (CAJA_ICON_CONTAINER (widget));

        set_drop_target (CAJA_ICON_CONTAINER (widget), NULL);

        /* reinitialise it for the next dnd */
        drag_info->drop_occured = FALSE;
    }

}

void
caja_icon_dnd_init (CajaIconContainer *container)
{
    GtkTargetList *targets;
    int n_elements;

    g_return_if_fail (container != NULL);
    g_return_if_fail (CAJA_IS_ICON_CONTAINER (container));

    container->details->dnd_info = g_new0 (CajaIconDndInfo, 1);
    caja_drag_init (&container->details->dnd_info->drag_info,
                    drag_types, G_N_ELEMENTS (drag_types), TRUE);

    /* Set up the widget as a drag destination.
     * (But not a source, as drags starting from this widget will be
         * implemented by dealing with events manually.)
     */
    n_elements = G_N_ELEMENTS (drop_types);
    if (!caja_icon_container_get_is_desktop (container))
    {
        /* Don't set up rootwindow drop */
        n_elements -= 1;
    }
    gtk_drag_dest_set (GTK_WIDGET (container),
                       0,
                       drop_types, n_elements,
                       GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK);

    targets = gtk_drag_dest_get_target_list (GTK_WIDGET (container));
    gtk_target_list_add_text_targets (targets, CAJA_ICON_DND_TEXT);

    /* Messages for outgoing drag. */
    g_signal_connect (container, "drag_begin",
                      G_CALLBACK (drag_begin_callback), NULL);
    g_signal_connect (container, "drag_data_get",
                      G_CALLBACK (drag_data_get_callback), NULL);
    g_signal_connect (container, "drag_end",
                      G_CALLBACK (drag_end_callback), NULL);

    /* Messages for incoming drag. */
    g_signal_connect (container, "drag_data_received",
                      G_CALLBACK (drag_data_received_callback), NULL);
    g_signal_connect (container, "drag_motion",
                      G_CALLBACK (drag_motion_callback), NULL);
    g_signal_connect (container, "drag_drop",
                      G_CALLBACK (drag_drop_callback), NULL);
    g_signal_connect (container, "drag_leave",
                      G_CALLBACK (drag_leave_callback), NULL);
}

void
caja_icon_dnd_fini (CajaIconContainer *container)
{
    g_return_if_fail (CAJA_IS_ICON_CONTAINER (container));

    if (container->details->dnd_info != NULL)
    {
        stop_auto_scroll (container);

        caja_drag_finalize (&container->details->dnd_info->drag_info);
        container->details->dnd_info = NULL;
    }
}
