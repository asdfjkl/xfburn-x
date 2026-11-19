/*
 * Copyright (c) 2005-2006 Jean-François Wauthy (pollux@xfce.org)
 * Copyright (c) 2008      David Mohr (squisher@xfce.org)
 * Copyright (c) 2026      Dominik Klein
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
 */

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <stdlib.h>
#include <glib.h>
#include <libburn.h>
#include <unistd.h>

#include "xfburn-global.h"
#include "xfburn-perform-burn.h"
#include "xfburn-progress-dialog.h"
#include "xfburn-utils.h"
#include "xfburn-log.h"

struct _XfburnVerifyData
{
  GArray *sector_hashes;       /* guint64 per sector */
  GArray *track_sector_counts; /* guint per track */
  gint sector_size;
  guchar *partial_buf;
  gint partial_len;
  gboolean partial_swap;
  guint partial_track_idx;
  off_t max_bytes;
  off_t total_bytes_seen;
};

typedef struct
{
  struct burn_source *inner;
  gboolean swap;
  guint track_idx;
  XfburnVerifyData *verify_data;
} VerifySourcePriv;

static guint64
xfburn_hash_sector (const guchar *buf, gint size)
{
  guint64 h = G_GUINT64_CONSTANT (0xcbf29ce484222325);
  gint i;

  for (i = 0; i < size; i++) {
    h ^= (guint64) buf[i];
    h *= G_GUINT64_CONSTANT (0x100000001b3);
  }
  h ^= h >> 33;
  h *= G_GUINT64_CONSTANT (0xff51afd7ed558ccd);
  h ^= h >> 33;
  h *= G_GUINT64_CONSTANT (0xc4ceb9fe1a85ec53);
  h ^= h >> 33;
  return h;
}

static void
xfburn_verify_commit_partial_sector (XfburnVerifyData *vdata)
{
  gint i;
  guint64 h;

  if (vdata == NULL || vdata->partial_len <= 0)
    return;

  if (vdata->partial_len < vdata->sector_size)
    memset (vdata->partial_buf + vdata->partial_len, 0, vdata->sector_size - vdata->partial_len);

  if (vdata->partial_swap) {
    for (i = 0; i + 1 < vdata->sector_size; i += 2) {
      guchar tmp = vdata->partial_buf[i];
      vdata->partial_buf[i] = vdata->partial_buf[i + 1];
      vdata->partial_buf[i + 1] = tmp;
    }
  }

  h = xfburn_hash_sector (vdata->partial_buf, vdata->sector_size);
  g_array_append_val (vdata->sector_hashes, h);

  if (vdata->partial_track_idx < vdata->track_sector_counts->len)
    g_array_index (vdata->track_sector_counts, guint, vdata->partial_track_idx)++;

  vdata->partial_len = 0;
}

static void
xfburn_verify_feed_bytes (XfburnVerifyData *vdata, const guchar *buf, gint len, gboolean swap, guint track_idx)
{
  gint offset = 0;
  gint to_feed = len;

  if (vdata == NULL || len <= 0)
    return;

  if (vdata->partial_len > 0 && vdata->partial_track_idx != track_idx)
    xfburn_verify_commit_partial_sector (vdata);

  if (vdata->max_bytes > 0) {
    off_t rem = vdata->max_bytes - vdata->total_bytes_seen;
    if (rem <= 0) {
      vdata->total_bytes_seen += len;
      return;
    }
    if ((off_t) to_feed > rem)
      to_feed = (gint) rem;
  }
  vdata->total_bytes_seen += len;

  while (offset < to_feed) {
    gint space = vdata->sector_size - vdata->partial_len;
    gint chunk = MIN (space, to_feed - offset);

    memcpy (vdata->partial_buf + vdata->partial_len, buf + offset, chunk);
    vdata->partial_len += chunk;
    vdata->partial_swap = swap;
    vdata->partial_track_idx = track_idx;
    offset += chunk;

    if (vdata->partial_len == vdata->sector_size)
      xfburn_verify_commit_partial_sector (vdata);
  }
}

static int
verify_source_read (struct burn_source *src, unsigned char *buffer, int size)
{
  VerifySourcePriv *priv = (VerifySourcePriv *) src->data;
  int ret;

  if (priv == NULL || priv->inner == NULL)
    return -1;

  if (priv->inner->read != NULL)
    ret = priv->inner->read (priv->inner, buffer, size);
  else if (priv->inner->read_xt != NULL)
    ret = priv->inner->read_xt (priv->inner, buffer, size);
  else
    return -1;

  if (ret > 0) {
    xfburn_verify_feed_bytes (priv->verify_data, buffer, ret, priv->swap, priv->track_idx);
  } else if (ret == 0) {
    if (priv->verify_data && priv->verify_data->partial_len > 0 &&
        priv->verify_data->partial_track_idx == priv->track_idx) {
      xfburn_verify_commit_partial_sector (priv->verify_data);
    }
  }

  return ret;
}

static off_t
verify_source_get_size (struct burn_source *src)
{
  VerifySourcePriv *priv = (VerifySourcePriv *) src->data;
  if (priv && priv->inner && priv->inner->get_size)
    return priv->inner->get_size (priv->inner);
  return 0;
}

static int
verify_source_set_size (struct burn_source *src, off_t size)
{
  VerifySourcePriv *priv = (VerifySourcePriv *) src->data;
  if (priv && priv->inner && priv->inner->set_size)
    return priv->inner->set_size (priv->inner, size);
  return 1;
}

static int
verify_source_cancel (struct burn_source *src)
{
  VerifySourcePriv *priv = (VerifySourcePriv *) src->data;
  if (priv && priv->inner && priv->inner->read == NULL && priv->inner->version >= 1 && priv->inner->cancel)
    return priv->inner->cancel (priv->inner);
  return 1;
}

static void
verify_source_free_data (struct burn_source *src)
{
  VerifySourcePriv *priv = (VerifySourcePriv *) src->data;
  if (priv) {
    if (priv->inner)
      burn_source_free (priv->inner);
    g_free (priv);
    src->data = NULL;
  }
}

XfburnVerifyData *
xfburn_verify_data_new (gint sector_size, off_t max_bytes)
{
  XfburnVerifyData *vdata = g_new0 (XfburnVerifyData, 1);
  guint initial_sectors = 0;

  if (max_bytes > 0 && sector_size > 0) {
    initial_sectors = (guint) ((max_bytes + sector_size - 1) / sector_size);
  }
  if (initial_sectors == 0) {
    initial_sectors = 4096;
  }

  vdata->sector_hashes = g_array_sized_new (FALSE, FALSE, sizeof (guint64), initial_sectors);
  vdata->track_sector_counts = g_array_new (FALSE, TRUE, sizeof (guint));
  vdata->sector_size = sector_size;
  vdata->partial_buf = g_malloc0 (sector_size);
  vdata->partial_len = 0;
  vdata->max_bytes = max_bytes;
  vdata->total_bytes_seen = 0;

  return vdata;
}

void
xfburn_verify_data_free (XfburnVerifyData *verify_data)
{
  if (verify_data == NULL)
    return;

  if (verify_data->sector_hashes)
    g_array_free (verify_data->sector_hashes, TRUE);
  if (verify_data->track_sector_counts)
    g_array_free (verify_data->track_sector_counts, TRUE);
  g_free (verify_data->partial_buf);
  g_free (verify_data);
}

