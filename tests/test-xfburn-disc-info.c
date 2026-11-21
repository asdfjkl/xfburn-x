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

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <libburn.h>

#include "xfburn-global.h"
#include "xfburn-device.h"
#include "xfburn-device-list.h"
#include "xfburn-disc-info.h"
#include "xfburn-settings.h"
#include "xfburn-main.h"
#include "xfburn-main-window.h"

/* Minimal link stubs for unrelated UI helpers in xfburn-utils.c */
gboolean
xfburn_settings_get_boolean (const gchar *key, gboolean fallback)
{
  return fallback;
}

gboolean
xfburn_main_has_initial_dir (void)
{
  return FALSE;
}

const gchar *
xfburn_main_get_initial_dir (void)
{
  return NULL;
}

XfburnMainWindow *
xfburn_main_window_get_instance (void)
{
  return NULL;
}

static GMutex test_log_mutex;
static const gchar *expected_warning_substr = NULL;
static gint expected_warning_hits = 0;

static GLogWriterOutput
test_log_writer (GLogLevelFlags log_level,
                 const GLogField *fields,
                 gsize n_fields,
                 gpointer user_data)
{
  if ((log_level & G_LOG_LEVEL_WARNING) != 0) {
    const gchar *msg = NULL;
    gsize i;

    for (i = 0; i < n_fields; i++) {
      if (g_strcmp0 (fields[i].key, "MESSAGE") == 0 && fields[i].value != NULL) {
        msg = (const gchar *) fields[i].value;
        break;
      }
    }

    g_mutex_lock (&test_log_mutex);
    if (expected_warning_substr != NULL && msg != NULL &&
        strstr (msg, expected_warning_substr) != NULL) {
      expected_warning_hits++;
      g_mutex_unlock (&test_log_mutex);
      return G_LOG_WRITER_HANDLED;
    }
    g_mutex_unlock (&test_log_mutex);
  }

  return g_log_writer_default (log_level, fields, n_fields, user_data);
}

static void
expect_warning_begin (const gchar *substr)
{
  g_mutex_lock (&test_log_mutex);
  expected_warning_substr = substr;
  expected_warning_hits = 0;
  g_mutex_unlock (&test_log_mutex);
}

static void
expect_warning_end (void)
{
  gint hits;

  g_mutex_lock (&test_log_mutex);
  hits = expected_warning_hits;
  expected_warning_substr = NULL;
  expected_warning_hits = 0;
  g_mutex_unlock (&test_log_mutex);

  g_assert_cmpint (hits, >=, 1);
}

