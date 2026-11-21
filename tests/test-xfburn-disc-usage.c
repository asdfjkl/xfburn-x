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

#include "xfburn-disc-usage.h"
#include "xfburn-data-disc-usage.h"
#include "xfburn-audio-disc-usage.h"
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

static gboolean have_gtk_display = FALSE;

static void
test_disc_usage_class_labels (void)
{
  XfburnDiscUsageClass *data_cls = g_type_class_ref (XFBURN_TYPE_DATA_DISC_USAGE);
  XfburnDiscUsageClass *audio_cls = g_type_class_ref (XFBURN_TYPE_AUDIO_DISC_USAGE);

  g_assert_nonnull (data_cls);
  g_assert_cmpint (data_cls->num_labels, ==, 11);
  g_assert_cmpstr (data_cls->labels[0].label, ==, "200MB CD");
  g_assert_cmpstr (data_cls->labels[2].label, ==, "700MB CD");
  g_assert_cmpstr (data_cls->labels[5].label, ==, "4.3GB DVD");
  g_assert_cmpstr (data_cls->labels[6].label, ==, "7.9GB DVD");
  g_assert_cmpstr (data_cls->labels[7].label, ==, "22.5GB BD");
  g_assert_cmpstr (data_cls->labels[8].label, ==, "46.5GB BD");
  g_assert_cmpstr (data_cls->labels[9].label, ==, "93GB BD");
  g_assert_cmpstr (data_cls->labels[10].label, ==, "2TB");

  g_assert_nonnull (audio_cls);
  g_assert_cmpint (audio_cls->num_labels, ==, 5);
  g_assert_cmpstr (audio_cls->labels[0].label, ==, "200MB / 21min CD");
  g_assert_cmpstr (audio_cls->labels[2].label, ==, "700MB / 80min CD");
  g_assert_cmpstr (audio_cls->labels[4].label, ==, "900MB / 99min CD");

  g_type_class_unref (audio_cls);
  g_type_class_unref (data_cls);
}

static void
test_data_disc_usage_widget_bd_regression (void)
{
  GtkWidget *widget;
  XfburnDiscUsage *du;
  GtkTreeModel *model;
  gchar *active_text;

  if (!have_gtk_display) {
    g_test_skip ("No GTK display available for widget test");
    return;
  }

  widget = xfburn_data_disc_usage_new ();
  g_object_ref_sink (widget);
  du = XFBURN_DISC_USAGE (widget);

  /* Regression check: combo must hold all 11 real data disc sizes, never the 7 dummy test sizes */
  model = gtk_combo_box_get_model (GTK_COMBO_BOX (du->combo));
  g_assert_cmpint (gtk_tree_model_iter_n_children (model, NULL), ==, 11);

  active_text = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (du->combo));
  g_assert_cmpstr (active_text, ==, "700MB CD");
  g_free (active_text);
  g_assert_false (gtk_widget_get_sensitive (du->button));

  /* 20 GB collection must auto-select 22.5GB BD (index 7) and enable "Proceed to Burn" */
  xfburn_disc_usage_set_size (du, 20.0 * 1000.0 * 1000.0 * 1000.0);
  g_assert_cmpint (gtk_combo_box_get_active (GTK_COMBO_BOX (du->combo)), ==, 7);
  active_text = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (du->combo));
  g_assert_cmpstr (active_text, ==, "22.5GB BD");
  g_free (active_text);
  g_assert_cmpint (xfburn_disc_usage_get_disc_type (du), ==, DVD_DISC);
  g_assert_true (gtk_widget_get_sensitive (du->button));

  /* Manually picking a smaller disc disables Proceed to Burn; switching back re-enables it */
  gtk_combo_box_set_active (GTK_COMBO_BOX (du->combo), 2);
  g_assert_false (gtk_widget_get_sensitive (du->button));
  gtk_combo_box_set_active (GTK_COMBO_BOX (du->combo), 7);
  g_assert_true (gtk_widget_get_sensitive (du->button));

  /* 40 GB collection must auto-select 46.5GB BD (index 8) and stay enabled */
  xfburn_disc_usage_set_size (du, 40.0 * 1000.0 * 1000.0 * 1000.0);
  g_assert_cmpint (gtk_combo_box_get_active (GTK_COMBO_BOX (du->combo)), ==, 8);
  active_text = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (du->combo));
  g_assert_cmpstr (active_text, ==, "46.5GB BD");
  g_free (active_text);
  g_assert_true (gtk_widget_get_sensitive (du->button));

  /* Clearing collection resets to default 700MB CD and disables Proceed to Burn */
  xfburn_disc_usage_set_size (du, 0);
  g_assert_cmpint (gtk_combo_box_get_active (GTK_COMBO_BOX (du->combo)), ==, 2);
  g_assert_false (gtk_widget_get_sensitive (du->button));

  gtk_widget_destroy (widget);
  g_object_unref (widget);
}

static void
test_audio_disc_usage_widget (void)
{
  GtkWidget *widget;
  XfburnDiscUsage *du;
  GtkTreeModel *model;
  gchar *active_text;

  if (!have_gtk_display) {
    g_test_skip ("No GTK display available for widget test");
    return;
  }

  widget = xfburn_audio_disc_usage_new ();
  g_object_ref_sink (widget);
  du = XFBURN_DISC_USAGE (widget);

  /* Regression check: combo must hold all 5 audio CD entries (not 7 dummy entries) */
  model = gtk_combo_box_get_model (GTK_COMBO_BOX (du->combo));
  g_assert_cmpint (gtk_tree_model_iter_n_children (model, NULL), ==, 5);

  active_text = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (du->combo));
  g_assert_cmpstr (active_text, ==, "700MB / 80min CD");
  g_free (active_text);
  g_assert_false (gtk_widget_get_sensitive (du->button));

  /* 45 minutes fits on 650MB / 74min CD */
  xfburn_disc_usage_set_size (du, 45.0 * 60.0);
  g_assert_cmpint (gtk_combo_box_get_active (GTK_COMBO_BOX (du->combo)), ==, 1);
  g_assert_true (gtk_widget_get_sensitive (du->button));

  /* 120 minutes exceeds max 99min audio CD */
  xfburn_disc_usage_set_size (du, 120.0 * 60.0);
  g_assert_cmpint (gtk_combo_box_get_active (GTK_COMBO_BOX (du->combo)), ==, 4);
  g_assert_false (gtk_widget_get_sensitive (du->button));

  gtk_widget_destroy (widget);
  g_object_unref (widget);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  have_gtk_display = gtk_init_check (&argc, &argv);

  g_test_add_func ("/xfburn/disc-usage/class-labels", test_disc_usage_class_labels);
  g_test_add_func ("/xfburn/disc-usage/data-bd-regression", test_data_disc_usage_widget_bd_regression);
  g_test_add_func ("/xfburn/disc-usage/audio-widget", test_audio_disc_usage_widget);

  return g_test_run ();
}