struct burn_source *
xfburn_verify_wrap_source (struct burn_source *inner, gboolean swap, XfburnVerifyData *verify_data)
{
  struct burn_source *wrapper;
  VerifySourcePriv *priv;
  guint zero = 0;

  g_return_val_if_fail (inner != NULL, NULL);
  g_return_val_if_fail (verify_data != NULL, NULL);

  wrapper = calloc (1, sizeof (struct burn_source));
  if (wrapper == NULL)
    return NULL;

  priv = g_new0 (VerifySourcePriv, 1);
  inner->refcount++;
  priv->inner = inner;
  priv->swap = swap;
  priv->verify_data = verify_data;
  priv->track_idx = verify_data->track_sector_counts->len;
  g_array_append_val (verify_data->track_sector_counts, zero);

  wrapper->refcount = 1;
  wrapper->read = NULL;
  wrapper->read_sub = NULL;
  wrapper->get_size = verify_source_get_size;
  wrapper->set_size = verify_source_set_size;
  wrapper->free_data = verify_source_free_data;
  wrapper->next = NULL;
  wrapper->data = priv;
  wrapper->version = 1;
  wrapper->read_xt = verify_source_read;
  wrapper->cancel = verify_source_cancel;

  return wrapper;
}

/*************/
/* internals */
/*************/

static const gchar *
drive_status_to_string (enum burn_drive_status st)
{
  switch (st) {
    case BURN_DRIVE_IDLE:
      return "IDLE";
    case BURN_DRIVE_SPAWNING:
      return "SPAWNING";
    case BURN_DRIVE_READING:
      return "READING";
    case BURN_DRIVE_WRITING:
      return "WRITING";
    case BURN_DRIVE_WRITING_LEADIN:
      return "WRITING_LEADIN";
    case BURN_DRIVE_WRITING_LEADOUT:
      return "WRITING_LEADOUT";
    case BURN_DRIVE_ERASING:
      return "ERASING";
    case BURN_DRIVE_GRABBING:
      return "GRABBING";
    case BURN_DRIVE_WRITING_PREGAP:
      return "WRITING_PREGAP";
    case BURN_DRIVE_CLOSING_TRACK:
      return "CLOSING_TRACK";
    case BURN_DRIVE_CLOSING_SESSION:
      return "CLOSING_SESSION";
    case BURN_DRIVE_FORMATTING:
      return "FORMATTING";
    default:
      return "UNKNOWN";
  }
}

/* ts B40225 : Automatically format unformatted BD-RE and DVD-RAM
   Return: <=-1 failure, 0 = aborted, 1 = formatting happened, 2 = no formatting needed
*/
static int
xfburn_auto_format(GtkWidget *dialog_progress, struct burn_drive *drive)
{
  int ret, profile = 0, status = 0, num_formats = 0;
  char profile_name[80];
  off_t size = 0;
  unsigned int dummy = 0;
  struct burn_progress p;
  double percent;
  int format_flag = 64 | (3 << 1); /* Fast formatting with default size */
  gboolean stop;
  gboolean stopping = FALSE;
  gint64 t_call;
  gint tick_count = 0;

  profile_name[0] = '\0';

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_get_profile", "auto_format_check");
  ret = burn_disc_get_profile (drive, &profile, profile_name);
  XFBURN_LOG_CALL_EXIT ("burn_disc_get_profile", t_call, "ret=%d profile=0x%x name=\"%s\"", ret, profile, profile_name);

#ifdef XFBURN_DVD_PLUS_RW_FOR_BD_RE
  if (profile == 0x1a) {
    DBG("Pretending DVD+RW is a BD RE");
    profile = 0x43;           /* Pretend (for this function only) to be BD-RE */
    format_flag |= 16;        /* Re-format already formatted medium */
    g_strlcpy (profile_name, "DVD+RW as BD-RE", sizeof (profile_name));
  }
#endif

  if (ret <= 0 || (profile != 0x12 && profile != 0x43)) {
    return 2;
  }
  /* The medium is DVD-RAM or BD-RE */
  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_get_formats", "profile=0x%x", profile);
  ret = burn_disc_get_formats (drive, &status, &size, &dummy, &num_formats);
  XFBURN_LOG_CALL_EXIT ("burn_disc_get_formats", t_call, "ret=%d status=%d size=%" G_GINT64_FORMAT " num_formats=%d",
                        ret, status, (gint64) size, num_formats);

#ifdef XFBURN_DVD_PLUS_RW_FOR_BD_RE
  if (strncmp(profile_name, "DVD+RW", 6) == 0) {
    DBG("Pretending to be unformatted");
    ret = 1;
    status = BURN_FORMAT_IS_UNFORMATTED;
  }
#endif
#ifdef XFBURN_PRETEND_UNFORMATTED
  DBG("Pretending to be unformatted (using re-format)");
  status = BURN_FORMAT_IS_UNFORMATTED;
  format_flag |= 16;        /* Re-format already formatted medium */
#endif

  if (ret <= 0 || status != BURN_FORMAT_IS_UNFORMATTED) {
    return 2;
  }

  /* The medium is unformatted */
  XFBURN_LOG_INFO ("STATE", "Auto-formatting unformatted medium profile=0x%x (\"%s\") format_flag=0x%x",
                   profile, profile_name, format_flag);

  xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                               XFBURN_PROGRESS_DIALOG_STATUS_FORMATTING,
                                               _("Formatting..."));

#ifdef XFBURN_KEEP_UNFORMATTED
  g_warning ("Will not Format");
  return 0;
#endif

  /* Apply formatting */
  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_format", "size=0 flag=0x%x", format_flag);
  burn_disc_format (drive, 0, format_flag);
  XFBURN_LOG_CALL_EXIT ("burn_disc_format", t_call, "started");

  while (burn_drive_get_status (drive, &p) != BURN_DRIVE_IDLE) {
    xfburn_log_drain_backend_messages ("auto_format_loop", NULL, 0);

    if (p.sectors > 0 && p.sector >= 0) {
      /* display 1 to 99 percent */
      percent = 1.0 + ((double) p.sector + 1.0) / ((double) p.sectors) * 98.0;
      xfburn_progress_dialog_set_progress_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                        percent);
      if ((tick_count % 4) == 0) {
        XFBURN_LOG_INFO ("PROG", "phase=\"auto_format\" sector=%d/%d", p.sector, p.sectors);
      }
    }

    g_object_get (G_OBJECT (dialog_progress), "stop", &stop, NULL);

    if (stop && !stopping) {
      DBG ("cancelling...");
      XFBURN_LOG_WARN ("STATE", "User requested cancel during auto-format");
      stopping = TRUE;
      xfburn_progress_dialog_set_status (XFBURN_PROGRESS_DIALOG (dialog_progress), XFBURN_PROGRESS_DIALOG_STATUS_STOPPING);
    }

    tick_count++;
    usleep (500000);
  }
  xfburn_log_drain_backend_messages ("auto_format_end", NULL, 0);

  /* Check for success */
  if (burn_drive_wrote_well (drive)) {
    DBG ("Formatting done");
    XFBURN_LOG_INFO ("STATE", "Auto-formatting completed successfully (stopping=%d)", stopping);

    if (stopping) {
      return 0;
    }

    percent = 100.0;

    xfburn_progress_dialog_set_progress_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                      percent);
  } else {
    DBG ("Formatting failed");
    XFBURN_LOG_ERROR ("STATE", "Auto-formatting failed (burn_drive_wrote_well=0)");

    return -1;
  }
  return 1;
}


