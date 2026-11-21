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

#include <gtk/gtk.h>
#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>

#include "xfburn-global.h"
#include "xfburn-device.h"
#include "xfburn-device-list.h"
#include "xfburn-udev-manager.h"
#include "xfburn-log.h"
#include "xfburn-disc-info.h"
#include "xfburn-disc-info-dialog.h"

#define XFBURN_DISC_INFO_DIALOG_GET_PRIVATE(obj) \
  (xfburn_disc_info_dialog_get_instance_private (XFBURN_DISC_INFO_DIALOG (obj)))

enum {
  COMBO_COL_NAME,
  COMBO_COL_DEVICE,
  COMBO_N_COLS,
};

enum {
  RESPONSE_COPY = 1,
};

typedef struct _XfburnDiscInfoDialogPrivate
{
  XfburnDeviceList *devlist;
#ifdef HAVE_GUDEV
  XfburnUdevManager *udevman;
  gulong udev_handler_id;
  guint udev_timeout_id;
#endif

  GtkWidget *combo_device;
  GtkWidget *button_refresh;
  GtkWidget *spinner;
  GtkWidget *label_summary;
  GtkWidget *label_feedback;
  GtkWidget *text_view;
  GtkWidget *button_copy;

  guint64 generation;
  GCancellable *probe_cancellable;
  gchar *last_report_text;
  guint feedback_timeout_id;
} XfburnDiscInfoDialogPrivate;

struct _XfburnDiscInfoDialog
{
  XfceTitledDialog parent;
};

typedef struct {
  GWeakRef dialog_ref;
  guint64 generation;
  gchar *addr;
  gchar *name;
  gchar *revision;
  GCancellable *cancellable;
  XfburnDiscInfo info;
} DiscInfoTask;

G_DEFINE_TYPE_WITH_PRIVATE (XfburnDiscInfoDialog, xfburn_disc_info_dialog, XFCE_TYPE_TITLED_DIALOG)

static void xfburn_disc_info_dialog_finalize (GObject *object);
static void start_probe (XfburnDiscInfoDialog *dialog);
static void populate_device_combo (XfburnDiscInfoDialog *dialog, const gchar *prefer_addr);
static void cb_combo_device_changed (GtkComboBox *combo, XfburnDiscInfoDialog *dialog);

static void
disc_info_task_free (DiscInfoTask *task)
{
  if (task == NULL) {
    return;
  }

  g_weak_ref_clear (&task->dialog_ref);
  xfburn_disc_info_clear (&task->info);
  g_clear_object (&task->cancellable);
  g_free (task->addr);
  g_free (task->name);
  g_free (task->revision);
  g_free (task);
}

static XfburnDevice *
get_combo_selected_device (XfburnDiscInfoDialog *dialog)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);
  GtkTreeModel *model = gtk_combo_box_get_model (GTK_COMBO_BOX (priv->combo_device));
  GtkTreeIter iter;
  XfburnDevice *device = NULL;

  if (gtk_combo_box_get_active_iter (GTK_COMBO_BOX (priv->combo_device), &iter)) {
    gtk_tree_model_get (model, &iter, COMBO_COL_DEVICE, &device, -1);
  }

  return device;
}

static void
set_summary_markup (XfburnDiscInfoDialog *dialog, const gchar *text)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);
  gchar *escaped = g_markup_escape_text (text != NULL ? text : "", -1);
  gchar *markup = g_strdup_printf ("<b>%s</b>", escaped);

  gtk_label_set_markup (GTK_LABEL (priv->label_summary), markup);
  g_free (markup);
  g_free (escaped);
}

static void
set_report_text (XfburnDiscInfoDialog *dialog, const gchar *report)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (priv->text_view));
  const gchar *text = report != NULL ? report : "";
  const gchar *line_start;
  GtkTextIter iter;

  g_free (priv->last_report_text);
  priv->last_report_text = g_strdup (text);

  gtk_text_buffer_set_text (buffer, "", 0);
  gtk_text_buffer_get_start_iter (buffer, &iter);

  line_start = text;
  while (*line_start != '\0') {
    const gchar *newline = strchr (line_start, '\n');
    gsize line_len = newline != NULL ? (gsize) (newline - line_start + 1) : strlen (line_start);
    gboolean is_section_header = (line_start[0] != ' ' && line_start[0] != '\n');

    if (is_section_header) {
      gtk_text_buffer_insert_with_tags_by_name (buffer, &iter, line_start, (gint) line_len, "section", NULL);
    } else {
      gtk_text_buffer_insert (buffer, &iter, line_start, (gint) line_len);
    }

    if (newline == NULL) {
      break;
    }
    line_start = newline + 1;
  }

  gtk_widget_set_sensitive (priv->button_copy, priv->last_report_text != NULL && priv->last_report_text[0] != '\0');
}

