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

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <stdlib.h>

#include <gtk/gtk.h>
#include <libxfce4util/libxfce4util.h>
#include <libburn.h>

#include "xfburn-global.h"
#include "xfburn-device.h"
#include "xfburn-device-list.h"
#include "xfburn-utils.h"
#include "xfburn-log.h"
#include "xfburn-disc-info.h"

#ifdef burn_header_version_major
#define XFBURN_LIBBURN_VERSION_CODE \
  ((burn_header_version_major * 10000) + (burn_header_version_minor * 100) + burn_header_version_micro)
#else
#define XFBURN_LIBBURN_VERSION_CODE 0
#endif

static gboolean
profile_is_cd (gint profile_no)
{
  return profile_no == XFBURN_PROFILE_CDROM ||
         profile_no == XFBURN_PROFILE_CDR ||
         profile_no == XFBURN_PROFILE_CDRW;
}

static gboolean
profile_is_bd (gint profile_no)
{
  return profile_no == XFBURN_PROFILE_BDROM ||
         profile_no == XFBURN_PROFILE_BD_R ||
         profile_no == XFBURN_PROFILE_BD_R_RRM ||
         profile_no == XFBURN_PROFILE_BD_RE;
}

static gboolean
profile_is_overwritable (gint profile_no)
{
  return profile_no == XFBURN_PROFILE_DVDRAM ||
         profile_no == XFBURN_PROFILE_DVD_MINUS_RW_OVERWRITE ||
         profile_no == XFBURN_PROFILE_DVD_PLUS_RW ||
         profile_no == XFBURN_PROFILE_DVD_PLUS_RW_DL ||
         profile_no == XFBURN_PROFILE_BD_RE;
}

static gboolean
profile_is_readonly (gint profile_no)
{
  return profile_no == XFBURN_PROFILE_CDROM ||
         profile_no == XFBURN_PROFILE_DVDROM ||
         profile_no == XFBURN_PROFILE_BDROM;
}

static gint
cmp_speed_ints (gconstpointer a, gconstpointer b)
{
  return GPOINTER_TO_INT (a) - GPOINTER_TO_INT (b);
}

static gboolean
speed_list_has (GSList *list, gint speed)
{
  GSList *it;

  for (it = list; it != NULL; it = it->next) {
    if (GPOINTER_TO_INT (it->data) == speed) {
      return TRUE;
    }
  }
  return FALSE;
}

/* Convert arbitrary firmware/media bytes to a safe UTF-8 printable ASCII string */
static gchar *
sanitize_printable_ascii (const char *src, gssize len, gboolean trim_spaces)
{
  GString *out;
  gssize i;

  if (src == NULL) {
    return NULL;
  }

  if (len < 0) {
    len = (gssize) strlen (src);
  }

  out = g_string_sized_new ((gsize) len + 1);
  for (i = 0; i < len; i++) {
    guchar c = (guchar) src[i];
    if (c == '\0') {
      break;
    }
    if (c >= 0x20 && c <= 0x7e) {
      g_string_append_c (out, (gchar) c);
    } else {
      g_string_append_c (out, '.');
    }
  }

  if (trim_spaces) {
    g_strstrip (out->str);
    out->len = strlen (out->str);
  }

  if (out->len == 0) {
    g_string_free (out, TRUE);
    return NULL;
  }

  return g_string_free (out, FALSE);
}

static gchar *
format_media_id_ascii (const XfburnDiscInfo *info)
{
  GString *out;
  const gchar *raw_prod;
  const gchar *c1;
  const gchar *c2;

  raw_prod = (info->raw_product_id != NULL && info->raw_product_id[0] != '\0')
                 ? info->raw_product_id
                 : info->product_id;
  c1 = (info->raw_media_code1 != NULL && info->raw_media_code1[0] != '\0')
           ? info->raw_media_code1
           : info->media_code1;
  c2 = (info->raw_media_code2 != NULL && info->raw_media_code2[0] != '\0')
           ? info->raw_media_code2
           : info->media_code2;

  out = g_string_new (NULL);

  if (raw_prod != NULL && raw_prod[0] != '\0') {
    gchar **parts = g_strsplit (raw_prod, "/", -1);
    gint i;

    for (i = 0; parts[i] != NULL; i++) {
      gchar *clean = sanitize_printable_ascii (parts[i], -1, TRUE);
      if (clean != NULL) {
        if (out->len > 0) {
          g_string_append_c (out, ' ');
        }
        g_string_append (out, clean);
        g_free (clean);
      }
    }
    g_strfreev (parts);
  } else {
    gchar *clean1 = sanitize_printable_ascii (c1, -1, TRUE);
    gchar *clean2 = sanitize_printable_ascii (c2, -1, TRUE);

    if (clean1 != NULL) {
      g_string_append (out, clean1);
      g_free (clean1);
    }
    if (clean2 != NULL) {
      if (out->len > 0) {
        g_string_append_c (out, ' ');
      }
      g_string_append (out, clean2);
      g_free (clean2);
    }
  }

  if (out->len == 0) {
    g_string_free (out, TRUE);
    return NULL;
  }

  return g_string_free (out, FALSE);
}

static void
append_hex_bytes (GString *out, const gchar *str)
{
  const guchar *p;

  if (str == NULL) {
    return;
  }

  for (p = (const guchar *) str; *p != '\0'; p++) {
    if (p != (const guchar *) str) {
      g_string_append_c (out, ' ');
    }
    g_string_append_printf (out, "%02X", (guint) *p);
  }
}

static gchar *
format_media_id_hex (const XfburnDiscInfo *info)
{
  GString *out;
  const gchar *raw_prod;
  const gchar *c1;
  const gchar *c2;

  raw_prod = (info->raw_product_id != NULL && info->raw_product_id[0] != '\0')
                 ? info->raw_product_id
                 : info->product_id;
  c1 = (info->raw_media_code1 != NULL && info->raw_media_code1[0] != '\0')
           ? info->raw_media_code1
           : info->media_code1;
  c2 = (info->raw_media_code2 != NULL && info->raw_media_code2[0] != '\0')
           ? info->raw_media_code2
           : info->media_code2;

  out = g_string_new (NULL);

  if (raw_prod != NULL && raw_prod[0] != '\0') {
    gchar **parts = g_strsplit (raw_prod, "/", -1);
    gint i;

    for (i = 0; parts[i] != NULL; i++) {
      if (parts[i][0] == '\0') {
        continue;
      }
      if (out->len > 0) {
        g_string_append (out, " / ");
      }
      append_hex_bytes (out, parts[i]);
    }
    g_strfreev (parts);
  } else {
    if (c1 != NULL && c1[0] != '\0') {
      append_hex_bytes (out, c1);
    }
    if (c2 != NULL && c2[0] != '\0') {
      if (out->len > 0) {
        g_string_append (out, " / ");
      }
      append_hex_bytes (out, c2);
    }
  }

  if (out->len == 0) {
    g_string_free (out, TRUE);
    return NULL;
  }

  return g_string_free (out, FALSE);
}