/**************/
/* public API */
/**************/
gboolean
xfburn_set_write_mode (struct burn_write_opts *opts, XfburnWriteMode write_mode, struct burn_disc *disc,
                       XfburnWriteMode fallback)
{
  g_assert (fallback != WRITE_MODE_AUTO);

  /* set all other burn_write_opts before calling this!
   * See docs on burn_write_opts_auto_write_type
   */

  /* keep all modes here, just in case we want to change the default sometimes */
  if (write_mode == WRITE_MODE_AUTO) {
    char reason[BURN_REASONS_LEN];
    enum burn_write_types mode;

    reason[0] = '\0';
    mode = burn_write_opts_auto_write_type (opts, disc, reason, 0);
    xfburn_log_drain_backend_messages ("auto_write_type", NULL, 0);

    if (mode != BURN_WRITE_NONE) {
      DBG("Automatically selected burn mode %d", mode);
      XFBURN_LOG_INFO ("JOB_OPTS", "Auto-selected write_type=%d reason=\"%s\"", mode, reason);

      /* write mode set, we're done */
      return TRUE;
    } else {
      XFBURN_LOG_WARN ("JOB_OPTS", "Could not auto-select write mode (reason=\"%s\"), falling back to %d",
                       reason, fallback);
      g_warning ("Could not automatically determine write mode: %s", reason);
      DBG ("This could be bad. Falling back to default %d.", fallback);

      write_mode = fallback;
      /* fall through to set opts up */
    }
  }

  XFBURN_LOG_INFO ("JOB_OPTS", "Setting explicit write_mode=%d", write_mode);

  switch (write_mode) {
  case WRITE_MODE_AUTO:
    g_error ("WRITE_MODE_AUTO not valid at this point.");
    break;
  case WRITE_MODE_TAO:
    burn_write_opts_set_write_type (opts, BURN_WRITE_TAO, BURN_BLOCK_MODE1);
    break;
  case WRITE_MODE_SAO:
    burn_write_opts_set_write_type (opts, BURN_WRITE_SAO, BURN_BLOCK_SAO);
    break;
  case WRITE_MODE_RAW16:
    burn_write_opts_set_write_type (opts, BURN_WRITE_RAW, BURN_BLOCK_RAW16);
    break;
  case WRITE_MODE_RAW96P:
    burn_write_opts_set_write_type (opts, BURN_WRITE_RAW, BURN_BLOCK_RAW96P);
    break;
  case WRITE_MODE_RAW96R:
    burn_write_opts_set_write_type (opts, BURN_WRITE_RAW, BURN_BLOCK_RAW96R);
    break;
  default:
    return FALSE;
  }

  return TRUE;
}

static gchar *
xfburn_format_failed_sectors (GArray *failed_sectors)
{
  GString *str = g_string_new (NULL);
  guint i = 0;
  guint ranges_shown = 0;
  const guint max_ranges = 24;

  while (i < failed_sectors->len) {
    guint start = g_array_index (failed_sectors, guint, i);
    guint end = start;

    while (i + 1 < failed_sectors->len &&
           g_array_index (failed_sectors, guint, i + 1) == end + 1) {
      end++;
      i++;
    }

    if (ranges_shown < max_ranges) {
      if (ranges_shown > 0)
        g_string_append (str, ", ");
      if (start == end)
        g_string_append_printf (str, "%u", start);
      else
        g_string_append_printf (str, "%u-%u", start, end);
    }
    ranges_shown++;
    i++;
  }

  if (ranges_shown > max_ranges)
    g_string_append_printf (str, " ... (%u %s)", failed_sectors->len, _("sectors total"));

  return g_string_free (str, FALSE);
}