static void
set_scanning_ui (XfburnDiscInfoDialog *dialog, gboolean scanning)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);

  if (scanning) {
    gtk_widget_show (priv->spinner);
    gtk_spinner_start (GTK_SPINNER (priv->spinner));
    gtk_widget_set_sensitive (priv->button_refresh, FALSE);
    gtk_widget_set_sensitive (priv->button_copy, FALSE);
    set_summary_markup (dialog, _("Reading disc information..."));
  } else {
    gtk_spinner_stop (GTK_SPINNER (priv->spinner));
    gtk_widget_hide (priv->spinner);
    gtk_widget_set_sensitive (priv->button_refresh, TRUE);
  }
}

static gboolean
cb_inspect_complete (gpointer user_data)
{
  DiscInfoTask *task = (DiscInfoTask *) user_data;
  XfburnDiscInfoDialog *dialog = g_weak_ref_get (&task->dialog_ref);
  XfburnDiscInfoDialogPrivate *priv;
  gchar *summary;
  gchar *report;

  if (dialog == NULL) {
    return G_SOURCE_REMOVE;
  }

  priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);

  if (task->generation != priv->generation || task->info.cancelled) {
    g_object_unref (dialog);
    return G_SOURCE_REMOVE;
  }

  g_clear_object (&priv->probe_cancellable);
  set_scanning_ui (dialog, FALSE);

  summary = xfburn_disc_info_format_summary (&task->info);
  report = xfburn_disc_info_format_report (&task->info);

  set_summary_markup (dialog, summary);
  set_report_text (dialog, report);

  g_free (summary);
  g_free (report);
  g_object_unref (dialog);
  return G_SOURCE_REMOVE;
}

static gpointer
thread_inspect_disc (gpointer user_data)
{
  DiscInfoTask *task = (DiscInfoTask *) user_data;

  xfburn_log_set_thread_name ("disc_info");

  xfburn_disc_info_probe_addr (task->addr,
                               task->name,
                               task->revision,
                               task->cancellable,
                               &task->info);

  gdk_threads_add_idle_full (G_PRIORITY_DEFAULT_IDLE,
                             cb_inspect_complete,
                             task,
                             (GDestroyNotify) disc_info_task_free);
  return NULL;
}

static void
start_probe (XfburnDiscInfoDialog *dialog)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);
  XfburnDevice *device;
  DiscInfoTask *task;

  if (priv->probe_cancellable != NULL) {
    g_cancellable_cancel (priv->probe_cancellable);
    g_clear_object (&priv->probe_cancellable);
  }

  priv->generation++;

  device = get_combo_selected_device (dialog);
  if (device == NULL) {
    set_scanning_ui (dialog, FALSE);
    set_summary_markup (dialog, _("No burner detected"));
    set_report_text (dialog, _("No optical burner detected.\n"));
    return;
  }

  if (xfburn_device_is_burning ()) {
    XfburnDiscInfo busy_info;
    gchar *summary;
    gchar *report;

    memset (&busy_info, 0, sizeof (busy_info));
    busy_info.disc_status = BURN_DISC_UNGRABBED;
    busy_info.next_track_damaged = -1;
    busy_info.drive_busy_burning = TRUE;
    g_object_get (G_OBJECT (device),
                  "address", &busy_info.addr,
                  "name", &busy_info.name,
                  "revision", &busy_info.revision,
                  NULL);

    set_scanning_ui (dialog, FALSE);
    summary = xfburn_disc_info_format_summary (&busy_info);
    report = xfburn_disc_info_format_report (&busy_info);
    set_summary_markup (dialog, summary);
    set_report_text (dialog, report);

    g_free (summary);
    g_free (report);
    xfburn_disc_info_clear (&busy_info);
    g_object_unref (device);
    return;
  }

  /* Cancel any background device-list probe so we can acquire the drive cleanly */
  xfburn_device_list_cancel_refresh (priv->devlist);

  priv->probe_cancellable = g_cancellable_new ();
  set_scanning_ui (dialog, TRUE);

  task = g_new0 (DiscInfoTask, 1);
  g_weak_ref_init (&task->dialog_ref, dialog);
  task->generation = priv->generation;
  g_object_get (G_OBJECT (device),
                "address", &task->addr,
                "name", &task->name,
                "revision", &task->revision,
                NULL);
  task->cancellable = g_object_ref (priv->probe_cancellable);

  g_object_unref (device);

  g_thread_unref (g_thread_new ("disc_info", thread_inspect_disc, task));
}

