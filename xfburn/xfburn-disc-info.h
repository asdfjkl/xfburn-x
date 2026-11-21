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

#ifndef __XFBURN_DISC_INFO_H__
#define __XFBURN_DISC_INFO_H__

#include <glib.h>
#include <gio/gio.h>
#include <libburn.h>

G_BEGIN_DECLS

typedef struct {
  gint type;
  gint64 size;
  guint tdp;
} XfburnDiscFormatEntry;

typedef struct {
  gint session;
  gint track_no;
  gboolean is_data;
  gint start_lba;
  gint blocks;
  guchar extensions_valid;
  gint track_status_bits;
  guchar pmin;
  guchar psec;
  guchar pframe;
} XfburnDiscTrackEntry;

typedef struct {
  /* Grab / execution status */
  gboolean grabbed;
  gboolean cancelled;
  gboolean umount_failed;
  gboolean drive_busy_burning;
  gchar *error_msg;

  /* Drive info */
  gchar *addr;
  gchar *name;
  gchar *revision;
  gchar *serial_no;
  gint buffer_size;
  gboolean drive_can_simulate;
  gint num_supported_profiles;
  gint supported_profiles[64];

  /* Disc status & media identity */
  enum burn_disc_status disc_status;
  gint profile_no;
  gchar profile_name[80];
  gboolean is_erasable;
  gboolean is_nominally_erasable;

  gchar *product_id;
  gchar *media_code1;
  gchar *media_code2;
  gchar *raw_product_id;
  gchar *raw_media_code1;
  gchar *raw_media_code2;
  gchar *book_type;
  gchar *guessed_manufacturer;

  /* CD-specific metadata */
  gboolean has_atip_lba;
  gint start_lba;
  gint end_lba;
  gboolean has_cd_info;
  gchar cd_disc_type[80];
  gint cd_valid_bits;
  guint cd_disc_id;
  gchar cd_bar_code[16];

  /* DVD physical format info */
  gboolean has_phys_format;
  gint phys_disk_category;
  gchar *phys_book_name;
  gint phys_part_version;
  gint phys_num_layers;
  gint phys_num_blocks;

  /* BD-specific metadata */
  gboolean has_bd_spare;
  gint bd_spare_alloc_blocks;
  gint bd_spare_free_blocks;
  gboolean bd_r_pow;

  /* Capacity & format status */
  gint64 available_space;
  gint64 recorded_sectors;
  gboolean has_format_info;
  gint format_status;
  gint64 format_size;
  guint format_bl_sas;
  GArray *formats; /* element type: XfburnDiscFormatEntry */

  gboolean has_nwa;
  gint next_track_lba;
  gint next_track_nwa;
  gint next_track_damaged; /* -1 = not queried, 0..3 per burn_disc_next_track_is_damaged */

  /* Speeds & write capabilities */
  GSList *write_speeds; /* GINT_TO_POINTER (kbps), sorted ascending */
  gint min_write_speed;
  gint max_write_speed;
  gint max_read_speed;

  gboolean has_multi_caps;
  gint might_do_tao;
  gint might_do_sao;
  gint might_do_raw;
  enum burn_write_types advised_write_mode;
  gboolean multi_session;
  gboolean multi_track;
  gboolean might_simulate;

  /* Sessions & tracks (TOC) */
  gint complete_sessions;
  gint incomplete_sessions;
  GArray *tracks; /* element type: XfburnDiscTrackEntry */
} XfburnDiscInfo;

void xfburn_disc_info_clear (XfburnDiscInfo *info);

gboolean xfburn_disc_info_probe_addr (const gchar *addr,
                                      const gchar *name,
                                      const gchar *revision,
                                      GCancellable *cancellable,
                                      XfburnDiscInfo *out_info);

gchar *xfburn_disc_info_format_summary (const XfburnDiscInfo *info);
gchar *xfburn_disc_info_format_report (const XfburnDiscInfo *info);

G_END_DECLS

#endif /* __XFBURN_DISC_INFO_H__ */