static gboolean
xfburn_perform_burn_verify (GtkWidget *dialog_progress,
                            struct burn_drive *drive,
                            int factor,
                            XfburnVerifyData *verify_data,
                            gboolean *stopped,
                            gchar **verify_failed_msg)
{
  guint total_verify_sectors;
  guint verified_sectors = 0;
  gint sector_size;
  gint chunk_max_sectors;
  guchar *buf = NULL;
  GArray *failed_sectors = NULL;
  gint64 time_start_us;
  gint64 last_log_us;
  gint64 t_call;
  gint64 wait_start_us;
  gint64 last_watchdog_us;
  char drive_addr[BURN_DRIVE_ADR_LEN];
  gboolean stop = FALSE;
  gboolean success = TRUE;
  /* bit1 (2): suppress libburn error message on read error
   * bit2 (4): do NOT retry on error with single-block steps inside libburn
   * bit4 (16): do NOT retry on ILLEGAL MODE FOR THIS TRACK */
  const int read_flags = (1 << 1) | (1 << 2) | (1 << 4);

  *stopped = FALSE;
  *verify_failed_msg = NULL;

  if (verify_data == NULL) {
    return TRUE;
  }

  if (verify_data->partial_len > 0) {
    xfburn_verify_commit_partial_sector (verify_data);
  }

  total_verify_sectors = verify_data->sector_hashes->len;
  if (total_verify_sectors == 0) {
    return TRUE;
  }

  sector_size = verify_data->sector_size;
  if (sector_size == DATA_BYTES_PER_SECTOR) {
    chunk_max_sectors = 32;
  } else {
    chunk_max_sectors = 16;
  }

  drive_addr[0] = '\0';
  if (burn_drive_d_get_adr (drive, drive_addr) <= 0) {
    g_strlcpy (drive_addr, "unknown", sizeof (drive_addr));
  }

  XFBURN_LOG_INFO ("VERIFY",
                   "Starting post-burn verification addr=\"%s\" total_sectors=%u sector_size=%d chunk_max=%d",
                   drive_addr, total_verify_sectors, sector_size, chunk_max_sectors);

  xfburn_progress_dialog_reset (XFBURN_PROGRESS_DIALOG (dialog_progress));
  xfburn_progress_dialog_set_speed_label (XFBURN_PROGRESS_DIALOG (dialog_progress), _("Estimated reading speed:"));
  xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
  xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("unused"));
  xfburn_progress_dialog_set_buffer_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
  xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                               XFBURN_PROGRESS_DIALOG_STATUS_RUNNING,
                                               _("Verifying..."));
  xfburn_progress_dialog_set_progress_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), 0.0);

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_drive_re_assess", "addr=\"%s\"", drive_addr);
  burn_drive_re_assess (drive, 0);
  XFBURN_LOG_CALL_EXIT ("burn_drive_re_assess", t_call, "done");
  xfburn_log_drain_backend_messages ("verify_re_assess", NULL, 0);

  wait_start_us = g_get_monotonic_time ();
  last_watchdog_us = wait_start_us;
  while (burn_drive_get_status (drive, NULL) != BURN_DRIVE_IDLE) {
    gint64 now_us = g_get_monotonic_time ();
    if (now_us - wait_start_us > 30 * G_USEC_PER_SEC &&
        now_us - last_watchdog_us >= 10 * G_USEC_PER_SEC) {
      XfburnDeviceIoCounters wd_cnt;
      xfburn_log_read_device_io_counters (drive_addr, NULL, &wd_cnt);
      XFBURN_LOG_WARN ("WATCHDOG",
                       "Verify re-assess wait >30s addr=\"%s\" elapsed_s=%ld dev_state=\"%s\" ioreq=%" G_GINT64_FORMAT " iodone=%" G_GINT64_FORMAT " ioerr=%" G_GINT64_FORMAT,
                       drive_addr,
                       (long) ((now_us - wait_start_us) / G_USEC_PER_SEC),
                       wd_cnt.state, wd_cnt.iorequest_cnt, wd_cnt.iodone_cnt, wd_cnt.ioerr_cnt);
      last_watchdog_us = now_us;
    }
    usleep (100001);
  }

  burn_drive_set_speed (drive, 0, 0);

  buf = g_malloc ((gsize) chunk_max_sectors * (gsize) sector_size);
  failed_sectors = g_array_new (FALSE, FALSE, sizeof (guint));
  time_start_us = g_get_monotonic_time ();
  last_log_us = time_start_us;

  if (sector_size == DATA_BYTES_PER_SECTOR) {
    int start_lba = 0;
    int nwa = 0;
    guint s = 0;

    if (burn_disc_track_lba_nwa (drive, NULL, 1, &start_lba, &nwa) <= 0 || start_lba < 0) {
      start_lba = 0;
    }

    XFBURN_LOG_INFO ("VERIFY", "Data track verification start_lba=%d nwa=%d total_sectors=%u",
                     start_lba, nwa, total_verify_sectors);

    while (s < total_verify_sectors) {
      guint chunk_sectors = MIN ((guint) chunk_max_sectors, total_verify_sectors - s);
      off_t chunk_bytes = (off_t) chunk_sectors * DATA_BYTES_PER_SECTOR;
      off_t data_count = 0;
      int ret;
      guint good_sectors;
      guint k;
      gdouble percent;
      gdouble elapsed_sec;
      gint64 now_us;

      g_object_get (G_OBJECT (dialog_progress), "stop", &stop, NULL);
      if (stop) {
        *stopped = TRUE;
        XFBURN_LOG_WARN ("VERIFY", "User cancelled data verification at sector %u/%u", s, total_verify_sectors);
        xfburn_progress_dialog_set_status (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                           XFBURN_PROGRESS_DIALOG_STATUS_STOPPING);
        break;
      }

      ret = burn_read_data (drive,
                            (off_t) (start_lba + (int) s) * DATA_BYTES_PER_SECTOR,
                            (char *) buf,
                            chunk_bytes,
                            &data_count,
                            read_flags);

      if (ret > 0 && data_count >= chunk_bytes) {
        good_sectors = chunk_sectors;
      } else if (data_count > 0) {
        good_sectors = (guint) (data_count / DATA_BYTES_PER_SECTOR);
      } else {
        good_sectors = 0;
      }

      for (k = 0; k < good_sectors; k++) {
        guint64 expected_h = g_array_index (verify_data->sector_hashes, guint64, s + k);
        guint64 actual_h = xfburn_hash_sector (buf + (gsize) k * DATA_BYTES_PER_SECTOR, DATA_BYTES_PER_SECTOR);
        if (actual_h != expected_h) {
          guint bad_lba = (guint) (start_lba + (int) (s + k));
          XFBURN_LOG_ERROR ("VERIFY",
                            "Hash mismatch at LBA %u (sector %u): expected=0x%016" G_GINT64_MODIFIER "x actual=0x%016" G_GINT64_MODIFIER "x",
                            bad_lba, s + k, expected_h, actual_h);
          g_array_append_val (failed_sectors, bad_lba);
        }
      }

      if (good_sectors < chunk_sectors) {
        /* Sector at s + good_sectors failed on read: do not retry it, record and skip */
        guint bad_k = good_sectors;
        guint bad_lba = (guint) (start_lba + (int) (s + bad_k));
        XfburnDeviceIoCounters err_cnt;

        xfburn_log_drain_backend_messages ("verify_read_err", NULL, 0);
        xfburn_log_read_device_io_counters (drive_addr, NULL, &err_cnt);
        XFBURN_LOG_ERROR ("VERIFY",
                          "Read error at LBA %u: ret=%d data_count=%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " dev_state=\"%s\" ioerr_cnt=%" G_GINT64_FORMAT,
                          bad_lba, ret, (gint64) data_count, (gint64) chunk_bytes,
                          err_cnt.state, err_cnt.ioerr_cnt);
        g_array_append_val (failed_sectors, bad_lba);

        /* Probe remaining sectors in this chunk one sector at a time (without retrying) */
        for (k = bad_k + 1; k < chunk_sectors; k++) {
          off_t single_count = 0;
          int ret_single;
          guint cur_lba = (guint) (start_lba + (int) (s + k));

          g_object_get (G_OBJECT (dialog_progress), "stop", &stop, NULL);
          if (stop) {
            *stopped = TRUE;
            xfburn_progress_dialog_set_status (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                               XFBURN_PROGRESS_DIALOG_STATUS_STOPPING);
            break;
          }

          ret_single = burn_read_data (drive,
                                       (off_t) cur_lba * DATA_BYTES_PER_SECTOR,
                                       (char *) buf,
                                       DATA_BYTES_PER_SECTOR,
                                       &single_count,
                                       read_flags);
          if (ret_single <= 0 || single_count != DATA_BYTES_PER_SECTOR) {
            XFBURN_LOG_ERROR ("VERIFY",
                              "Single-sector read error at LBA %u: ret=%d single_count=%" G_GINT64_FORMAT,
                              cur_lba, ret_single, (gint64) single_count);
            g_array_append_val (failed_sectors, cur_lba);
          } else {
            guint64 expected_h = g_array_index (verify_data->sector_hashes, guint64, s + k);
            guint64 actual_h = xfburn_hash_sector (buf, DATA_BYTES_PER_SECTOR);
            if (actual_h != expected_h) {
              XFBURN_LOG_ERROR ("VERIFY",
                                "Single-sector hash mismatch at LBA %u: expected=0x%016" G_GINT64_MODIFIER "x actual=0x%016" G_GINT64_MODIFIER "x",
                                cur_lba, expected_h, actual_h);
              g_array_append_val (failed_sectors, cur_lba);
            }
          }
        }

        if (*stopped) {
          break;
        }
      }

      s += chunk_sectors;
      verified_sectors = s;

      percent = ((gdouble) verified_sectors / (gdouble) total_verify_sectors) * 100.0;
      if (percent >= 100.0) {
        percent = 99.9;
      }
      xfburn_progress_dialog_set_progress_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), percent);

      now_us = g_get_monotonic_time ();
      elapsed_sec = (gdouble) (now_us - time_start_us) / 1000000.0;
      if (elapsed_sec >= 0.25 && verified_sectors > 0 && factor > 0) {
        gdouble cur_speed = (((gdouble) verified_sectors * (gdouble) DATA_BYTES_PER_SECTOR) / elapsed_sec) /
                            ((gdouble) (factor * 1000.0));
        xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), (gfloat) cur_speed);
      }

      if (now_us - last_log_us >= 2 * G_USEC_PER_SEC) {
        XFBURN_LOG_INFO ("VERIFY",
                         "progress verified_sectors=%u/%u failed_sectors=%u",
                         verified_sectors, total_verify_sectors, failed_sectors->len);
        last_log_us = now_us;
      }
    }
  } else {
    /* Audio CD verification */
    guint n_tracks = verify_data->track_sector_counts->len;
    guint t;
    guint global_s = 0;
    int fallback_lba = 0;
    struct burn_disc *read_disc = burn_drive_get_disc (drive);
    struct burn_session **read_sessions = NULL;
    int n_read_sessions = 0;
    struct burn_track **read_tracks = NULL;
    int n_read_tracks = 0;
    guchar zero_sector[AUDIO_BYTES_PER_SECTOR];
    guint64 zero_sector_hash;

    memset (zero_sector, 0, sizeof (zero_sector));
    zero_sector_hash = xfburn_hash_sector (zero_sector, AUDIO_BYTES_PER_SECTOR);

    if (read_disc != NULL) {
      read_sessions = burn_disc_get_sessions (read_disc, &n_read_sessions);
      if (read_sessions != NULL && n_read_sessions > 0) {
        read_tracks = burn_session_get_tracks (read_sessions[n_read_sessions - 1], &n_read_tracks);
      }
    }

    for (t = 0; t < n_tracks && !(*stopped); t++) {
      guint t_sectors = g_array_index (verify_data->track_sector_counts, guint, t);
      guint disc_t_sectors = t_sectors;
      int track_lba = fallback_lba;
      int nwa = 0;
      guint ts = 0;

      if (t_sectors == 0) {
        continue;
      }

      if (read_tracks != NULL && (int) t < n_read_tracks) {
        struct burn_toc_entry toc_entry;
        memset (&toc_entry, 0, sizeof (toc_entry));
        burn_track_get_entry (read_tracks[t], &toc_entry);
        track_lba = toc_entry.start_lba;
        if (toc_entry.track_blocks > (int) t_sectors) {
          disc_t_sectors = (guint) toc_entry.track_blocks;
        }
      } else if (burn_disc_track_lba_nwa (drive, NULL, (int) t + 1, &track_lba, &nwa) <= 0) {
        track_lba = fallback_lba;
      }

      XFBURN_LOG_INFO ("VERIFY",
                       "Audio track %u/%u start_lba=%d sectors=%u disc_sectors=%u",
                       t + 1, n_tracks, track_lba, t_sectors, disc_t_sectors);

      if (n_tracks > 1) {
        gchar *str = g_strdup_printf (_("Verifying track %2u/%u..."), t + 1, n_tracks);
        xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                     XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, str);
        g_free (str);
      }

      while (ts < disc_t_sectors) {
        guint chunk_sectors = MIN ((guint) chunk_max_sectors, disc_t_sectors - ts);
        off_t chunk_bytes = (off_t) chunk_sectors * AUDIO_BYTES_PER_SECTOR;
        off_t data_count = 0;
        int ret;
        guint good_sectors;
        guint k;
        gdouble percent;
        gdouble elapsed_sec;
        gint64 now_us;

        g_object_get (G_OBJECT (dialog_progress), "stop", &stop, NULL);
        if (stop) {
          *stopped = TRUE;
          XFBURN_LOG_WARN ("VERIFY", "User cancelled audio verification at track %u sector %u", t + 1, ts);
          xfburn_progress_dialog_set_status (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                             XFBURN_PROGRESS_DIALOG_STATUS_STOPPING);
          break;
        }

        ret = burn_read_audio (drive,
                               track_lba + (int) ts,
                               (char *) buf,
                               chunk_bytes,
                               &data_count,
                               read_flags);

        if (ret > 0 && data_count >= chunk_bytes) {
          good_sectors = chunk_sectors;
        } else if (data_count > 0) {
          good_sectors = (guint) (data_count / AUDIO_BYTES_PER_SECTOR);
        } else {
          good_sectors = 0;
        }

        for (k = 0; k < good_sectors; k++) {
          guint64 expected_h;
          guint64 actual_h;

          if (ts + k < t_sectors) {
            expected_h = g_array_index (verify_data->sector_hashes, guint64, global_s + ts + k);
          } else {
            expected_h = zero_sector_hash;
          }
          actual_h = xfburn_hash_sector (buf + (gsize) k * AUDIO_BYTES_PER_SECTOR, AUDIO_BYTES_PER_SECTOR);
          if (actual_h != expected_h) {
            guint bad_lba = (guint) MAX (0, track_lba + (int) (ts + k));
            XFBURN_LOG_ERROR ("VERIFY",
                              "Audio hash mismatch at LBA %u (track %u): expected=0x%016" G_GINT64_MODIFIER "x actual=0x%016" G_GINT64_MODIFIER "x",
                              bad_lba, t + 1, expected_h, actual_h);
            g_array_append_val (failed_sectors, bad_lba);
          }
        }

        if (good_sectors < chunk_sectors) {
          guint bad_k = good_sectors;
          guint bad_lba = (guint) MAX (0, track_lba + (int) (ts + bad_k));
          XfburnDeviceIoCounters err_cnt;

          xfburn_log_drain_backend_messages ("verify_audio_read_err", NULL, 0);
          xfburn_log_read_device_io_counters (drive_addr, NULL, &err_cnt);
          XFBURN_LOG_ERROR ("VERIFY",
                            "Audio read error at LBA %u: ret=%d data_count=%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " dev_state=\"%s\" ioerr_cnt=%" G_GINT64_FORMAT,
                            bad_lba, ret, (gint64) data_count, (gint64) chunk_bytes,
                            err_cnt.state, err_cnt.ioerr_cnt);
          g_array_append_val (failed_sectors, bad_lba);

          for (k = bad_k + 1; k < chunk_sectors; k++) {
            off_t single_count = 0;
            int ret_single;
            int cur_lba_signed = track_lba + (int) (ts + k);
            guint cur_lba = (guint) MAX (0, cur_lba_signed);

            g_object_get (G_OBJECT (dialog_progress), "stop", &stop, NULL);
            if (stop) {
              *stopped = TRUE;
              xfburn_progress_dialog_set_status (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                 XFBURN_PROGRESS_DIALOG_STATUS_STOPPING);
              break;
            }

            ret_single = burn_read_audio (drive,
                                          cur_lba_signed,
                                          (char *) buf,
                                          AUDIO_BYTES_PER_SECTOR,
                                          &single_count,
                                          read_flags);
            if (ret_single <= 0 || single_count != AUDIO_BYTES_PER_SECTOR) {
              XFBURN_LOG_ERROR ("VERIFY",
                                "Single audio sector read error at LBA %u: ret=%d single_count=%" G_GINT64_FORMAT,
                                cur_lba, ret_single, (gint64) single_count);
              g_array_append_val (failed_sectors, cur_lba);
            } else {
              guint64 expected_h;
              guint64 actual_h;

              if (ts + k < t_sectors) {
                expected_h = g_array_index (verify_data->sector_hashes, guint64, global_s + ts + k);
              } else {
                expected_h = zero_sector_hash;
              }
              actual_h = xfburn_hash_sector (buf, AUDIO_BYTES_PER_SECTOR);
              if (actual_h != expected_h) {
                XFBURN_LOG_ERROR ("VERIFY",
                                  "Single audio sector hash mismatch at LBA %u: expected=0x%016" G_GINT64_MODIFIER "x actual=0x%016" G_GINT64_MODIFIER "x",
                                  cur_lba, expected_h, actual_h);
                g_array_append_val (failed_sectors, cur_lba);
              }
            }
          }

          if (*stopped) {
            break;
          }
        }

        ts += chunk_sectors;
        verified_sectors = global_s + MIN (ts, t_sectors);

        percent = ((gdouble) verified_sectors / (gdouble) total_verify_sectors) * 100.0;
        if (percent >= 100.0) {
          percent = 99.9;
        }
        xfburn_progress_dialog_set_progress_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), percent);

        now_us = g_get_monotonic_time ();
        elapsed_sec = (gdouble) (now_us - time_start_us) / 1000000.0;
        if (elapsed_sec >= 0.25 && verified_sectors > 0 && factor > 0) {
          gdouble cur_speed = (((gdouble) verified_sectors * (gdouble) AUDIO_BYTES_PER_SECTOR) / elapsed_sec) /
                              ((gdouble) (factor * 1000.0));
          xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), (gfloat) cur_speed);
        }

        if (now_us - last_log_us >= 2 * G_USEC_PER_SEC) {
          XFBURN_LOG_INFO ("VERIFY",
                           "audio progress verified_sectors=%u/%u failed_sectors=%u",
                           verified_sectors, total_verify_sectors, failed_sectors->len);
          last_log_us = now_us;
        }
      }

      global_s += t_sectors;
      fallback_lba = track_lba + (int) disc_t_sectors;
    }

    if (read_disc != NULL) {
      burn_disc_free (read_disc);
    }
  }

  /* Drain and log any pending libburn messages from read probes */
  xfburn_log_drain_backend_messages ("verify_read", NULL, 0);

  if (!(*stopped)) {
    xfburn_progress_dialog_set_progress_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), 100.0);

    if (failed_sectors->len == 0) {
      success = TRUE;
      XFBURN_LOG_INFO ("VERIFY",
                       "Verification completed successfully: verified_sectors=%u/%u elapsed_s=%ld",
                       verified_sectors, total_verify_sectors,
                       (long) ((g_get_monotonic_time () - time_start_us) / G_USEC_PER_SEC));
    } else {
      gchar *sectors_str = xfburn_format_failed_sectors (failed_sectors);
      *verify_failed_msg = g_strdup_printf (_("Disc Read Error: %s"), sectors_str);
      XFBURN_LOG_ERROR ("VERIFY",
                        "Verification FAILED: failed_sectors=%u/%u ranges=\"%s\"",
                        failed_sectors->len, total_verify_sectors, sectors_str);
      g_free (sectors_str);
      success = FALSE;
    }
  } else {
    XFBURN_LOG_WARN ("VERIFY", "Verification aborted by user at sector %u/%u",
                     verified_sectors, total_verify_sectors);
    success = FALSE;
  }

  g_array_free (failed_sectors, TRUE);
  g_free (buf);

  return success;
}