static void
populate_device_combo (XfburnDiscInfoDialog *dialog, const gchar *prefer_addr)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);
  GtkListStore *store = GTK_LIST_STORE (gtk_combo_box_get_model (GTK_COMBO_BOX (priv->combo_device)));
  GList *devices = NULL;
  GList *iter;
  XfburnDevice *curr_dev = xfburn_device_list_get_current_device (priv->devlist);
  gint idx = 0;
  gint selected = -1;

  g_signal_handlers_block_by_func (priv->combo_device, G_CALLBACK (cb_combo_device_changed), dialog);

  gtk_list_store_clear (store);
  g_object_get (G_OBJECT (priv->devlist), "devices", &devices, NULL);

  for (iter = devices; iter != NULL; iter = iter->next) {
    XfburnDevice *dev = XFBURN_DEVICE (iter->data);
    gchar *name = NULL;
    gchar *addr = NULL;
    gchar *label;
    GtkTreeIter tree_iter;

    g_object_get (G_OBJECT (dev), "name", &name, "address", &addr, NULL);
    if (name != NULL && addr != NULL) {
      label = g_strdup_printf ("%s (%s)", name, addr);
    } else if (name != NULL) {
      label = g_strdup (name);
    } else {
      label = g_strdup (addr != NULL ? addr : _("Unknown drive"));
    }

    gtk_list_store_append (store, &tree_iter);
    gtk_list_store_set (store, &tree_iter,
                        COMBO_COL_NAME, label,
                        COMBO_COL_DEVICE, dev,
                        -1);

    if (prefer_addr != NULL && addr != NULL && g_strcmp0 (addr, prefer_addr) == 0) {
      selected = idx;
    } else if (selected < 0 && dev == curr_dev) {
      selected = idx;
    }

    g_free (label);
    g_free (name);
    g_free (addr);
    idx++;
  }

  if (selected < 0 && idx > 0) {
    selected = 0;
  }

  gtk_widget_set_sensitive (priv->combo_device, idx > 0);
  if (selected >= 0) {
    gtk_combo_box_set_active (GTK_COMBO_BOX (priv->combo_device), selected);
  }

  g_signal_handlers_unblock_by_func (priv->combo_device, G_CALLBACK (cb_combo_device_changed), dialog);
}

static void
cb_combo_device_changed (GtkComboBox *combo, XfburnDiscInfoDialog *dialog)
{
  start_probe (dialog);
}

static void
cb_refresh_clicked (GtkButton *button, XfburnDiscInfoDialog *dialog)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);
  XfburnDevice *prev_dev;
  gchar *prev_addr = NULL;

  prev_dev = get_combo_selected_device (dialog);
  if (prev_dev != NULL) {
    g_object_get (G_OBJECT (prev_dev), "address", &prev_addr, NULL);
    g_object_unref (prev_dev);
  }

  xfburn_device_list_rescan_devices_only (priv->devlist);
  populate_device_combo (dialog, prev_addr);
  g_free (prev_addr);

  start_probe (dialog);
}

static gboolean
cb_clear_feedback_timeout (gpointer user_data)
{
  XfburnDiscInfoDialog *dialog = XFBURN_DISC_INFO_DIALOG (user_data);
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);

  priv->feedback_timeout_id = 0;
  gtk_label_set_text (GTK_LABEL (priv->label_feedback), "");
  return G_SOURCE_REMOVE;
}

static void
copy_report_to_clipboard (XfburnDiscInfoDialog *dialog)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);
  GtkClipboard *clipboard;

  if (priv->last_report_text == NULL || priv->last_report_text[0] == '\0') {
    return;
  }

  clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text (clipboard, priv->last_report_text, -1);

  gtk_label_set_text (GTK_LABEL (priv->label_feedback), _("Copied to clipboard"));
  g_clear_handle_id (&priv->feedback_timeout_id, g_source_remove);
  priv->feedback_timeout_id = gdk_threads_add_timeout_seconds (2, cb_clear_feedback_timeout, dialog);
}

