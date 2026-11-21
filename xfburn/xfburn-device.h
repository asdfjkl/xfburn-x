/*
 *  Copyright (C) 2009 David Mohr <david@mcbf.net>
 *  Copyright (c) 2026 Dominik Klein
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifndef __XFBURN_DEVICE__
#define __XFBURN_DEVICE__

#include <gio/gio.h>
#include <glib-object.h>
#include <libburn.h>

G_BEGIN_DECLS

#define XFBURN_TYPE_DEVICE xfburn_device_get_type()
G_DECLARE_FINAL_TYPE (XfburnDevice, xfburn_device, XFBURN, DEVICE, GObject)

typedef struct {
  gchar *addr;
  gboolean grabbed;
  gboolean cancelled;
  gboolean umount_failed;
  gchar *error_msg;

  gboolean has_libburn_info;
  gboolean cdr;
  gboolean cdrw;
  gboolean dvdr;
  gboolean dvdram;
  gint buffer_size;
  gboolean dummy_write;
  gint tao_block_types;
  gint sao_block_types;
  gint raw_block_types;
  gint packet_block_types;

  enum burn_disc_status disc_status;
  int profile_no;
  char profile_name[80];
  int is_erasable;
  GSList *supported_speeds;
} XfburnDeviceProbeResult;

XfburnDevice* xfburn_device_new (void);

void xfburn_device_probe_result_clear (XfburnDeviceProbeResult *result);
gboolean xfburn_device_probe_addr (const gchar *addr,
                                   const gchar *name,
                                   gboolean get_speed_info,
                                   gboolean details_known,
                                   GCancellable *cancellable,
                                   XfburnDeviceProbeResult *out_result);
void xfburn_device_apply_probe_result (XfburnDevice *device,
                                       XfburnDeviceProbeResult *result);

void xfburn_device_fillin_libburn_info (XfburnDevice *device, struct burn_drive_info *drive);
gboolean xfburn_device_refresh_info (XfburnDevice * device, gboolean get_speed_info);
gboolean xfburn_device_grab (XfburnDevice * device, struct burn_drive_info **drive_info);
gboolean xfburn_device_grab_addr_for_probe (const gchar *addr,
                                            GCancellable *cancellable,
                                            struct burn_drive_info **drive_info,
                                            gboolean *out_cancelled,
                                            gboolean *out_umount_failed,
                                            gchar **out_error_msg);
gboolean xfburn_device_release (struct burn_drive_info *drive_info, gboolean eject);
void xfburn_device_release_probe (struct burn_drive_info *drive_info);
void xfburn_device_wait_for_probes (void);
gboolean xfburn_device_is_busy (void);
gboolean xfburn_device_is_burning (void);
gboolean xfburn_device_details_known (XfburnDevice *device);
gboolean xfburn_device_last_umount_failed (XfburnDevice *device);
const gchar * xfburn_device_set_name (XfburnDevice *device, const gchar *vendor, const gchar *product);
gboolean xfburn_device_can_burn (XfburnDevice *device);
gboolean xfburn_device_can_dummy_write (XfburnDevice *device);

G_END_DECLS

#endif /* __XFBURN_DEVICE__ */