static void
test_blank_cdrw_report (void)
{
  XfburnDiscInfo info;
  XfburnDiscFormatEntry fmt;
  gchar *summary;
  gchar *report;

  memset (&info, 0, sizeof (info));
  info.grabbed = TRUE;
  info.next_track_damaged = -1;
  info.addr = g_strdup ("/dev/sr0");
  info.name = g_strdup ("ASUS BW-16D1HT");
  info.revision = g_strdup ("3.10");
  info.serial_no = g_strdup ("K9IL68H2314");
  info.buffer_size = 4096;
  info.num_supported_profiles = 3;
  info.supported_profiles[0] = XFBURN_PROFILE_CDR;
  info.supported_profiles[1] = XFBURN_PROFILE_CDRW;
  info.supported_profiles[2] = XFBURN_PROFILE_DVD_MINUS_R;

  info.disc_status = BURN_DISC_BLANK;
  info.profile_no = XFBURN_PROFILE_CDRW;
  g_strlcpy (info.profile_name, "CD-RW", sizeof (info.profile_name));
  info.is_erasable = TRUE;
  info.guessed_manufacturer = g_strdup ("Mitsubishi Chemical Corporation");
  info.product_id = g_strdup ("97m34s23f/79m59s74f");
  info.raw_product_id = g_strdup ("97m34s23f/79m59s74f");
  info.has_atip_lba = TRUE;
  info.start_lba = -11077;
  info.end_lba = 359849;
  info.has_cd_info = TRUE;
  g_strlcpy (info.cd_disc_type, "CD-DA or CD-ROM", sizeof (info.cd_disc_type));
  info.cd_valid_bits = (1 << 0);

  info.available_space = (gint64) 359849 * DATA_BYTES_PER_SECTOR;
  info.has_format_info = TRUE;
  info.format_status = BURN_FORMAT_IS_UNFORMATTED;
  info.format_size = info.available_space;
  info.formats = g_array_new (FALSE, TRUE, sizeof (XfburnDiscFormatEntry));
  fmt.type = 0x10;
  fmt.size = info.available_space;
  fmt.tdp = 0;
  g_array_append_val (info.formats, fmt);

  info.write_speeds = g_slist_append (NULL, GINT_TO_POINTER (706));
  info.write_speeds = g_slist_append (info.write_speeds, GINT_TO_POINTER (1764));
  info.max_read_speed = 7056;
  info.has_multi_caps = TRUE;
  info.might_do_tao = 2;
  info.might_do_sao = 1;
  info.advised_write_mode = BURN_WRITE_TAO;
  info.multi_session = TRUE;
  info.multi_track = TRUE;
  info.might_simulate = TRUE;

  summary = xfburn_disc_info_format_summary (&info);
  report = xfburn_disc_info_format_report (&info);

  g_assert_cmpstr (summary, ==, "Blank — CD-RW (0x000a)");
  g_assert_true (g_utf8_validate (report, -1, NULL));
  g_assert_nonnull (strstr (report, "ASUS BW-16D1HT (rev. 3.10)"));
  g_assert_nonnull (strstr (report, "K9IL68H2314"));
  g_assert_nonnull (strstr (report, "CD-RW (0x000a)"));
  g_assert_nonnull (strstr (report, "Yes (erasable)"));
  g_assert_nonnull (strstr (report, "Mitsubishi Chemical Corporation"));
  g_assert_nonnull (strstr (report, "97m34s23f/79m59s74f"));
  g_assert_null (strstr (report, "Media ID (ASCII)"));
  g_assert_null (strstr (report, "Media ID (Hex)"));
  g_assert_nonnull (strstr (report, "CD-DA or CD-ROM"));
  g_assert_nonnull (strstr (report, "4x (706 kB/s), 10x (1764 kB/s)"));
  g_assert_nonnull (strstr (report, "TAO, SAO/DAO (advised: TAO)"));
  g_assert_nonnull (strstr (report, "0x10 CD-RW/DVD-RW full"));

  g_free (summary);
  g_free (report);
  xfburn_disc_info_clear (&info);
  /* Clear must be idempotent */
  xfburn_disc_info_clear (&info);
}

static void
test_dvd_media_id_ascii_and_hex (void)
{
  XfburnDiscInfo info;
  gchar *report;

  memset (&info, 0, sizeof (info));
  info.grabbed = TRUE;
  info.next_track_damaged = -1;
  info.addr = g_strdup ("/dev/sr0");
  info.name = g_strdup ("HL-DT-ST DVDRAM");
  info.disc_status = BURN_DISC_BLANK;
  info.profile_no = XFBURN_PROFILE_DVD_PLUS_R_DL;
  g_strlcpy (info.profile_name, "DVD+R/DL", sizeof (info.profile_name));

  /* Replicates the classic "CMC MAG M01 000" Media ID from DVD+R / DVD-R */
  info.product_id = g_strdup ("CMC_MAG/M01/000");
  info.media_code1 = g_strdup ("CMC_MAG");
  info.media_code2 = g_strdup ("M01");
  info.raw_product_id = g_strdup ("CMC MAG/M01/000");
  info.raw_media_code1 = g_strdup ("CMC MAG");
  info.raw_media_code2 = g_strdup ("M01");
  info.book_type = g_strdup ("DVD+R DL");
  info.guessed_manufacturer = g_strdup ("CMC Magnetics Corporation");

  info.has_phys_format = TRUE;
  info.phys_book_name = g_strdup ("DVD+R DL");
  info.phys_part_version = 1;
  info.phys_num_layers = 2;
  info.phys_num_blocks = 4173824;

  report = xfburn_disc_info_format_report (&info);

  g_assert_true (g_utf8_validate (report, -1, NULL));
  g_assert_nonnull (strstr (report, "CMC MAG M01 000"));
  g_assert_nonnull (strstr (report, "43 4D 43 20 4D 41 47 / 4D 30 31 / 30 30 30"));
  g_assert_nonnull (strstr (report, "CMC_MAG/M01/000"));
  g_assert_nonnull (strstr (report, "2 (multi-layer), part version 1"));

  g_free (report);
  xfburn_disc_info_clear (&info);
}