static void
cb_dialog_response (GtkDialog *dialog, gint response_id, gpointer user_data)
{
  if (response_id == RESPONSE_COPY) {
    copy_report_to_clipboard (XFBURN_DISC_INFO_DIALOG (dialog));
    g_signal_stop_emission_by_name (dialog, "response");
  }
}

#ifdef HAVE_GUDEV
static gboolean
cb_udev_refresh_timeout (gpointer user_data)
{
  XfburnDiscInfoDialog *dialog = XFBURN_DISC_INFO_DIALOG (user_data);
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);
  XfburnDevice *prev_dev;
  gchar *prev_addr = NULL;

  priv->udev_timeout_id = 0;

  if (xfburn_device_is_burning ()) {
    return G_SOURCE_REMOVE;
  }

  prev_dev = get_combo_selected_device (dialog);
  if (prev_dev != NULL) {
    g_object_get (G_OBJECT (prev_dev), "address", &prev_addr, NULL);
    g_object_unref (prev_dev);
  }

  xfburn_device_list_rescan_devices_only (priv->devlist);
  populate_device_combo (dialog, prev_addr);
  g_free (prev_addr);

  start_probe (dialog);
  return G_SOURCE_REMOVE;
}

static void
cb_udev_volume_changed (XfburnUdevManager *udevman, XfburnDiscInfoDialog *dialog)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);

  if (xfburn_device_is_burning ()) {
    return;
  }

  g_clear_handle_id (&priv->udev_timeout_id, g_source_remove);
  priv->udev_timeout_id = gdk_threads_add_timeout (1000, cb_udev_refresh_timeout, dialog);
}
#endif

static void
xfburn_disc_info_dialog_class_init (XfburnDiscInfoDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xfburn_disc_info_dialog_finalize;
}

static void
xfburn_disc_info_dialog_init (XfburnDiscInfoDialog *obj)
{
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (obj);
  GtkBox *content_area = GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (obj)));
  GtkWidget *vbox;
  GtkWidget *hbox_drive;
  GtkWidget *hbox_status;
  GtkWidget *label_drive;
  GtkWidget *img_refresh;
  GtkWidget *scrolled;
  GtkWidget *button_close;
  GtkListStore *store;
  GtkCellRenderer *cell;
  GtkTextBuffer *buffer;

  gtk_window_set_title (GTK_WINDOW (obj), _("Disc Info"));
  xfce_titled_dialog_set_subtitle (XFCE_TITLED_DIALOG (obj),
                                   _("Inspect inserted optical media and drive capabilities"));
  gtk_window_set_icon_name (GTK_WINDOW (obj), "drive-optical");
  gtk_window_set_modal (GTK_WINDOW (obj), TRUE);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (obj), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (obj), 640, 520);

  priv->devlist = xfburn_device_list_new ();

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, BORDER * 2);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), BORDER * 2);
  gtk_box_pack_start (content_area, vbox, TRUE, TRUE, 0);
  gtk_widget_show (vbox);

  /* Drive selector + Refresh */
  hbox_drive = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BORDER);
  gtk_box_pack_start (GTK_BOX (vbox), hbox_drive, FALSE, FALSE, 0);
  gtk_widget_show (hbox_drive);

  label_drive = gtk_label_new_with_mnemonic (_("_Drive:"));
  gtk_box_pack_start (GTK_BOX (hbox_drive), label_drive, FALSE, FALSE, 0);
  gtk_widget_show (label_drive);

  store = gtk_list_store_new (COMBO_N_COLS, G_TYPE_STRING, XFBURN_TYPE_DEVICE);
  priv->combo_device = gtk_combo_box_new_with_model (GTK_TREE_MODEL (store));
  g_object_unref (store);

  cell = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (priv->combo_device), cell, TRUE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (priv->combo_device), cell, "text", COMBO_COL_NAME, NULL);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label_drive), priv->combo_device);
  gtk_box_pack_start (GTK_BOX (hbox_drive), priv->combo_device, TRUE, TRUE, 0);
  gtk_widget_show (priv->combo_device);

  img_refresh = gtk_image_new_from_icon_name ("view-refresh", GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_widget_show (img_refresh);
  priv->button_refresh = gtk_button_new ();
  gtk_container_add (GTK_CONTAINER (priv->button_refresh), img_refresh);
  gtk_widget_set_tooltip_text (priv->button_refresh, _("Refresh disc information"));
  gtk_box_pack_start (GTK_BOX (hbox_drive), priv->button_refresh, FALSE, FALSE, 0);
  gtk_widget_show (priv->button_refresh);

  /* Status summary row */
  hbox_status = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BORDER);
  gtk_box_pack_start (GTK_BOX (vbox), hbox_status, FALSE, FALSE, 0);
  gtk_widget_show (hbox_status);

  priv->spinner = gtk_spinner_new ();
  gtk_box_pack_start (GTK_BOX (hbox_status), priv->spinner, FALSE, FALSE, 0);

  priv->label_summary = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (priv->label_summary), 0.0f);
  gtk_label_set_selectable (GTK_LABEL (priv->label_summary), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (priv->label_summary), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (hbox_status), priv->label_summary, TRUE, TRUE, 0);
  gtk_widget_show (priv->label_summary);

  priv->label_feedback = gtk_label_new ("");
  gtk_box_pack_end (GTK_BOX (hbox_status), priv->label_feedback, FALSE, FALSE, 0);
  gtk_widget_show (priv->label_feedback);

  /* Scrollable monospace report */
  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
  gtk_box_pack_start (GTK_BOX (vbox), scrolled, TRUE, TRUE, 0);
  gtk_widget_show (scrolled);

  priv->text_view = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (priv->text_view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (priv->text_view), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (priv->text_view), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (priv->text_view), GTK_WRAP_NONE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (priv->text_view), 8);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (priv->text_view), 8);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (priv->text_view), 6);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (priv->text_view), 6);
  gtk_container_add (GTK_CONTAINER (scrolled), priv->text_view);
  gtk_widget_show (priv->text_view);

  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (priv->text_view));
  gtk_text_buffer_create_tag (buffer, "section", "weight", PANGO_WEIGHT_BOLD, NULL);

  /* Action buttons */
  priv->button_copy = xfce_gtk_button_new_mixed ("edit-copy", _("_Copy"));
  gtk_widget_set_tooltip_text (priv->button_copy, _("Copy full disc report to clipboard"));
  gtk_widget_set_sensitive (priv->button_copy, FALSE);
  gtk_widget_show (priv->button_copy);
  gtk_dialog_add_action_widget (GTK_DIALOG (obj), priv->button_copy, RESPONSE_COPY);

  button_close = xfce_gtk_button_new_mixed ("window-close", _("_Close"));
  gtk_widget_show (button_close);
  gtk_dialog_add_action_widget (GTK_DIALOG (obj), button_close, GTK_RESPONSE_CLOSE);
  gtk_widget_set_can_default (button_close, TRUE);
  gtk_widget_grab_default (button_close);

  g_signal_connect (priv->combo_device, "changed", G_CALLBACK (cb_combo_device_changed), obj);
  g_signal_connect (priv->button_refresh, "clicked", G_CALLBACK (cb_refresh_clicked), obj);
  g_signal_connect (obj, "response", G_CALLBACK (cb_dialog_response), NULL);