static gchar *
format_bytes_human (gint64 bytes)
{
  if (bytes < 0) {
    return g_strdup ("0 B");
  }
  if (bytes >= (gint64) 1024 * 1024 * 1024) {
    gdouble gib = (gdouble) bytes / (1024.0 * 1024.0 * 1024.0);
    gdouble mib = (gdouble) bytes / (1024.0 * 1024.0);
    return g_strdup_printf ("%.2f GiB (%.1f MiB)", gib, mib);
  }
  if (bytes >= 1024 * 1024) {
    gdouble mib = (gdouble) bytes / (1024.0 * 1024.0);
    return g_strdup_printf ("%.1f MiB", mib);
  }
  if (bytes >= 1024) {
    gdouble kib = (gdouble) bytes / 1024.0;
    return g_strdup_printf ("%.1f KiB", kib);
  }
  return g_strdup_printf ("%" G_GINT64_FORMAT " B", bytes);
}

static gchar *
format_sectors_msf (gint64 sectors)
{
  gint64 total_secs;
  gint64 mins;
  gint64 secs;

  if (sectors <= 0) {
    return g_strdup ("00:00");
  }

  total_secs = sectors / 75;
  mins = total_secs / 60;
  secs = total_secs % 60;
  return g_strdup_printf ("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, mins, secs);
}

static const gchar *
format_type_description (gint type)
{
  switch (type) {
    case 0x00:
      return _("Full format");
    case 0x10:
      return _("CD-RW/DVD-RW full");
    case 0x11:
      return _("CD-RW/DVD-RW grow");
    case 0x13:
      return _("DVD-RW quick grow");
    case 0x15:
      return _("DVD-RW quick");
    case 0x26:
      return _("DVD+RW background");
    case 0x30:
      return _("BD-RE with spare areas");
    case 0x31:
      return _("BD-RE without spare areas");
    case 0x32:
      return _("BD-R with spare areas");
    default:
      return NULL;
  }
}

static const gchar *
write_type_to_string (enum burn_write_types wt)
{
  switch (wt) {
    case BURN_WRITE_TAO:
      return "TAO";
    case BURN_WRITE_SAO:
      return "SAO/DAO";
    case BURN_WRITE_RAW:
      return "RAW";
    case BURN_WRITE_PACKET:
      return "Packet";
    case BURN_WRITE_NONE:
    default:
      return _("None");
  }
}

static const gchar *
disc_status_short_label (const XfburnDiscInfo *info)
{
  if (info->drive_busy_burning) {
    return _("Busy");
  }
  if (!info->grabbed) {
    return _("Unavailable");
  }

  switch (info->disc_status) {
    case BURN_DISC_BLANK:
      return profile_is_overwritable (info->profile_no) ? _("Blank / Overwritable") : _("Blank");
    case BURN_DISC_EMPTY:
      return _("Empty");
    case BURN_DISC_APPENDABLE:
      return _("Appendable");
    case BURN_DISC_FULL:
      return _("Full / Closed");
    case BURN_DISC_UNSUITABLE:
      return _("Unsuitable");
    case BURN_DISC_UNREADY:
      return _("Not Ready");
    case BURN_DISC_UNGRABBED:
    default:
      return _("Unavailable");
  }
}

static const gchar *
disc_status_long_label (const XfburnDiscInfo *info)
{
  if (info->drive_busy_burning) {
    return _("Drive is busy (burn or blank operation in progress)");
  }
  if (!info->grabbed) {
    if (info->umount_failed) {
      return _("Cannot access drive (disc is mounted and in use)");
    }
    return _("Cannot access drive (it might be in use)");
  }

  switch (info->disc_status) {
    case BURN_DISC_BLANK:
      if (profile_is_overwritable (info->profile_no)) {
        return _("Blank / Overwritable (ready to write)");
      }
      return _("Blank (ready to write)");
    case BURN_DISC_EMPTY:
      return _("Empty (no disc in drive)");
    case BURN_DISC_APPENDABLE:
      return _("Appendable (incomplete / multisession)");
    case BURN_DISC_FULL:
      return _("Full / Closed (read-only)");
    case BURN_DISC_UNSUITABLE:
      return _("Unsuitable media");
    case BURN_DISC_UNREADY:
      return _("Drive not ready");
    case BURN_DISC_UNGRABBED:
    default:
      return _("Unknown");
  }
}

static const gchar *
profile_display_name (const XfburnDiscInfo *info)
{
  if (info->profile_name[0] != '\0') {
    return info->profile_name;
  }

  switch (info->profile_no) {
    case XFBURN_PROFILE_CDROM:
      return "CD-ROM";
    case XFBURN_PROFILE_CDR:
      return "CD-R";
    case XFBURN_PROFILE_CDRW:
      return "CD-RW";
    case XFBURN_PROFILE_DVDROM:
      return "DVD-ROM";
    case XFBURN_PROFILE_DVD_MINUS_R:
      return "DVD-R";
    case XFBURN_PROFILE_DVDRAM:
      return "DVD-RAM";
    case XFBURN_PROFILE_DVD_MINUS_RW_OVERWRITE:
      return "DVD-RW restricted overwrite";
    case XFBURN_PROFILE_DVD_MINUS_RW_SEQUENTIAL:
      return "DVD-RW sequential recording";
    case XFBURN_PROFILE_DVD_MINUS_R_DL:
      return "DVD-R/DL sequential recording";
    case XFBURN_PROFILE_DVD_MINUS_R_DL_JUMP:
      return "DVD-R/DL layer jump";
    case XFBURN_PROFILE_DVD_PLUS_RW:
      return "DVD+RW";
    case XFBURN_PROFILE_DVD_PLUS_R:
      return "DVD+R";
    case XFBURN_PROFILE_DVD_PLUS_RW_DL:
      return "DVD+RW/DL";
    case XFBURN_PROFILE_DVD_PLUS_R_DL:
      return "DVD+R/DL";
    case XFBURN_PROFILE_BDROM:
      return "BD-ROM";
    case XFBURN_PROFILE_BD_R:
      return "BD-R sequential recording";
    case XFBURN_PROFILE_BD_R_RRM:
      return "BD-R random recording";
    case XFBURN_PROFILE_BD_RE:
      return "BD-RE";
    default:
      return _("Unknown");
  }
}

static void
append_kv (GString *out, const gchar *key, const gchar *value)
{
  const gchar *k = key != NULL ? key : "";
  glong char_len = g_utf8_strlen (k, -1);
  glong pad = (char_len < 20) ? (20 - char_len) : 1;

  g_string_append (out, "  ");
  g_string_append (out, k);
  while (pad-- > 0) {
    g_string_append_c (out, ' ');
  }
  if (k[0] != '\0') {
    g_string_append (out, ": ");
  } else {
    g_string_append (out, "  ");
  }
  g_string_append (out, value != NULL ? value : "");
  g_string_append_c (out, '\n');
}

void
xfburn_disc_info_clear (XfburnDiscInfo *info)
{
  if (info == NULL) {
    return;
  }

  g_clear_pointer (&info->error_msg, g_free);
  g_clear_pointer (&info->addr, g_free);
  g_clear_pointer (&info->name, g_free);
  g_clear_pointer (&info->revision, g_free);
  g_clear_pointer (&info->serial_no, g_free);

  g_clear_pointer (&info->product_id, g_free);
  g_clear_pointer (&info->media_code1, g_free);
  g_clear_pointer (&info->media_code2, g_free);
  g_clear_pointer (&info->raw_product_id, g_free);
  g_clear_pointer (&info->raw_media_code1, g_free);
  g_clear_pointer (&info->raw_media_code2, g_free);
  g_clear_pointer (&info->book_type, g_free);
  g_clear_pointer (&info->guessed_manufacturer, g_free);
  g_clear_pointer (&info->phys_book_name, g_free);

  g_clear_pointer (&info->formats, g_array_unref);
  g_clear_slist (&info->write_speeds, NULL);
  g_clear_pointer (&info->tracks, g_array_unref);

  memset (info, 0, sizeof (*info));
  info->disc_status = BURN_DISC_UNGRABBED;
  info->next_track_damaged = -1;
}

static void
collect_drive_info (struct burn_drive_info *drive_info, XfburnDiscInfo *out)
{
  struct burn_drive *drive = drive_info->drive;

  out->buffer_size = drive_info->buffer_size;
  out->drive_can_simulate = (drive_info->write_simulate != 0);

  if (out->name == NULL || out->name[0] == '\0') {
    gchar *vendor = sanitize_printable_ascii (drive_info->vendor, -1, TRUE);
    gchar *product = sanitize_printable_ascii (drive_info->product, -1, TRUE);
    if (vendor != NULL && product != NULL) {
      g_free (out->name);
      out->name = g_strdup_printf ("%s %s", vendor, product);
    } else if (product != NULL) {
      g_free (out->name);
      out->name = g_strdup (product);
    }
    g_free (vendor);
    g_free (product);
  }

  if (out->revision == NULL || out->revision[0] == '\0') {
    g_free (out->revision);
    out->revision = sanitize_printable_ascii (drive_info->revision, -1, TRUE);
  }

#if XFBURN_LIBBURN_VERSION_CODE >= 10402
  {
    char *sno = NULL;
    int sno_len = 0;

    if (burn_drive_get_serial_no (drive, &sno, &sno_len) == 1 && sno != NULL && sno_len > 0) {
      out->serial_no = sanitize_printable_ascii (sno, sno_len, TRUE);
    }
    free (sno);
  }
#endif

#if XFBURN_LIBBURN_VERSION_CODE >= 700
  {
    int num_profiles = 0;
    int profiles[64];
    char is_current[64];
    int i;

    memset (profiles, 0, sizeof (profiles));
    memset (is_current, 0, sizeof (is_current));
    if (burn_drive_get_all_profiles (drive, &num_profiles, profiles, is_current) == 1 &&
        num_profiles > 0) {
      if (num_profiles > 64) {
        num_profiles = 64;
      }
      out->num_supported_profiles = num_profiles;
      for (i = 0; i < num_profiles; i++) {
        out->supported_profiles[i] = profiles[i];
      }
    }
  }
#endif
}

static void
collect_media_identity (struct burn_drive *drive, XfburnDiscInfo *out)
{
  int prof_ret;

  prof_ret = burn_disc_get_profile (drive, &out->profile_no, out->profile_name);
  if (prof_ret != 1) {
    out->profile_no = XFBURN_PROFILE_NONE;
    out->profile_name[0] = '\0';
  }

  out->is_erasable = (burn_disc_erasable (drive) != 0);

  if (profile_is_cd (out->profile_no)) {
    int start_lba = 0;
    int end_lba = 0;

    burn_disc_read_atip (drive);
    if (burn_drive_get_start_end_lba (drive, &start_lba, &end_lba, 0) == 1) {
      out->has_atip_lba = TRUE;
      out->start_lba = start_lba;
      out->end_lba = end_lba;
    }
  }

#if XFBURN_LIBBURN_VERSION_CODE >= 702
  {
    char *prod = NULL;
    char *code1 = NULL;
    char *code2 = NULL;
    char *book = NULL;

    if (burn_disc_get_media_id (drive, &prod, &code1, &code2, &book, 0) == 1) {
      out->product_id = sanitize_printable_ascii (prod, -1, FALSE);
      out->media_code1 = sanitize_printable_ascii (code1, -1, FALSE);
      out->media_code2 = sanitize_printable_ascii (code2, -1, FALSE);
      out->book_type = sanitize_printable_ascii (book, -1, TRUE);

      if (code1 != NULL && code1[0] != '\0') {
        char *guessed = burn_guess_manufacturer (out->profile_no,
                                                 code1,
                                                 code2 != NULL ? code2 : "",
                                                 0);
        if (guessed != NULL) {
          out->guessed_manufacturer = sanitize_printable_ascii (guessed, -1, TRUE);
          free (guessed);
        }
      }
    }
    free (prod);
    free (code1);
    free (code2);
    free (book);

    /* Query unescaped raw ASCII codes (flag bit0 = 1) so spaces like "CMC MAG" are preserved */
    prod = NULL;
    code1 = NULL;
    code2 = NULL;
    book = NULL;
    if (burn_disc_get_media_id (drive, &prod, &code1, &code2, &book, 1) == 1) {
      if (prod != NULL && prod[0] != '\0') {
        out->raw_product_id = g_strdup (prod);
      }
      if (code1 != NULL && code1[0] != '\0') {
        out->raw_media_code1 = g_strdup (code1);
      }
      if (code2 != NULL && code2[0] != '\0') {
        out->raw_media_code2 = g_strdup (code2);
      }
    }
    free (prod);
    free (code1);
    free (code2);
    free (book);
  }

  {
    char disc_type[80];
    unsigned int disc_id = 0;
    char bar_code[9];
    int app_code = 0;
    int valid = 0;

    memset (disc_type, 0, sizeof (disc_type));
    memset (bar_code, 0, sizeof (bar_code));
    if (burn_disc_get_cd_info (drive, disc_type, &disc_id, bar_code, &app_code, &valid) == 1) {
      out->has_cd_info = TRUE;
      out->cd_valid_bits = valid;
      out->cd_disc_id = disc_id;
      if ((valid & (1 << 0)) != 0) {
        g_strlcpy (out->cd_disc_type, disc_type, sizeof (out->cd_disc_type));
      }
      if ((valid & (1 << 2)) != 0) {
        g_strlcpy (out->cd_bar_code, bar_code, sizeof (out->cd_bar_code));
      }
      if ((valid & (1 << 5)) != 0) {
        out->is_nominally_erasable = TRUE;
      }
    }
  }
#endif

#if XFBURN_LIBBURN_VERSION_CODE >= 10104
  {
    int disk_category = 0;
    char *book_name = NULL; /* static memory in libburn, must NOT be freed */
    int part_version = 0;
    int num_layers = 0;
    int num_blocks = 0;

    if (burn_disc_get_phys_format_info (drive,
                                        &disk_category,
                                        &book_name,
                                        &part_version,
                                        &num_layers,
                                        &num_blocks,
                                        0) == 1) {
      out->has_phys_format = TRUE;
      out->phys_disk_category = disk_category;
      out->phys_book_name = sanitize_printable_ascii (book_name, -1, TRUE);
      out->phys_part_version = part_version;
      out->phys_num_layers = num_layers;
      out->phys_num_blocks = num_blocks;
    }
  }
#endif

#if XFBURN_LIBBURN_VERSION_CODE >= 808
  if (profile_is_bd (out->profile_no) && out->profile_no != XFBURN_PROFILE_BDROM) {
    int alloc_blocks = 0;
    int free_blocks = 0;

    if (burn_disc_get_bd_spare_info (drive, &alloc_blocks, &free_blocks, 0) == 1) {
      out->has_bd_spare = TRUE;
      out->bd_spare_alloc_blocks = alloc_blocks;
      out->bd_spare_free_blocks = free_blocks;
    }
  }
#endif

#if XFBURN_LIBBURN_VERSION_CODE >= 10408
  if (profile_is_bd (out->profile_no)) {
    out->bd_r_pow = (burn_drive_get_bd_r_pow (drive) == 1);
  }
#endif
}

static void
collect_capacity_and_formats (struct burn_drive *drive, XfburnDiscInfo *out)
{
  int fmt_status = 0;
  off_t fmt_size = 0;
  unsigned fmt_bl_sas = 0;
  int num_formats = 0;

  out->available_space = (gint64) burn_disc_available_space (drive, NULL);

  if (burn_disc_get_formats (drive, &fmt_status, &fmt_size, &fmt_bl_sas, &num_formats) == 1) {
    int i;

    out->has_format_info = TRUE;
    out->format_status = fmt_status;
    out->format_size = (gint64) fmt_size;
    out->format_bl_sas = fmt_bl_sas;

    if (num_formats > 0) {
      out->formats = g_array_sized_new (FALSE, TRUE, sizeof (XfburnDiscFormatEntry), (guint) num_formats);
      for (i = 0; i < num_formats; i++) {
        int ftype = 0;
        off_t fsize = 0;
        unsigned ftdp = 0;

        if (burn_disc_get_format_descr (drive, i, &ftype, &fsize, &ftdp) == 1) {
          XfburnDiscFormatEntry entry;
          entry.type = ftype;
          entry.size = (gint64) fsize;
          entry.tdp = ftdp;
          g_array_append_val (out->formats, entry);
        }
      }
    }
  }

  if (out->disc_status == BURN_DISC_BLANK || out->disc_status == BURN_DISC_APPENDABLE) {
    int lba = 0;
    int nwa = 0;
    int nwa_ret;

    nwa_ret = burn_disc_track_lba_nwa (drive, NULL, 0, &lba, &nwa);
    if (nwa_ret == 1) {
      out->has_nwa = TRUE;
      out->next_track_lba = lba;
      out->next_track_nwa = nwa;
    }
#if XFBURN_LIBBURN_VERSION_CODE >= 10100
    if (nwa_ret >= 0) {
      out->next_track_damaged = burn_disc_next_track_is_damaged (drive, 0);
    }
#endif
  }
}

static void
collect_speeds_and_caps (struct burn_drive *drive, XfburnDiscInfo *out)
{
  struct burn_speed_descriptor *speed_list = NULL;
  struct burn_multi_caps *caps = NULL;
  int ret;

  ret = burn_drive_get_speedlist (drive, &speed_list);
  if (ret > 0 && speed_list != NULL) {
    struct burn_speed_descriptor *el = speed_list;

    while (el != NULL) {
      if (el->write_speed > 0 && !speed_list_has (out->write_speeds, el->write_speed)) {
        out->write_speeds = g_slist_prepend (out->write_speeds, GINT_TO_POINTER (el->write_speed));
      }
      el = el->next;
    }
    burn_drive_free_speedlist (&speed_list);
  }

  out->min_write_speed = burn_drive_get_min_write_speed (drive);
  out->max_write_speed = burn_drive_get_write_speed (drive);
  out->max_read_speed = burn_drive_get_read_speed (drive);

  if (out->min_write_speed > 0 && !speed_list_has (out->write_speeds, out->min_write_speed)) {
    out->write_speeds = g_slist_prepend (out->write_speeds, GINT_TO_POINTER (out->min_write_speed));
  }
  if (out->max_write_speed > 0 && !speed_list_has (out->write_speeds, out->max_write_speed)) {
    out->write_speeds = g_slist_prepend (out->write_speeds, GINT_TO_POINTER (out->max_write_speed));
  }
  if (out->write_speeds != NULL) {
    out->write_speeds = g_slist_sort (out->write_speeds, cmp_speed_ints);
  }

  if (burn_disc_get_multi_caps (drive, BURN_WRITE_NONE, &caps, 0) >= 0 && caps != NULL) {
    out->has_multi_caps = TRUE;
    out->might_do_tao = caps->might_do_tao;
    out->might_do_sao = caps->might_do_sao;
    out->might_do_raw = caps->might_do_raw;
    out->advised_write_mode = caps->advised_write_mode;
    out->multi_session = (caps->multi_session != 0);
    out->multi_track = (caps->multi_track != 0);
    out->might_simulate = (caps->might_simulate != 0);
    burn_disc_free_multi_caps (&caps);
  }
}

static void
collect_toc (struct burn_drive *drive, XfburnDiscInfo *out)
{
  struct burn_disc *disc;
  struct burn_session **sessions;
  int num_complete = 0;
  int num_incomplete = 0;
  int total_sessions;
  int s;
  gint64 total_blocks = 0;

  disc = burn_drive_get_disc (drive);
  if (disc == NULL) {
    return;
  }

  sessions = burn_disc_get_sessions (disc, &num_complete);
#if XFBURN_LIBBURN_VERSION_CODE >= 10208
  num_incomplete = burn_disc_get_incomplete_sessions (disc);
#endif

  out->complete_sessions = num_complete;
  out->incomplete_sessions = num_incomplete;
  total_sessions = num_complete + num_incomplete;

  if (sessions != NULL && total_sessions > 0) {
    out->tracks = g_array_new (FALSE, TRUE, sizeof (XfburnDiscTrackEntry));

    for (s = 0; s < total_sessions; s++) {
      struct burn_track **tracks;
      int num_tracks = 0;
      int t;

      if (sessions[s] == NULL) {
        continue;
      }

      tracks = burn_session_get_tracks (sessions[s], &num_tracks);
      if (tracks == NULL || num_tracks <= 0) {
        continue;
      }

      for (t = 0; t < num_tracks; t++) {
        struct burn_toc_entry toc;
        XfburnDiscTrackEntry entry;
        int session_no;
        int track_no;

        if (tracks[t] == NULL) {
          continue;
        }

        memset (&toc, 0, sizeof (toc));
        burn_track_get_entry (tracks[t], &toc);

        session_no = toc.session;
        track_no = toc.point;
        if ((toc.extensions_valid & 1) != 0) {
          session_no |= ((int) toc.session_msb << 8);
          track_no |= ((int) toc.point_msb << 8);
        }
        if (session_no <= 0) {
          session_no = s + 1;
        }

        memset (&entry, 0, sizeof (entry));
        entry.session = session_no;
        entry.track_no = track_no;
        entry.is_data = ((toc.control & 4) != 0);
        entry.extensions_valid = toc.extensions_valid;
        entry.track_status_bits = toc.track_status_bits;
        entry.pmin = toc.pmin;
        entry.psec = toc.psec;
        entry.pframe = toc.pframe;

        if ((toc.extensions_valid & 1) != 0) {
          entry.start_lba = toc.start_lba;
          entry.blocks = toc.track_blocks;
        } else {
          entry.start_lba = ((int) toc.pmin * 60 + (int) toc.psec) * 75 + (int) toc.pframe - 150;
          entry.blocks = 0;
        }

        if (entry.blocks > 0 && ((toc.extensions_valid & 4) == 0 || (toc.track_status_bits & (1 << 14)) == 0)) {
          total_blocks += entry.blocks;
        }

        g_array_append_val (out->tracks, entry);
      }
    }
  }

  if (total_blocks == 0) {
    int disc_sectors = burn_disc_get_sectors (disc);
    if (disc_sectors > 0) {
      total_blocks = disc_sectors;
    }
  }
  out->recorded_sectors = total_blocks;

  burn_disc_free (disc);
}

gboolean
xfburn_disc_info_probe_addr (const gchar *addr,
                             const gchar *name,
                             const gchar *revision,
                             GCancellable *cancellable,
                             XfburnDiscInfo *out_info)
{
  struct burn_drive_info *drive_info = NULL;
  int wait_ticks = 0;

  g_return_val_if_fail (out_info != NULL, FALSE);

  memset (out_info, 0, sizeof (*out_info));
  out_info->disc_status = BURN_DISC_UNGRABBED;
  out_info->next_track_damaged = -1;
  out_info->addr = g_strdup (addr);
  out_info->name = g_strdup (name);
  out_info->revision = g_strdup (revision);

  if (addr == NULL || addr[0] == '\0') {
    return FALSE;
  }

  if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
    out_info->cancelled = TRUE;
    return FALSE;
  }

  if (xfburn_device_is_burning ()) {
    out_info->drive_busy_burning = TRUE;
    return FALSE;
  }

  if (!xfburn_device_grab_addr_for_probe (addr,
                                          cancellable,
                                          &drive_info,
                                          &out_info->cancelled,
                                          &out_info->umount_failed,
                                          &out_info->error_msg)) {
    if (xfburn_device_is_burning ()) {
      out_info->drive_busy_burning = TRUE;
    }
    return FALSE;
  }

  collect_drive_info (drive_info, out_info);

  while ((out_info->disc_status = burn_disc_get_status (drive_info->drive)) == BURN_DISC_UNREADY &&
         wait_ticks < 50) {
    if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
      out_info->cancelled = TRUE;
      break;
    }
    g_usleep (100001);
    wait_ticks++;
  }

  if (!out_info->cancelled &&
      out_info->disc_status != BURN_DISC_EMPTY &&
      out_info->disc_status != BURN_DISC_UNGRABBED) {
    collect_media_identity (drive_info->drive, out_info);
    if (cancellable == NULL || !g_cancellable_is_cancelled (cancellable)) {
      collect_capacity_and_formats (drive_info->drive, out_info);
    }
    if (cancellable == NULL || !g_cancellable_is_cancelled (cancellable)) {
      collect_speeds_and_caps (drive_info->drive, out_info);
    }
    if (cancellable == NULL || !g_cancellable_is_cancelled (cancellable)) {
      if (out_info->disc_status == BURN_DISC_FULL ||
          out_info->disc_status == BURN_DISC_APPENDABLE ||
          (out_info->disc_status == BURN_DISC_BLANK && profile_is_overwritable (out_info->profile_no))) {
        collect_toc (drive_info->drive, out_info);
      }
    }
  } else if (!out_info->cancelled) {
    out_info->max_read_speed = burn_drive_get_read_speed (drive_info->drive);
  }

  xfburn_log_drain_backend_messages ("disc_info_probe", NULL, 0);
  xfburn_device_release_probe (drive_info);

  if (out_info->cancelled || (cancellable != NULL && g_cancellable_is_cancelled (cancellable))) {
    out_info->cancelled = TRUE;
    return FALSE;
  }

  out_info->grabbed = TRUE;
  XFBURN_LOG_INFO ("MEDIA",
                   "Disc info probed addr=\"%s\" status=%d profile=0x%04x name=\"%s\" product_id=\"%s\" manufacturer=\"%s\" avail_bytes=%" G_GINT64_FORMAT " sessions=%d+%d",
                   addr,
                   out_info->disc_status,
                   out_info->profile_no,
                   out_info->profile_name,
                   out_info->product_id != NULL ? out_info->product_id : "",
                   out_info->guessed_manufacturer != NULL ? out_info->guessed_manufacturer : "",
                   out_info->available_space,
                   out_info->complete_sessions,
                   out_info->incomplete_sessions);
  return TRUE;
}