static void
test_written_toc_table (void)
{
  XfburnDiscInfo info;
  XfburnDiscTrackEntry t1;
  XfburnDiscTrackEntry t2;
  gchar *summary;
  gchar *report;

  memset (&info, 0, sizeof (info));
  info.grabbed = TRUE;
  info.next_track_damaged = -1;
  info.addr = g_strdup ("/dev/sr0");
  info.disc_status = BURN_DISC_FULL;
  info.profile_no = XFBURN_PROFILE_CDR;
  g_strlcpy (info.profile_name, "CD-R", sizeof (info.profile_name));
  info.complete_sessions = 2;
  info.incomplete_sessions = 0;
  info.recorded_sectors = 18000 + 25000;

  info.tracks = g_array_new (FALSE, TRUE, sizeof (XfburnDiscTrackEntry));

  memset (&t1, 0, sizeof (t1));
  t1.session = 1;
  t1.track_no = 1;
  t1.is_data = FALSE;
  t1.start_lba = 0;
  t1.blocks = 18000; /* 04:00 */
  g_array_append_val (info.tracks, t1);

  memset (&t2, 0, sizeof (t2));
  t2.session = 2;
  t2.track_no = 2;
  t2.is_data = TRUE;
  t2.start_lba = 29400;
  t2.blocks = 25000;
  t2.extensions_valid = 4;
  t2.track_status_bits = (1 << 5) | (1 << 13); /* Damaged + Packet */
  g_array_append_val (info.tracks, t2);

  summary = xfburn_disc_info_format_summary (&info);
  report = xfburn_disc_info_format_report (&info);

  g_assert_cmpstr (summary, ==, "Full / Closed — CD-R (0x0009)");
  g_assert_nonnull (strstr (report, "Sessions & Tracks"));
  g_assert_nonnull (strstr (report, "2 complete"));
  g_assert_nonnull (strstr (report, "Audio"));
  g_assert_nonnull (strstr (report, "04:00"));
  g_assert_nonnull (strstr (report, "Data"));
  g_assert_nonnull (strstr (report, "Damaged,Packet"));

  g_free (summary);
  g_free (report);
  xfburn_disc_info_clear (&info);
}

static void
test_bd_spare_and_pow (void)
{
  XfburnDiscInfo info;
  gchar *report;

  memset (&info, 0, sizeof (info));
  info.grabbed = TRUE;
  info.next_track_damaged = 1;
  info.addr = g_strdup ("/dev/sr0");
  info.disc_status = BURN_DISC_APPENDABLE;
  info.profile_no = XFBURN_PROFILE_BD_R;
  g_strlcpy (info.profile_name, "BD-R sequential recording", sizeof (info.profile_name));
  info.has_bd_spare = TRUE;
  info.bd_spare_alloc_blocks = 12288;
  info.bd_spare_free_blocks = 6144;
  info.bd_r_pow = TRUE;
  info.has_nwa = TRUE;
  info.next_track_lba = 1024;
  info.next_track_nwa = 2048;

  report = xfburn_disc_info_format_report (&info);

  g_assert_true (g_utf8_validate (report, -1, NULL));
  g_assert_nonnull (strstr (report, "50.0%"));
  g_assert_nonnull (strstr (report, "Pseudo Overwrite"));
  g_assert_nonnull (strstr (report, "LBA 2048"));
  g_assert_nonnull (strstr (report, "Warning: last track marked damaged"));

  g_free (report);
  xfburn_disc_info_clear (&info);
}

