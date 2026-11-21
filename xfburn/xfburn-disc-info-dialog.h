/*
 * Copyright (c) 2026 Dominik Klein
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef __XFBURN_DISC_INFO_DIALOG_H__
#define __XFBURN_DISC_INFO_DIALOG_H__

#include <gtk/gtk.h>
#include <libxfce4ui/libxfce4ui.h>

G_BEGIN_DECLS

#if !LIBXFCE4UI_CHECK_VERSION(4, 21, 8)
#ifndef XFBURN_LIBXFCE4UI_AUTOPTR_CLEANUP_FUNC_ALREADY_DEFINED
G_DEFINE_AUTOPTR_CLEANUP_FUNC (XfceTitledDialog, g_object_unref)
#define XFBURN_LIBXFCE4UI_AUTOPTR_CLEANUP_FUNC_ALREADY_DEFINED 1
#endif
#endif

#define XFBURN_TYPE_DISC_INFO_DIALOG xfburn_disc_info_dialog_get_type ()
G_DECLARE_FINAL_TYPE (XfburnDiscInfoDialog, xfburn_disc_info_dialog, XFBURN, DISC_INFO_DIALOG, XfceTitledDialog)

GtkWidget *xfburn_disc_info_dialog_new (void);

G_END_DECLS

#endif /* __XFBURN_DISC_INFO_DIALOG_H__ */