gchar *
xfburn_disc_info_format_summary (const XfburnDiscInfo *info)
{
  const gchar *status;

  if (info == NULL) {
    return g_strdup (_("No burner detected"));
  }

  if (info->drive_busy_burning) {
    return g_strdup (_("Drive is busy (burn or blank operation in progress)"));
  }

  if (!info->grabbed) {
    if (info->umount_failed) {
      return g_strdup (_("Cannot access drive (disc is mounted and in use)"));
    }
    return g_strdup (_("Cannot access drive (it might be in use)"));
  }

  status = disc_status_short_label (info);

  if (info->disc_status == BURN_DISC_EMPTY) {
    return g_strdup_printf ("%s — %s", status, _("No disc in drive"));
  }

  if (info->profile_no != XFBURN_PROFILE_NONE) {
    return g_strdup_printf ("%s — %s (0x%04x)",
                            status,
                            profile_display_name (info),
                            (guint) info->profile_no);
  }

  return g_strdup (status);
}

static void
append_supported_profiles (GString *out, const XfburnDiscInfo *info)
{
#if XFBURN_LIBBURN_VERSION_CODE >= 700
  GString *prof_list;
  gint i;

  if (info->num_supported_profiles <= 0) {
    return;
  }

  prof_list = g_string_new (NULL);
  for (i = 0; i < info->num_supported_profiles; i++) {
    char pname[80];
    const gchar *short_name = NULL;

    memset (pname, 0, sizeof (pname));
    if (burn_obtain_profile_name (info->supported_profiles[i], pname) == 1 && pname[0] != '\0') {
      short_name = pname;
    } else {
      continue;
    }

    /* Shorten verbose MMC suffixes for compact single-line drive summary */
    if (g_str_has_prefix (short_name, "DVD-R sequential")) {
      short_name = "DVD-R";
    } else if (g_str_has_prefix (short_name, "DVD-RW sequential")) {
      short_name = "DVD-RW (seq)";
    } else if (g_str_has_prefix (short_name, "DVD-RW restricted")) {
      short_name = "DVD-RW (ovw)";
    } else if (g_str_has_prefix (short_name, "DVD-R/DL sequential")) {
      short_name = "DVD-R DL";
    } else if (g_str_has_prefix (short_name, "DVD-R/DL layer jump")) {
      short_name = "DVD-R DL (jump)";
    } else if (g_str_has_prefix (short_name, "BD-R sequential")) {
      short_name = "BD-R";
    } else if (g_str_has_prefix (short_name, "BD-R random")) {
      short_name = "BD-R (RRM)";
    }

    if (prof_list->len > 0) {
      g_string_append (prof_list, ", ");
    }
    g_string_append (prof_list, short_name);
  }

  if (prof_list->len > 0) {
    append_kv (out, _("Supported Media"), prof_list->str);
  }
  g_string_free (prof_list, TRUE);
#else
  (void) out;
  (void) info;
#endif
}