void
xfburn_perform_burn_write (GtkWidget *dialog_progress,
                           struct burn_drive *drive, XfburnWriteMode write_mode, struct burn_write_opts *burn_options, int sector_size,
                           struct burn_disc *disc, struct burn_source **fifos, int *track_sectors,
                           XfburnVerifyData *verify_data)
{
  enum burn_disc_status disc_state;
  enum burn_drive_status status;
  enum burn_drive_status prev_status = BURN_DRIVE_IDLE;
  struct burn_progress progress;
  time_t time_start;
  char media_name[80];
  char drive_addr[BURN_DRIVE_ADR_LEN];
  int media_no = 0;
  int factor;

  int ret;
  int wrote_well;
  char msg_text[BURN_MSGS_MESSAGE_LEN];
  const char *final_status_text;
  XfburnProgressDialogStatus final_status;
  char *final_message;
  gdouble percent = 0.0;
  int total_sectors, burned_sectors;
  off_t disc_size;
  int disc_sectors;
  gboolean stopping = FALSE;
  gint64 t_call;
  gint64 wait_start_us;
  gint64 last_watchdog_us;
  gint64 burn_start_us;
  gint64 last_heartbeat_us;
  gint64 last_sector_advance_us;
  int last_progress_sector = -1;
  gint64 prev_ioerr_cnt = -1;
  XfburnDeviceIoCounters io_counters;

  memset (&progress, 0, sizeof (progress));
  media_name[0] = '\0';
  drive_addr[0] = '\0';
  if (burn_drive_d_get_adr (drive, drive_addr) <= 0) {
    g_strlcpy (drive_addr, "unknown", sizeof (drive_addr));
  }

  xfburn_log_dump_device_hardware (drive_addr, NULL);
  if (xfburn_log_read_device_io_counters (drive_addr, NULL, &io_counters)) {
    prev_ioerr_cnt = io_counters.ioerr_cnt;
  }

  wait_start_us = g_get_monotonic_time ();
  last_watchdog_us = wait_start_us;
  while (burn_drive_get_status (drive, NULL) != BURN_DRIVE_IDLE) {
    gint64 now_us = g_get_monotonic_time ();
    if (now_us - wait_start_us > 30 * G_USEC_PER_SEC &&
        now_us - last_watchdog_us >= 10 * G_USEC_PER_SEC) {
      xfburn_log_read_device_io_counters (drive_addr, NULL, &io_counters);
      XFBURN_LOG_WARN ("WATCHDOG",
                       "Pre-burn drive IDLE wait >30s addr=\"%s\" elapsed_s=%ld dev_state=\"%s\" ioreq=%" G_GINT64_FORMAT " iodone=%" G_GINT64_FORMAT " ioerr=%" G_GINT64_FORMAT,
                       drive_addr,
                       (long) ((now_us - wait_start_us) / G_USEC_PER_SEC),
                       io_counters.state, io_counters.iorequest_cnt, io_counters.iodone_cnt, io_counters.ioerr_cnt);
      last_watchdog_us = now_us;
    }
    usleep(100001);
  }

  /* retrieve media type, so we can convert from 'kb/s' into 'x' rating */
  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_get_profile", "addr=\"%s\"", drive_addr);
  ret = burn_disc_get_profile (drive, &media_no, media_name);
  XFBURN_LOG_CALL_EXIT ("burn_disc_get_profile", t_call, "ret=%d media_no=0x%x media_name=\"%s\"",
                        ret, media_no, media_name);
  if (ret == 1) {
    factor = xfburn_media_profile_to_kb (media_no);
  } else {
    g_warning ("no profile could be retrieved to calculate current burn speed");
    factor = 1;
  }

  /* Evaluate drive and media */
  wait_start_us = g_get_monotonic_time ();
  last_watchdog_us = wait_start_us;
  while ((disc_state = burn_disc_get_status(drive)) == BURN_DISC_UNREADY) {
    gint64 now_us = g_get_monotonic_time ();
    if (now_us - wait_start_us > 30 * G_USEC_PER_SEC &&
        now_us - last_watchdog_us >= 10 * G_USEC_PER_SEC) {
      xfburn_log_read_device_io_counters (drive_addr, NULL, &io_counters);
      XFBURN_LOG_WARN ("WATCHDOG",
                       "Pre-burn disc UNREADY wait >30s addr=\"%s\" elapsed_s=%ld dev_state=\"%s\" ioerr=%" G_GINT64_FORMAT,
                       drive_addr,
                       (long) ((now_us - wait_start_us) / G_USEC_PER_SEC),
                       io_counters.state, io_counters.ioerr_cnt);
      last_watchdog_us = now_us;
    }
    usleep(100001);
  }

  xfburn_log_drain_backend_messages ("pre_burn_eval", NULL, 0);

  if (disc_state == BURN_DISC_APPENDABLE && write_mode != WRITE_MODE_TAO) {
    XFBURN_LOG_ERROR ("JOB_START", "Cannot append to multisession disc with write_mode=%d (need TAO)", write_mode);
    xfburn_progress_dialog_burning_failed (XFBURN_PROGRESS_DIALOG (dialog_progress), _("Cannot append data to multisession disc in this write mode (use TAO instead)."));
    return;
  } else if (disc_state != BURN_DISC_BLANK) {
    XFBURN_LOG_ERROR ("JOB_START", "Invalid disc_state=%d for burning on addr=\"%s\"", disc_state, drive_addr);
    if (disc_state == BURN_DISC_FULL) {
      xfburn_progress_dialog_burning_failed (XFBURN_PROGRESS_DIALOG (dialog_progress), _("Closed disc with data detected, a blank or appendable disc is needed."));
    } else if (disc_state == BURN_DISC_EMPTY) {
      xfburn_progress_dialog_burning_failed (XFBURN_PROGRESS_DIALOG (dialog_progress), _("No disc detected in drive."));
    } else {
      g_warning ("Cannot recognize state of drive and disc: disc_state = %d", disc_state);
      xfburn_progress_dialog_burning_failed (XFBURN_PROGRESS_DIALOG (dialog_progress), _("Cannot recognize the state of the drive and disc."));
    }
    return;
  }

  ret = xfburn_auto_format (dialog_progress, drive);
  if (ret < 0) {
    XFBURN_LOG_ERROR ("JOB_START", "xfburn_auto_format failed (ret=%d)", ret);
    xfburn_progress_dialog_burning_failed (XFBURN_PROGRESS_DIALOG (dialog_progress), _("Formatting failed."));
    return;
  } else if (ret == 0) {
    XFBURN_LOG_WARN ("JOB_START", "xfburn_auto_format cancelled by user");
    xfburn_progress_dialog_set_status (XFBURN_PROGRESS_DIALOG (dialog_progress), XFBURN_PROGRESS_DIALOG_STATUS_CANCELLED);
    return;
  } else if (ret == 1) {
    /* formatting happened, reset the dialog */
    xfburn_progress_dialog_reset (XFBURN_PROGRESS_DIALOG(dialog_progress));
  }

  total_sectors = burn_disc_get_sectors (disc);

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_available_space", "addr=\"%s\"", drive_addr);
  disc_size = burn_disc_available_space (drive, burn_options);
  XFBURN_LOG_CALL_EXIT ("burn_disc_available_space", t_call, "disc_size=%" G_GINT64_FORMAT, (gint64) disc_size);

  /* available_space assumes data tracks, so we'll just compare sectors */
  disc_sectors = disc_size / DATA_BYTES_PER_SECTOR;

  {
    int verify_flag = 0;
    if (verify_data != NULL) {
      verify_flag = 1;
    }
    XFBURN_LOG_INFO ("JOB_START",
                     "addr=\"%s\" media_no=0x%x media_name=\"%s\" disc_state=%d write_mode=%d sector_size=%d total_sectors=%d avail_sectors=%d avail_bytes=%" G_GINT64_FORMAT " verify=%d",
                     drive_addr, media_no, media_name, disc_state, write_mode, sector_size,
                     total_sectors, disc_sectors, (gint64) disc_size,
                     verify_flag);
  }

  if (disc_sectors < total_sectors) {
    XFBURN_LOG_ERROR ("JOB_START",
                      "Insufficient space on disc: total_sectors=%d > avail_sectors=%d",
                      total_sectors, disc_sectors);
    xfburn_progress_dialog_burning_failed (XFBURN_PROGRESS_DIALOG (dialog_progress), _("There is not enough space available on the inserted disc."));
    return;
  }

  /* set us up to receive libburn errors */
  xfburn_capture_libburn_messages ();

  /* no error message by default */
