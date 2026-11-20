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

#include <glib.h>
#include <gtk/gtk.h>

#include "xfburn-global.h"
#include "xfburn-device-list.h"
#include "xfburn-utils.h"
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

static void
test_profile_to_kb (void)
{
  /* CD profiles (including CD-RW 0x0a regression test) */
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_NONE), ==, CDR_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_CDROM), ==, CDR_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_CDR), ==, CDR_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_CDRW), ==, CDR_1X_SPEED);

  g_assert_cmpfloat_with_epsilon (xfburn_media_profile_to_kb_exact (XFBURN_PROFILE_CDR), CDR_1X_SPEED_KB, 0.01);
  g_assert_cmpfloat_with_epsilon (xfburn_media_profile_to_kb_exact (XFBURN_PROFILE_CDRW), CDR_1X_SPEED_KB, 0.01);

  /* DVD profiles */
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVDROM), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_MINUS_R), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVDRAM), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_MINUS_RW_OVERWRITE), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_MINUS_RW_SEQUENTIAL), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_MINUS_R_DL), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_MINUS_R_DL_JUMP), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_PLUS_RW), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_PLUS_R), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_PLUS_RW_DL), ==, DVD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_DVD_PLUS_R_DL), ==, DVD_1X_SPEED);

  g_assert_cmpfloat_with_epsilon (xfburn_media_profile_to_kb_exact (XFBURN_PROFILE_DVD_PLUS_RW), DVD_1X_SPEED_KB, 0.01);

  /* BD profiles (including BD-R RRM 0x42 for formatted BD-R / M-DISC) */
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_BDROM), ==, BD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_BD_R), ==, BD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_BD_R_RRM), ==, BD_1X_SPEED);
  g_assert_cmpint (xfburn_media_profile_to_kb (XFBURN_PROFILE_BD_RE), ==, BD_1X_SPEED);

  g_assert_cmpfloat_with_epsilon (xfburn_media_profile_to_kb_exact (XFBURN_PROFILE_BD_R), BD_1X_SPEED_KB, 0.01);
  g_assert_cmpfloat_with_epsilon (xfburn_media_profile_to_kb_exact (XFBURN_PROFILE_BD_R_RRM), BD_1X_SPEED_KB, 0.01);
}

static void
assert_speed_label (gint speed_kbps, gint profile_no, const gchar *expected)
{
  gchar *actual = xfburn_format_burn_speed (speed_kbps, profile_no);
  g_assert_cmpstr (actual, ==, expected);
  g_free (actual);
}

static void
test_format_labels (void)
{
  /* CD-RW & CD-R */
  assert_speed_label (706, XFBURN_PROFILE_CDRW, "4x (706 kB/s)");
  assert_speed_label (1764, XFBURN_PROFILE_CDRW, "10x (1764 kB/s)");
  assert_speed_label (4233, XFBURN_PROFILE_CDR, "24x (4233 kB/s)");
  assert_speed_label (8467, XFBURN_PROFILE_CDR, "48x (8467 kB/s)");

  /* DVD including fractional 2.4x DVD+RW */
  assert_speed_label (3324, XFBURN_PROFILE_DVD_PLUS_RW, "2.4x (3324 kB/s)");
  assert_speed_label (5540, XFBURN_PROFILE_DVD_MINUS_R, "4x (5540 kB/s)");
  assert_speed_label (11080, XFBURN_PROFILE_DVD_MINUS_R, "8x (11080 kB/s)");

  /* BD-R / M-DISC BD-R / BD-RE */
  assert_speed_label (8991, XFBURN_PROFILE_BD_R, "2x (8991 kB/s)");
  assert_speed_label (17982, XFBURN_PROFILE_BD_R_RRM, "4x (17982 kB/s)");
  assert_speed_label (26973, XFBURN_PROFILE_BD_RE, "6x (26973 kB/s)");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/xfburn/speed/profile-to-kb", test_profile_to_kb);
  g_test_add_func ("/xfburn/speed/format-labels", test_format_labels);

  return g_test_run ();
}