#ifdef HAVE_GUDEV
  priv->udevman = xfburn_udev_manager_ref_global ();
  if (priv->udevman != NULL) {
    priv->udev_handler_id = g_signal_connect (priv->udevman,
                                              "volume-changed",
                                              G_CALLBACK (cb_udev_volume_changed),
                                              obj);
  }
#endif

  populate_device_combo (obj, NULL);
  start_probe (obj);
}

static void
xfburn_disc_info_dialog_finalize (GObject *object)
{
  XfburnDiscInfoDialog *dialog = XFBURN_DISC_INFO_DIALOG (object);
  XfburnDiscInfoDialogPrivate *priv = XFBURN_DISC_INFO_DIALOG_GET_PRIVATE (dialog);

  if (priv->probe_cancellable != NULL) {
    g_cancellable_cancel (priv->probe_cancellable);
    g_clear_object (&priv->probe_cancellable);
  }

  g_clear_handle_id (&priv->feedback_timeout_id, g_source_remove);

#ifdef HAVE_GUDEV
  g_clear_handle_id (&priv->udev_timeout_id, g_source_remove);
  if (priv->udevman != NULL) {
    g_clear_signal_handler (&priv->udev_handler_id, priv->udevman);
    g_clear_object (&priv->udevman);
  }
#endif

  g_clear_pointer (&priv->last_report_text, g_free);
  g_clear_object (&priv->devlist);

  G_OBJECT_CLASS (xfburn_disc_info_dialog_parent_class)->finalize (object);
}

GtkWidget *
xfburn_disc_info_dialog_new (void)
{
  return g_object_new (XFBURN_TYPE_DISC_INFO_DIALOG, NULL);
}