static void
test_empty_busy_and_error_states (void)
{
  XfburnDiscInfo info;
  gchar *summary;
  gchar *report;

  /* 1. Empty drive */
  memset (&info, 0, sizeof (info));
  info.grabbed = TRUE;
  info.next_track_damaged = -1;
  info.addr = g_strdup ("/dev/sr0");
  info.disc_status = BURN_DISC_EMPTY;
  summary = xfburn_disc_info_format_summary (&info);
  report = xfburn_disc_info_format_report (&info);
  g_assert_cmpstr (summary, ==, "Empty — No disc in drive");
  g_assert_nonnull (strstr (report, "Empty (no disc in drive)"));
  g_free (summary);
  g_free (report);
  xfburn_disc_info_clear (&info);

  /* 2. Mounted and busy (unmount failed) */
  memset (&info, 0, sizeof (info));
  info.grabbed = FALSE;
  info.umount_failed = TRUE;
  info.next_track_damaged = -1;
  info.addr = g_strdup ("/dev/sr0");
  info.error_msg = g_strdup ("Target is busy");
  summary = xfburn_disc_info_format_summary (&info);
  report = xfburn_disc_info_format_report (&info);
  g_assert_nonnull (strstr (summary, "mounted and in use"));
  g_assert_nonnull (strstr (report, "Target is busy"));
  g_assert_nonnull (strstr (report, "Refresh"));
  g_free (summary);
  g_free (report);
  xfburn_disc_info_clear (&info);

  /* 3. Drive busy burning */
  memset (&info, 0, sizeof (info));
  info.grabbed = FALSE;
  info.drive_busy_burning = TRUE;
  info.next_track_damaged = -1;
  info.addr = g_strdup ("/dev/sr0");
  summary = xfburn_disc_info_format_summary (&info);
  report = xfburn_disc_info_format_report (&info);
  g_assert_nonnull (strstr (summary, "burn or blank operation in progress"));
  g_assert_nonnull (strstr (report, "burn or blank operation in progress"));
  g_free (summary);
  g_free (report);
  xfburn_disc_info_clear (&info);
}

typedef struct {
  const gchar *addr;
  GCancellable *cancellable;
  gboolean ret;
  XfburnDiscInfo info;
} ProbeCtx;

static gpointer
worker_disc_info_probe (gpointer user_data)
{
  ProbeCtx *ctx = (ProbeCtx *) user_data;

  ctx->ret = xfburn_disc_info_probe_addr (ctx->addr,
                                          "Test Drive",
                                          "1.00",
                                          ctx->cancellable,
                                          &ctx->info);
  return NULL;
}

static void
test_probe_cancelled_and_invalid_fast (void)
{
  ProbeCtx ctx;
  GThread *worker;
  gint64 t_start;

  g_assert_true (burn_initialize ());

  /* Pre-cancelled probe returns immediately without grabbing */
  memset (&ctx, 0, sizeof (ctx));
  ctx.addr = "/dev/nonexistent-xfburn-test-sr99";
  ctx.cancellable = g_cancellable_new ();
  g_cancellable_cancel (ctx.cancellable);

  t_start = g_get_monotonic_time ();
  worker = g_thread_new ("test_disc_info_cancel", worker_disc_info_probe, &ctx);
  g_thread_join (worker);

  g_assert_false (ctx.ret);
  g_assert_true (ctx.info.cancelled);
  g_assert_false (ctx.info.grabbed);
  g_assert_cmpint (g_get_monotonic_time () - t_start, <, 500 * 1000);
  g_assert_false (xfburn_device_is_busy ());
  xfburn_disc_info_clear (&ctx.info);
  g_object_unref (ctx.cancellable);

  /* Invalid device path fails cleanly and releases grab slot */
  memset (&ctx, 0, sizeof (ctx));
  ctx.addr = "/dev/nonexistent-xfburn-test-sr99";
  ctx.cancellable = g_cancellable_new ();

  expect_warning_begin ("Device address does not lead to a burner");
  worker = g_thread_new ("test_disc_info_invalid", worker_disc_info_probe, &ctx);
  g_thread_join (worker);
  expect_warning_end ();

  g_assert_false (ctx.ret);
  g_assert_false (ctx.info.cancelled);
  g_assert_false (ctx.info.grabbed);
  g_assert_cmpint (ctx.info.disc_status, ==, BURN_DISC_UNGRABBED);
  g_assert_false (xfburn_device_is_busy ());
  xfburn_disc_info_clear (&ctx.info);
  g_object_unref (ctx.cancellable);

  xfburn_device_wait_for_probes ();
  burn_finish ();
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);
  g_log_set_writer_func (test_log_writer, NULL, NULL);

  g_test_add_func ("/xfburn/disc-info/blank-cdrw-report", test_blank_cdrw_report);
  g_test_add_func ("/xfburn/disc-info/dvd-media-id-ascii-hex", test_dvd_media_id_ascii_and_hex);
  g_test_add_func ("/xfburn/disc-info/written-toc-table", test_written_toc_table);
  g_test_add_func ("/xfburn/disc-info/bd-spare-and-pow", test_bd_spare_and_pow);
  g_test_add_func ("/xfburn/disc-info/empty-busy-and-error-states", test_empty_busy_and_error_states);
  g_test_add_func ("/xfburn/disc-info/probe-cancelled-and-invalid-fast", test_probe_cancelled_and_invalid_fast);

  return g_test_run ();
}