gchar *
xfburn_disc_info_format_report (const XfburnDiscInfo *info)
{
  GString *out;

  if (info == NULL) {
    return g_strdup (_("No optical burner detected.\n"));
  }

  out = g_string_new (NULL);

  /* Section 1: Drive */
  g_string_append_printf (out, "%s\n", _("Drive"));
  if (info->addr != NULL && info->addr[0] != '\0') {
    append_kv (out, _("Device"), info->addr);
  }
  if (info->name != NULL && info->name[0] != '\0') {
    if (info->revision != NULL && info->revision[0] != '\0') {
      gchar *model = g_strdup_printf ("%s (rev. %s)", info->name, info->revision);
      append_kv (out, _("Model"), model);
      g_free (model);
    } else {
      append_kv (out, _("Model"), info->name);
    }
  }
  if (info->serial_no != NULL && info->serial_no[0] != '\0') {
    append_kv (out, _("Serial Number"), info->serial_no);
  }
  if (info->buffer_size > 0) {
    gchar *buf_str = g_strdup_printf ("%d KiB", info->buffer_size);
    append_kv (out, _("Buffer Size"), buf_str);
    g_free (buf_str);
  }
  append_supported_profiles (out, info);

  /* Section 2: Disc & Media Identity */
  g_string_append_printf (out, "\n%s\n", _("Disc & Media Identity"));
  append_kv (out, _("Status"), disc_status_long_label (info));

  if (!info->grabbed) {
    if (info->error_msg != NULL && info->error_msg[0] != '\0') {
      append_kv (out, _("Details"), info->error_msg);
    }
    if (info->umount_failed) {
      g_string_append_printf (out, "\n  %s\n",
                              _("Close any files open on the disc or unmount it, then click Refresh."));
    }
    return g_string_free (out, FALSE);
  }

  if (info->disc_status == BURN_DISC_EMPTY) {
    if (info->max_read_speed > 0) {
      gchar *read_str = xfburn_format_burn_speed (info->max_read_speed, XFBURN_PROFILE_CDR);
      g_string_append_printf (out, "\n%s\n", _("Speeds & Write Modes"));
      append_kv (out, _("Max Read Speed"), read_str);
      g_free (read_str);
    }
    return g_string_free (out, FALSE);
  }

  if (info->profile_no != XFBURN_PROFILE_NONE) {
    gchar *ptype = g_strdup_printf ("%s (0x%04x)",
                                    profile_display_name (info),
                                    (guint) info->profile_no);
    append_kv (out, _("Media Type"), ptype);
    g_free (ptype);
  }

  if (profile_is_readonly (info->profile_no)) {
    append_kv (out, _("Rewritable"), _("No (read-only)"));
  } else if (profile_is_overwritable (info->profile_no)) {
    append_kv (out, _("Rewritable"), _("Yes (overwritable)"));
  } else if (info->is_erasable || info->is_nominally_erasable) {
    append_kv (out, _("Rewritable"), _("Yes (erasable)"));
  } else {
    append_kv (out, _("Rewritable"), _("No (write-once)"));
  }

  if (info->guessed_manufacturer != NULL && info->guessed_manufacturer[0] != '\0') {
    append_kv (out, _("Manufacturer"), info->guessed_manufacturer);
  }

  /* Only DVD and Blu-ray store an ASCII/binary Media ID on disc; CD-R/CD-RW
   * identify media solely through ATIP Lead-In/Lead-Out MSF timestamps. */
  if (!profile_is_cd (info->profile_no)) {
    gchar *mid_ascii = format_media_id_ascii (info);
    gchar *mid_hex = format_media_id_hex (info);

    if (mid_ascii != NULL) {
      append_kv (out, _("Media ID (ASCII)"), mid_ascii);
      g_free (mid_ascii);
    }
    if (mid_hex != NULL) {
      append_kv (out, _("Media ID (Hex)"), mid_hex);
      g_free (mid_hex);
    }
  }

  if (info->product_id != NULL && info->product_id[0] != '\0') {
    append_kv (out, _("Product ID"), info->product_id);
  }

  if (!profile_is_cd (info->profile_no)) {
    const gchar *mc1 = (info->raw_media_code1 != NULL && info->raw_media_code1[0] != '\0')
                           ? info->raw_media_code1
                           : info->media_code1;
    const gchar *mc2 = (info->raw_media_code2 != NULL && info->raw_media_code2[0] != '\0')
                           ? info->raw_media_code2
                           : info->media_code2;
    gchar *clean1 = sanitize_printable_ascii (mc1, -1, TRUE);
    gchar *clean2 = sanitize_printable_ascii (mc2, -1, TRUE);

    if (clean1 != NULL) {
      append_kv (out, _("Media Code 1"), clean1);
      g_free (clean1);
    }
    if (clean2 != NULL) {
      append_kv (out, _("Media Code 2"), clean2);
      g_free (clean2);
    }
  }

  if (info->book_type != NULL && info->book_type[0] != '\0') {
    append_kv (out, _("Book Type"), info->book_type);
  } else if (info->has_phys_format && info->phys_book_name != NULL && info->phys_book_name[0] != '\0') {
    append_kv (out, _("Book Type"), info->phys_book_name);
  }

  if (info->has_phys_format) {
    gchar *layers_str = g_strdup_printf ("%d (%s), %s %d",
                                         info->phys_num_layers,
                                         info->phys_num_layers > 1 ? _("multi-layer") : _("single-layer"),
                                         _("part version"),
                                         info->phys_part_version);
    append_kv (out, _("Physical Format"), layers_str);
    g_free (layers_str);

    if (info->phys_num_blocks > 0) {
      gchar *phys_size = format_bytes_human ((gint64) info->phys_num_blocks * DATA_BYTES_PER_SECTOR);
      gchar *phys_str = g_strdup_printf ("%s (%'d %s)", phys_size, info->phys_num_blocks, _("blocks"));
      append_kv (out, _("Physical Capacity"), phys_str);
      g_free (phys_str);
      g_free (phys_size);
    }
  }

  if (info->has_atip_lba) {
    int m_li = 0, s_li = 0, f_li = 0;
    int m_lo = 0, s_lo = 0, f_lo = 0;
    gchar *li_str;
    gchar *lo_str;

    burn_lba_to_msf (info->start_lba, &m_li, &s_li, &f_li);
    burn_lba_to_msf (info->end_lba, &m_lo, &s_lo, &f_lo);
    li_str = g_strdup_printf ("%02d:%02d:%02d (LBA %d)", m_li, s_li, f_li, info->start_lba);
    lo_str = g_strdup_printf ("%02d:%02d:%02d (LBA %d)", m_lo, s_lo, f_lo, info->end_lba);
    append_kv (out, _("ATIP Lead-In"), li_str);
    append_kv (out, _("ATIP Lead-Out"), lo_str);
    g_free (li_str);
    g_free (lo_str);
  }

  if (info->has_cd_info) {
    if (info->cd_disc_type[0] != '\0') {
      append_kv (out, _("CD Disc Type"), info->cd_disc_type);
    }
    if ((info->cd_valid_bits & (1 << 1)) != 0) {
      gchar *did = g_strdup_printf ("0x%08x", info->cd_disc_id);
      append_kv (out, _("CD Disc ID"), did);
      g_free (did);
    }
    if ((info->cd_valid_bits & (1 << 2)) != 0 && info->cd_bar_code[0] != '\0') {
      append_kv (out, _("Bar Code"), info->cd_bar_code);
    }
  }

  if (info->has_bd_spare) {
    gdouble pct = 0.0;
    gchar *spare_str;
    if (info->bd_spare_alloc_blocks > 0) {
      pct = (100.0 * (gdouble) info->bd_spare_free_blocks) / (gdouble) info->bd_spare_alloc_blocks;
    }
    spare_str = g_strdup_printf (_("%'d / %'d blocks free (%.1f%%)"),
                                 info->bd_spare_free_blocks,
                                 info->bd_spare_alloc_blocks,
                                 pct);
    append_kv (out, _("BD Spare Area"), spare_str);
    g_free (spare_str);
  }

  if (info->bd_r_pow) {
    append_kv (out, _("BD-R POW"), _("Yes (Pseudo Overwrite formatted; sequential multisession disabled)"));
  }

  /* Section 3: Capacity & Format */
  g_string_append_printf (out, "\n%s\n", _("Capacity & Format"));

  if (info->available_space > 0) {
    gint64 avail_sectors = info->available_space / DATA_BYTES_PER_SECTOR;
    gchar *human = format_bytes_human (info->available_space);
    gchar *val;

    if (profile_is_cd (info->profile_no)) {
      gchar *msf = format_sectors_msf (avail_sectors);
      val = g_strdup_printf ("%s (%'" G_GINT64_FORMAT " %s, %s)",
                             human, avail_sectors, _("sectors"), msf);
      g_free (msf);
    } else {
      val = g_strdup_printf ("%s (%'" G_GINT64_FORMAT " %s)",
                             human, avail_sectors, _("sectors"));
    }
    append_kv (out, _("Available Space"), val);
    g_free (val);
    g_free (human);
  } else {
    append_kv (out, _("Available Space"), _("0 B (none)"));
  }

  if (info->recorded_sectors > 0) {
    gint64 rec_bytes = info->recorded_sectors * DATA_BYTES_PER_SECTOR;
    gchar *human = format_bytes_human (rec_bytes);
    gchar *val;

    if (profile_is_cd (info->profile_no)) {
      gchar *msf = format_sectors_msf (info->recorded_sectors);
      val = g_strdup_printf ("%s (%'" G_GINT64_FORMAT " %s, %s)",
                             human, info->recorded_sectors, _("sectors"), msf);
      g_free (msf);
    } else {
      val = g_strdup_printf ("%s (%'" G_GINT64_FORMAT " %s)",
                             human, info->recorded_sectors, _("sectors"));
    }
    append_kv (out, _("Recorded Size"), val);
    g_free (val);
    g_free (human);
  }

  if (info->has_format_info) {
    const gchar *fstate;
    gchar *fsize = format_bytes_human (info->format_size);
    gchar *fstr;

    switch (info->format_status) {
      case BURN_FORMAT_IS_UNFORMATTED:
        fstate = _("Unformatted");
        break;
      case BURN_FORMAT_IS_FORMATTED:
        fstate = _("Formatted");
        break;
      case BURN_FORMAT_IS_UNKNOWN:
      default:
        fstate = _("Unknown");
        break;
    }

    if (info->format_size > 0) {
      fstr = g_strdup_printf ("%s (%s)", fstate, fsize);
    } else {
      fstr = g_strdup (fstate);
    }
    append_kv (out, _("Format Status"), fstr);
    g_free (fstr);
    g_free (fsize);

    if (info->formats != NULL && info->formats->len > 0) {
      guint i;
      for (i = 0; i < info->formats->len; i++) {
        const XfburnDiscFormatEntry *fe = &g_array_index (info->formats, XfburnDiscFormatEntry, i);
        const gchar *desc = format_type_description (fe->type);
        gchar *esize = format_bytes_human (fe->size);
        gchar *line;

        if (desc != NULL) {
          line = g_strdup_printf ("0x%02x %s — %s", (guint) fe->type, desc, esize);
        } else {
          line = g_strdup_printf ("0x%02x — %s", (guint) fe->type, esize);
        }
        append_kv (out, i == 0 ? _("Available Formats") : "", line);
        g_free (line);
        g_free (esize);
      }
    }
  }

  if (info->has_nwa) {
    gchar *nwa_str = g_strdup_printf ("LBA %d (%s %d)", info->next_track_nwa, _("track start"), info->next_track_lba);
    append_kv (out, _("Next Writable"), nwa_str);
    g_free (nwa_str);
  }

  if (info->next_track_damaged == 1) {
    append_kv (out, _("Track Health"), _("Warning: last track marked damaged (theoretically writable)"));
  } else if (info->next_track_damaged == 3) {
    append_kv (out, _("Track Health"), _("Warning: last track marked damaged and not writable"));
  }

  /* Section 4: Speeds & Write Modes */
  g_string_append_printf (out, "\n%s\n", _("Speeds & Write Modes"));

  if (info->write_speeds != NULL) {
    GString *sp = g_string_new (NULL);
    GSList *it;

    for (it = info->write_speeds; it != NULL; it = it->next) {
      gchar *lbl = xfburn_format_burn_speed (GPOINTER_TO_INT (it->data), info->profile_no);
      if (sp->len > 0) {
        g_string_append (sp, ", ");
      }
      g_string_append (sp, lbl);
      g_free (lbl);
    }
    append_kv (out, _("Write Speeds"), sp->str);
    g_string_free (sp, TRUE);
  } else if (!profile_is_readonly (info->profile_no)) {
    append_kv (out, _("Write Speeds"), _("None reported"));
  }

  if (info->max_read_speed > 0) {
    gchar *read_str = xfburn_format_burn_speed (info->max_read_speed, info->profile_no);
    append_kv (out, _("Max Read Speed"), read_str);
    g_free (read_str);
  }

  if (info->has_multi_caps) {
    GString *modes = g_string_new (NULL);
    GString *caps = g_string_new (NULL);

    if (info->might_do_tao > 0) {
      g_string_append (modes, "TAO");
    }
    if (info->might_do_sao > 0) {
      if (modes->len > 0) {
        g_string_append (modes, ", ");
      }
      g_string_append (modes, "SAO/DAO");
    }
    if (info->might_do_raw > 0) {
      if (modes->len > 0) {
        g_string_append (modes, ", ");
      }
      g_string_append (modes, "RAW");
    }
    if (modes->len > 0) {
      if (info->advised_write_mode != BURN_WRITE_NONE) {
        g_string_append_printf (modes, " (%s: %s)", _("advised"), write_type_to_string (info->advised_write_mode));
      }
      append_kv (out, _("Write Modes"), modes->str);
    }
    g_string_free (modes, TRUE);

    if (info->multi_session) {
      g_string_append (caps, _("Multi-session"));
    }
    if (info->multi_track) {
      if (caps->len > 0) {
        g_string_append (caps, ", ");
      }
      g_string_append (caps, _("Multi-track"));
    }
    if (info->might_simulate) {
      if (caps->len > 0) {
        g_string_append (caps, ", ");
      }
      g_string_append (caps, _("Simulation (dummy write)"));
    }
    if (caps->len > 0) {
      append_kv (out, _("Capabilities"), caps->str);
    }
    g_string_free (caps, TRUE);
  }

  /* Section 5: Sessions & Tracks (TOC) */
  if (info->complete_sessions > 0 || info->incomplete_sessions > 0 ||
      (info->tracks != NULL && info->tracks->len > 0)) {
    gchar *sess_str;

    g_string_append_printf (out, "\n%s\n", _("Sessions & Tracks"));
    if (info->incomplete_sessions > 0) {
      sess_str = g_strdup_printf (_("%d complete, %d incomplete"),
                                  info->complete_sessions,
                                  info->incomplete_sessions);
    } else {
      sess_str = g_strdup_printf (_("%d complete"), info->complete_sessions);
    }
    append_kv (out, _("Sessions"), sess_str);
    g_free (sess_str);

    if (info->tracks != NULL && info->tracks->len > 0) {
      guint i;

      g_string_append (out, "\n");
      g_string_append_printf (out, "  %-4s %-5s %-6s %-11s %-11s %-12s %s\n",
                              _("Sess"), _("Track"), _("Type"), _("Start LBA"),
                              _("Sectors"), _("Size"), _("Flags"));
      g_string_append (out, "  ---- ----- ------ ----------- ----------- ------------ ----------\n");

      for (i = 0; i < info->tracks->len; i++) {
        const XfburnDiscTrackEntry *te = &g_array_index (info->tracks, XfburnDiscTrackEntry, i);
        const gchar *type_str = te->is_data ? _("Data") : _("Audio");
        gchar *size_str;
        GString *flags = g_string_new (NULL);

        if (te->blocks > 0) {
          if (!te->is_data && profile_is_cd (info->profile_no)) {
            size_str = format_sectors_msf (te->blocks);
          } else {
            size_str = format_bytes_human ((gint64) te->blocks * DATA_BYTES_PER_SECTOR);
          }
        } else {
          size_str = g_strdup ("-");
        }

        if ((te->extensions_valid & 4) != 0) {
          if ((te->track_status_bits & (1 << 14)) != 0) {
            g_string_append (flags, _("Blank"));
          }
          if ((te->track_status_bits & (1 << 5)) != 0) {
            if (flags->len > 0) {
              g_string_append (flags, ",");
            }
            g_string_append (flags, _("Damaged"));
          }
          if ((te->track_status_bits & (1 << 13)) != 0) {
            if (flags->len > 0) {
              g_string_append (flags, ",");
            }
            g_string_append (flags, _("Packet"));
          }
        }

        g_string_append_printf (out, "  %-4d %-5d %-6s %-11d %-11d %-12s %s\n",
                                te->session,
                                te->track_no,
                                type_str,
                                te->start_lba,
                                te->blocks,
                                size_str,
                                flags->len > 0 ? flags->str : "-");
        g_free (size_str);
        g_string_free (flags, TRUE);
      }
    }
  }

  return g_string_free (out, FALSE);
}