#ifdef DEBUG_LIBBURN
  g_strlcpy (msg_text, _("see console"), sizeof (msg_text));
#else
  msg_text[0] = '\0';
#endif

  /* Install the default libburn abort signal handler.
   * Hopefully this means the drive won't be left in a burning state if we catch a signal */
  burn_set_signal_handling (PACKAGE, NULL, 0);

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_write", "addr=\"%s\" total_sectors=%d", drive_addr, total_sectors);
  burn_disc_write (burn_options, disc);
  XFBURN_LOG_CALL_EXIT ("burn_disc_write", t_call, "dispatched");

  wait_start_us = g_get_monotonic_time ();
  last_watchdog_us = wait_start_us;
  while (burn_drive_get_status (drive, NULL) == BURN_DRIVE_SPAWNING) {
    gint64 now_us = g_get_monotonic_time ();
    if (now_us - wait_start_us > 30 * G_USEC_PER_SEC &&
        now_us - last_watchdog_us >= 10 * G_USEC_PER_SEC) {
      xfburn_log_read_device_io_counters (drive_addr, NULL, &io_counters);
      XFBURN_LOG_WARN ("WATCHDOG",
                       "BURN_DRIVE_SPAWNING wait >30s addr=\"%s\" elapsed_s=%ld dev_state=\"%s\" ioerr=%" G_GINT64_FORMAT,
                       drive_addr,
                       (long) ((now_us - wait_start_us) / G_USEC_PER_SEC),
                       io_counters.state, io_counters.ioerr_cnt);
      last_watchdog_us = now_us;
    }
    usleep(1002);
  }

  time_start = time (NULL);
  burn_start_us = g_get_monotonic_time ();
  last_heartbeat_us = burn_start_us;
  last_sector_advance_us = burn_start_us;
  last_watchdog_us = burn_start_us;
  burned_sectors = 0;

  while ((status = burn_drive_get_status (drive, &progress)) != BURN_DRIVE_IDLE) {
    time_t time_now = time (NULL);
    gint64 now_us = g_get_monotonic_time ();
    gboolean stop;
    gdouble cur_speed = 0.0;
    gdouble buf_pct = -1.0;
    int fifo_status = -1;
    int fifo_size = 0;
    int fifo_free = 0;

    xfburn_log_drain_backend_messages ("burn_loop", msg_text, sizeof (msg_text));

    if (status != prev_status) {
      XFBURN_LOG_INFO ("STATE", "Burn drive status transition: %s (%d) -> %s (%d) sector=%d/%d",
                       drive_status_to_string (prev_status), prev_status,
                       drive_status_to_string (status), status,
                       progress.sector, progress.sectors);
      prev_status = status;
      last_sector_advance_us = now_us;
    }

    switch (status) {
    case BURN_DRIVE_WRITING:
      /* Is this really thread safe? It would seems so, but it's not verified */
      g_object_get (G_OBJECT (dialog_progress), "stop", &stop, NULL);

      if (stop && !stopping) {
        DBG ("cancelling...");
        XFBURN_LOG_WARN ("STATE", "User requested burn cancellation at sector=%d/%d", progress.sector, progress.sectors);
        t_call = g_get_monotonic_time ();
        XFBURN_LOG_CALL_ENTER ("burn_drive_cancel", "addr=\"%s\"", drive_addr);
        burn_drive_cancel (drive);
        XFBURN_LOG_CALL_EXIT ("burn_drive_cancel", t_call, "cancelled");
        stopping = TRUE;
      }

      if (progress.sector != last_progress_sector) {
        last_progress_sector = progress.sector;
        last_sector_advance_us = now_us;
      } else if (now_us - last_sector_advance_us > 15 * G_USEC_PER_SEC &&
                 now_us - last_watchdog_us >= 5 * G_USEC_PER_SEC) {
        xfburn_log_read_device_io_counters (drive_addr, NULL, &io_counters);
        XFBURN_LOG_WARN ("WATCHDOG",
                         "Burn sector progress stalled for %lds at sector=%d/%d dev_state=\"%s\" ioreq=%" G_GINT64_FORMAT " iodone=%" G_GINT64_FORMAT " ioerr=%" G_GINT64_FORMAT " inflight=%" G_GINT64_FORMAT,
                         (long) ((now_us - last_sector_advance_us) / G_USEC_PER_SEC),
                         progress.sector, progress.sectors,
                         io_counters.state, io_counters.iorequest_cnt, io_counters.iodone_cnt,
                         io_counters.ioerr_cnt,
                         io_counters.iorequest_cnt - io_counters.iodone_cnt);
        last_watchdog_us = now_us;
      }

      if (progress.sectors > 0 && progress.sector >= 0) {
        char *fifo_text = NULL;

        if (stopping) {
          xfburn_progress_dialog_set_status (XFBURN_PROGRESS_DIALOG (dialog_progress), XFBURN_PROGRESS_DIALOG_STATUS_STOPPING);
        } else if (progress.tracks > 1) {
          gchar *str;

          str = g_strdup_printf (_("Burning track %2d/%d..."), progress.track+1, progress.tracks);
          xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress), XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, str);
          g_free (str);
        } else {
          xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress), XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, _("Burning composition..."));
        }

        if (progress.buffer_capacity > 0) {
          percent = (gdouble) (progress.buffer_capacity - progress.buffer_available) / (gdouble) progress.buffer_capacity * 100.0;
          buf_pct = percent;
        } else {
          percent = -1.0;
        }

        /* assume that we don't have buffer info if the result is outside the sane range */
        if (percent >= 0 && percent <= 100.0) {
          xfburn_progress_dialog_set_buffer_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), percent);
        }

        /* accumulate the sectors that have been burned in the previous track */
        if (progress.track > 0 && track_sectors[progress.track-1] > 0) {
          burned_sectors += track_sectors[progress.track-1];
          track_sectors[progress.track-1] = 0;
        }

        percent = 1.0 + ((gdouble) progress.sector + burned_sectors + 1.0) / ((gdouble) total_sectors) * 98.0;

        xfburn_progress_dialog_set_progress_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), percent);

        if (time_now > time_start && factor > 0) {
          cur_speed = ((gdouble) ((gdouble)(glong)progress.sector * 2048L) / (gdouble) (time_now - time_start)) / ((gdouble) (factor * 1000.0));
        }
        xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                  cur_speed);

        if (fifos != NULL) {
          fifo_status = burn_fifo_inquire_status (fifos[progress.track], &fifo_size, &fifo_free, &fifo_text);
          switch (fifo_status) {
            case 0:
              xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("standby"));
              break;
            case 1:
              /* active */
              percent = (gdouble) (fifo_size - fifo_free) / (gdouble) fifo_size * 100.0;
              xfburn_progress_dialog_set_fifo_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), percent);
              break;
            case 2:
              xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("ending"));
              break;
            case 3:
              xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("failing"));
              break;
            case 4:
              xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("unused"));
              break;
            case 5:
              xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("abandoned"));
              break;
            case 6:
              xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("ended"));
              break;
            case 7:
              xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("aborted"));
              break;
            default:
              xfburn_progress_dialog_set_fifo_bar_text (XFBURN_PROGRESS_DIALOG (dialog_progress), _("no info"));
              break;
          }
        }
      } else {
        xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress), XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, _("Burning composition..."));
      }
      break;
    case BURN_DRIVE_WRITING_LEADIN:
      xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                   XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, _("Writing Lead-In..."));
      xfburn_progress_dialog_set_buffer_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      break;
    case BURN_DRIVE_WRITING_LEADOUT:
      xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                   XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, _("Writing Lead-Out..."));
      xfburn_progress_dialog_set_buffer_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      break;
    case BURN_DRIVE_WRITING_PREGAP:
      xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                   XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, _("Writing pregap..."));
      xfburn_progress_dialog_set_buffer_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      break;
    case BURN_DRIVE_CLOSING_TRACK:
      xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                   XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, _("Closing track..."));
      xfburn_progress_dialog_set_buffer_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      break;
    case BURN_DRIVE_CLOSING_SESSION:
      xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                   XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, _("Closing session..."));
      xfburn_progress_dialog_set_buffer_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      break;
    case BURN_DRIVE_FORMATTING:
      xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress),
                                                   XFBURN_PROGRESS_DIALOG_STATUS_RUNNING, _("Formatting..."));
      xfburn_progress_dialog_set_buffer_bar_fraction (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      xfburn_progress_dialog_set_writing_speed (XFBURN_PROGRESS_DIALOG (dialog_progress), -1);
      break;
    default:
      DBG ("Status %d not supported", status);
      break;
    }

    if (now_us - last_heartbeat_us >= 2 * G_USEC_PER_SEC) {
      int speed_whole = (int) cur_speed;
      int speed_tenths = (int) ((cur_speed - (gdouble) speed_whole) * 10.0);
      int fifo_used_kb = 0;

      if (speed_tenths < 0) {
        speed_tenths = 0;
      }
      if (fifo_size > 0) {
        fifo_used_kb = (fifo_size - fifo_free) / 1024;
      }

      if (xfburn_log_read_device_io_counters (drive_addr, NULL, &io_counters)) {
        if (prev_ioerr_cnt >= 0 && io_counters.ioerr_cnt > prev_ioerr_cnt) {
          XFBURN_LOG_WARN ("IO_ERR",
                           "Kernel SCSI/USB I/O error count increased during burn! addr=\"%s\" ioerr_cnt=%" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT " dev_state=\"%s\" sector=%d/%d",
                           drive_addr, prev_ioerr_cnt, io_counters.ioerr_cnt,
                           io_counters.state, progress.sector, progress.sectors);
        }
        prev_ioerr_cnt = io_counters.ioerr_cnt;
      }

      XFBURN_LOG_INFO ("PROG",
                       "state=\"%s\" track=%d/%d sector=%d/%d total_sectors=%d speed=%d.%dx drive_buf_pct=%d drive_buf_min_kb=%d/%d fifo_status=%d fifo_used_kb=%d/%d ioreq=%" G_GINT64_FORMAT " iodone=%" G_GINT64_FORMAT " ioerr=%" G_GINT64_FORMAT " inflight=%" G_GINT64_FORMAT " dev_state=\"%s\"",
                       drive_status_to_string (status),
                       progress.track + 1, progress.tracks,
                       progress.sector, progress.sectors, total_sectors,
                       speed_whole, speed_tenths,
                       (int) buf_pct,
                       progress.buffer_min_fill / 1024, progress.buffer_capacity / 1024,
                       fifo_status,
                       fifo_used_kb,
                       fifo_size / 1024,
                       io_counters.iorequest_cnt,
                       io_counters.iodone_cnt,
                       io_counters.ioerr_cnt,
                       io_counters.iorequest_cnt - io_counters.iodone_cnt,
                       io_counters.state);
      last_heartbeat_us = now_us;
    }

    usleep (500000);
  }

  XFBURN_LOG_INFO ("STATE", "Burn drive status transition: %s (%d) -> IDLE (0)",
                   drive_status_to_string (prev_status), prev_status);

  /* check the libburn message queue for errors */
  xfburn_log_drain_backend_messages ("burn_post", msg_text, sizeof (msg_text));

  xfburn_console_libburn_messages ();

  if (progress.buffer_capacity > 0) {
    percent = (gdouble) progress.buffer_min_fill / (gdouble) progress.buffer_capacity * 100.0;
  } else {
    percent = 0.0;
  }
  xfburn_progress_dialog_set_buffer_bar_min_fill (XFBURN_PROGRESS_DIALOG (dialog_progress), percent);

  wrote_well = burn_drive_wrote_well (drive);
  xfburn_log_read_device_io_counters (drive_addr, NULL, &io_counters);
  XFBURN_LOG_INFO ("BURN_END",
                   "addr=\"%s\" wrote_well=%d stopping=%d elapsed_s=%ld min_buf_pct=%d ioerr_cnt=%" G_GINT64_FORMAT " dev_state=\"%s\" last_err_msg=\"%s\"",
                   drive_addr, wrote_well, stopping,
                   (long) ((g_get_monotonic_time () - burn_start_us) / G_USEC_PER_SEC),
                   (int) percent, io_counters.ioerr_cnt, io_counters.state, msg_text);

  if (G_LIKELY (wrote_well)) {
    if (verify_data != NULL && (verify_data->sector_hashes->len > 0 || verify_data->partial_len > 0)) {
      gboolean verify_stopped = FALSE;
      gchar *verify_failed_msg = NULL;
      gchar *popup_msg = NULL;
      GtkMessageType popup_type = GTK_MESSAGE_INFO;

      if (xfburn_perform_burn_verify (dialog_progress, drive, factor, verify_data,
                                      &verify_stopped, &verify_failed_msg)) {
        final_message = g_strdup (_("Done"));
        final_status = XFBURN_PROGRESS_DIALOG_STATUS_COMPLETED;
        popup_msg = g_strdup (_("Disc successfully verified"));
        popup_type = GTK_MESSAGE_INFO;
      } else if (verify_stopped) {
        final_message = g_strdup (_("User Aborted"));
        final_status = XFBURN_PROGRESS_DIALOG_STATUS_CANCELLED;
      } else {
        if (verify_failed_msg != NULL) {
          final_message = verify_failed_msg;
        } else {
          final_message = g_strdup (_("Failure"));
        }
        verify_failed_msg = NULL;
        final_status = XFBURN_PROGRESS_DIALOG_STATUS_FAILED;
        popup_msg = g_strdup (final_message);
        popup_type = GTK_MESSAGE_ERROR;
      }
      g_free (verify_failed_msg);

      /* restore default signal handlers */
      burn_set_signal_handling (NULL, NULL, 1);

      xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress), final_status, final_message);
      g_free (final_message);

      if (popup_msg != NULL) {
        GtkWidget *popup;

        gdk_threads_enter ();
        popup = gtk_message_dialog_new (GTK_WINDOW (dialog_progress),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        popup_type,
                                        GTK_BUTTONS_OK,
                                        "%s", popup_msg);
        g_signal_connect (G_OBJECT (popup), "response", G_CALLBACK (gtk_widget_destroy), NULL);
        gtk_widget_show (popup);
        gdk_threads_leave ();
        g_free (popup_msg);
      }
      return;
    } else {
      final_message = g_strdup (_("Done"));
      final_status = XFBURN_PROGRESS_DIALOG_STATUS_COMPLETED;
    }
  } else {
    xfburn_log_dump_device_hardware (drive_addr, NULL);
    if (stopping) {
      final_status_text  = _("User Aborted");
      final_status = XFBURN_PROGRESS_DIALOG_STATUS_CANCELLED;
    } else {
      final_status_text  = _("Failure");
      final_status = XFBURN_PROGRESS_DIALOG_STATUS_FAILED;
    }
    if (msg_text[0] != '\0') {
      final_message = g_strdup_printf ("%s: %s", final_status_text, msg_text);
    } else {
      final_message = g_strdup (final_status_text);
    }
  }

  /* output it to console in case the program crashes */
  DBG ("Final burning status: %s", final_message);

  /* restore default signal handlers */
  burn_set_signal_handling (NULL, NULL, 1);

  xfburn_progress_dialog_set_status_with_text (XFBURN_PROGRESS_DIALOG (dialog_progress), final_status, final_message);
  g_free (final_message);
}
