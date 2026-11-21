/*
 * Copyright (c) 2005-2006 Jean-François Wauthy (pollux@xfce.org)
 * Copyright (c) 2008-2009 David Mohr <david@mcbf.net>
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

#include <glib.h>
#include <libxfce4util/libxfce4util.h>

#include <stdlib.h>
#include <unistd.h>

#include <libburn.h>

#include "xfburn-global.h"
#include "xfburn-udev-manager.h"
#include "xfburn-utils.h"
#include "xfburn-log.h"

#include "xfburn-device-list.h"
#include "xfburn-cclosure-marshal.h"

/*- private prototypes -*/

typedef struct _XfburnDeviceListPrivate XfburnDeviceListPrivate;

struct _XfburnDeviceListPrivate {
  GList *devices;
  gint num_drives;
  gint num_burners;
  XfburnDevice *curr_device;
  gboolean needs_refresh;
  gboolean shutting_down;

  GMutex probe_state_mutex;
  gboolean refresh_in_progress;
  gboolean refresh_queued;
  gboolean refresh_queued_device_only;
  guint64 refresh_generation;
  GCancellable *refresh_cancellable;
  GThread *active_probe_thread;
  guint probe_complete_idle_id;

#ifdef HAVE_GUDEV
  gulong volume_changed_handlerid;
  guint udev_refresh_timeout;
#endif
};

typedef struct {
  XfburnDeviceList *devlist;
  XfburnDevice *device;
  gchar *addr;
  gchar *name;
  gboolean details_known;
  gboolean device_changed;
  guint64 generation;
  GCancellable *cancellable;
  XfburnDeviceProbeResult result;
} DeviceProbeTask;

static XfburnDeviceList *global_device_list = NULL;

void get_libburn_device_list (XfburnDeviceList *devlist);
static void cb_combo_device_changed (GtkComboBox *combo, XfburnDeviceList *devlist);
static void cb_refresh_clicked (GtkButton *button, XfburnDeviceList *devlist);
#ifdef HAVE_GUDEV
static void cb_volumes_changed (XfburnUdevManager *udevman, XfburnDeviceList *devlist);
#endif
static XfburnDevice * get_selected_device (GtkComboBox *combo_device);
static void start_refresh_async (XfburnDeviceList *devlist, gboolean device_changed_only);
static void rescan_udev_devices (XfburnDeviceList *devlist);

enum {
  PROP_0,
  PROP_NUM_DRIVES,
  PROP_NUM_BURNERS,
  PROP_DEVICES,
  PROP_CURRENT_DEVICE,
};

enum {
  VOLUME_CHANGE_START,
  VOLUME_CHANGE_END,
  LAST_SIGNAL,
};

enum {
  DEVICE_NAME_COLUMN,
  DEVICE_POINTER_COLUMN,
  DEVICE_N_COLUMNS,
};

static guint signals[LAST_SIGNAL];


/*****************/
/*- class setup -*/
/*****************/

G_DEFINE_TYPE_WITH_PRIVATE (XfburnDeviceList, xfburn_device_list, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (xfburn_device_list_get_instance_private (XFBURN_DEVICE_LIST (o)))

static void
xfburn_device_list_get_property (GObject *object, guint property_id,
                              GValue *value, GParamSpec *pspec)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (XFBURN_DEVICE_LIST (object));

  switch (property_id) {
    case PROP_NUM_DRIVES:
      g_value_set_int (value, priv->num_drives);
      break;
    case PROP_NUM_BURNERS:
      g_value_set_int (value, priv->num_burners);
      break;
    case PROP_DEVICES:
      g_value_set_pointer (value, priv->devices);
      break;
    case PROP_CURRENT_DEVICE:
      g_value_set_object (value, priv->curr_device);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
xfburn_device_list_set_property (GObject *object, guint property_id,
                              const GValue *value, GParamSpec *pspec)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (XFBURN_DEVICE_LIST (object));

  switch (property_id) {
    case PROP_NUM_DRIVES:
      /* FIXME: should this be there? */
      priv->num_drives = g_value_get_int (value);
      break;
    case PROP_NUM_BURNERS:
      /* FIXME: should this be there? */
      priv->num_burners = g_value_get_int (value);
      break;
    case PROP_DEVICES:
      /* FIXME: should this be there? */
      priv->devices = g_value_get_pointer (value);
      break;
    case PROP_CURRENT_DEVICE:
      priv->curr_device = g_value_get_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

/* implement a singleton pattern */
static GObject*
xfburn_device_list_constructor (GType type, guint n_construct_params, GObjectConstructParam *construct_params)
{
  GObject *object;

  if (!global_device_list) {
    object = G_OBJECT_CLASS (xfburn_device_list_parent_class)->constructor (type, n_construct_params, construct_params);

    global_device_list = XFBURN_DEVICE_LIST (object);
  } else {
    object = g_object_ref (G_OBJECT (global_device_list));
  }

  return object;
}

static void
xfburn_device_list_finalize (GObject *object)
{
  XfburnDeviceList *devlist = XFBURN_DEVICE_LIST (object);
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);

  xfburn_device_list_shutdown (devlist);

#ifdef HAVE_GUDEV
  if (priv->volume_changed_handlerid > 0) {
    XfburnUdevManager *udevman = xfburn_udev_manager_ref_global ();
    if (udevman != NULL) {
      g_signal_handler_disconnect (udevman, priv->volume_changed_handlerid);
      g_object_unref (udevman);
    }
    priv->volume_changed_handlerid = 0;
  }
#endif

  g_list_free_full (priv->devices, (GDestroyNotify) g_object_unref);
  priv->devices = NULL;
  priv->curr_device = NULL;

  g_mutex_clear (&priv->probe_state_mutex);

  if (global_device_list == devlist) {
    global_device_list = NULL;
  }

  G_OBJECT_CLASS (xfburn_device_list_parent_class)->finalize (object);
}

static void
xfburn_device_list_class_init (XfburnDeviceListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xfburn_device_list_get_property;
  object_class->set_property = xfburn_device_list_set_property;
  object_class->constructor  = xfburn_device_list_constructor;
  object_class->finalize     = xfburn_device_list_finalize;

  signals[VOLUME_CHANGE_START] = g_signal_new ("volume-change-start", XFBURN_TYPE_DEVICE_LIST, G_SIGNAL_ACTION,
                                          0,
                                          NULL, NULL, g_cclosure_marshal_VOID__BOOLEAN,
                                          G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[VOLUME_CHANGE_END] = g_signal_new ("volume-change-end", XFBURN_TYPE_DEVICE_LIST, G_SIGNAL_ACTION,
                                          G_STRUCT_OFFSET (XfburnDeviceListClass, volume_changed),
                                          NULL, NULL, xfburn_cclosure_marshal_VOID__BOOLEAN_OBJECT,
                                          G_TYPE_NONE, 2, G_TYPE_BOOLEAN, XFBURN_TYPE_DEVICE);

  g_object_class_install_property (object_class, PROP_NUM_BURNERS,
                                   g_param_spec_int ("num-burners", _("Number of burners in the system"),
                                                     _("Number of burners in the system"), 0, G_MAXINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_NUM_DRIVES,
                                   g_param_spec_int ("num-drives", _("Number of drives in the system"),
                                                     _("Number of drives in the system (readers and writers)"), 0, G_MAXINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_DEVICES,
                                   g_param_spec_pointer ("devices", _("List of devices"),
                                                         _("List of devices"), G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_CURRENT_DEVICE,
                                   g_param_spec_object ("current-device", _("Currently selected device"),
                                                        _("Currently selected device"), XFBURN_TYPE_DEVICE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
xfburn_device_list_init (XfburnDeviceList *self)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (self);

#ifdef HAVE_GUDEV
  XfburnUdevManager *udevman = xfburn_udev_manager_get_global ();
#endif

  DBG ("Constructing device list");
  g_mutex_init (&priv->probe_state_mutex);
  xfburn_console_libburn_messages ();

#ifdef HAVE_GUDEV
  priv->devices = xfburn_udev_manager_get_devices (udevman, &priv->num_drives, &priv->num_burners);
  if (priv->num_drives > 0 && priv->num_burners < 1)
    g_warning ("There are %d drives in your system, but none are capable of burning", priv->num_drives);
  priv->volume_changed_handlerid = g_signal_connect (G_OBJECT (udevman), "volume-changed", G_CALLBACK (cb_volumes_changed), self);

  if (priv->num_burners > 0 && priv->devices != NULL) {
    priv->curr_device = XFBURN_DEVICE (priv->devices->data);
    /* Defer libburn disc/speed probing until a burn/blank dialog opens so app startup
     * is instantaneous and never auto-unmounts or blocks on a mounted disc. */
    priv->needs_refresh = TRUE;
  }
#else
  get_libburn_device_list (self);
  if (priv->num_burners > 0 && priv->devices != NULL) {
    priv->curr_device = XFBURN_DEVICE (priv->devices->data);
    priv->needs_refresh = TRUE;
  }
#endif
}


/***************/
/*- internals -*/
/***************/

void
get_libburn_device_list (XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);

  struct burn_drive_info *drives;
  gint i, ret;
  guint num_drives = 0;
  gint64 t_call;

  DBG ("Before scanning for drives");
  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_drive_scan", "scanning");
  while ((ret = burn_drive_scan (&drives, &(num_drives))) == 0) {
    usleep (1002);
  }
  XFBURN_LOG_CALL_EXIT ("burn_drive_scan", t_call, "ret=%d num_drives=%u", ret, num_drives);
  xfburn_log_drain_backend_messages ("burn_drive_scan", NULL, 0);
  DBG ("After scanning for drives");

  priv->num_drives = (gint) num_drives;

  if (ret < 0) {
    XFBURN_LOG_ERROR ("DEV", "An error occurred while scanning for available drives (ret=%d)", ret);
    g_warning ("An error occurred while scanning for available drives");
  }

  if (priv->num_drives < 1) {
    XFBURN_LOG_WARN ("DEV", "No drives were found (ret=%d)", ret);
    g_warning ("No drives were found! If this is in error, check the permissions");
  }

  for (i = 0; i < priv->num_drives; i++) {
    XfburnDevice *device = xfburn_device_new ();
    const gchar *name;
    char addr[BURN_DRIVE_ADR_LEN];
    char rev[5];

    name = xfburn_device_set_name (device, drives[i].vendor, drives[i].product);
    g_strlcpy (rev, drives[i].revision, 5);

    g_object_set (device, "revision", rev, NULL);

    xfburn_device_fillin_libburn_info (device, &drives[i]);

    ret = burn_drive_d_get_adr (drives[i].drive, addr);
    if (ret <= 0) {
      XFBURN_LOG_ERROR ("DEV", "Unable to get drive \"%s\" address (ret=%d)", name, ret);
      xfburn_log_drain_backend_messages ("get_adr", NULL, 0);
      g_warning ("Unable to get drive %s address (ret=%d). Please report this problem to libburn-hackers@pykix.org", name, ret);
      g_object_unref (device);
      burn_drive_snooze (drives[i].drive, 0);
      continue;
    }

    g_object_set (device, "address", addr, NULL);

    if (xfburn_device_can_burn (device)) {
      priv->devices = g_list_append (priv->devices, device);
      priv->num_burners++;
      xfburn_log_dump_device_hardware (addr, NULL);
    } else {
      g_message ("Ignoring reader '%s' at '%s'", name, addr);
      g_object_unref (device);
    }
    burn_drive_snooze(drives[i].drive, 0);
  }

  burn_drive_info_free (drives);

  if (priv->num_drives > 0 && priv->num_burners < 1)
    g_warning ("There are %d drives in your system, but none are capable of burning", priv->num_drives);

  DBG ("Done");
}

static XfburnDevice *
get_selected_device (GtkComboBox *combo_device)
{
  GtkTreeModel *model;
  GtkTreeIter iter;
  XfburnDevice *device = NULL;
  gboolean ret;

  model = gtk_combo_box_get_model (GTK_COMBO_BOX (combo_device));
  ret = gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo_device), &iter);
  if (ret)
    gtk_tree_model_get (model, &iter, DEVICE_POINTER_COLUMN, &device, -1);

  return device;
}

static void
device_probe_task_free (DeviceProbeTask *task)
{
  if (task == NULL) {
    return;
  }

  xfburn_device_probe_result_clear (&task->result);
  g_clear_object (&task->cancellable);
  g_clear_object (&task->device);
  g_clear_object (&task->devlist);
  g_free (task->addr);
  g_free (task->name);
  g_free (task);
}

static gboolean
cb_probe_complete (gpointer user_data)
{
  DeviceProbeTask *task = (DeviceProbeTask *) user_data;
  XfburnDeviceList *devlist = task->devlist;
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);
  GThread *finished_thread = NULL;

  g_mutex_lock (&priv->probe_state_mutex);
  priv->probe_complete_idle_id = 0;
  g_mutex_unlock (&priv->probe_state_mutex);

  if (priv->active_probe_thread != NULL) {
    finished_thread = priv->active_probe_thread;
    priv->active_probe_thread = NULL;
    g_thread_join (finished_thread);
  }

  if (priv->shutting_down) {
    return G_SOURCE_REMOVE;
  }

  priv->refresh_in_progress = FALSE;
  g_clear_object (&priv->refresh_cancellable);

  if (task->generation != priv->refresh_generation || task->result.cancelled) {
    if (priv->refresh_queued) {
      gboolean dev_only = priv->refresh_queued_device_only;
      priv->refresh_queued = FALSE;
      priv->refresh_queued_device_only = FALSE;

      if (!xfburn_device_is_burning () &&
          (dev_only || g_signal_has_handler_pending (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, FALSE))) {
        start_refresh_async (devlist, dev_only);
        return G_SOURCE_REMOVE;
      }
      priv->needs_refresh = TRUE;
    } else {
      priv->needs_refresh = TRUE;
    }

    g_signal_emit (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, task->device_changed, priv->curr_device);
    return G_SOURCE_REMOVE;
  }

  if (task->device != NULL && g_list_find (priv->devices, task->device) != NULL) {
    xfburn_device_apply_probe_result (task->device, &task->result);
  }

  /* If probing failed (e.g. drive was temporarily busy), mark stale so the next
   * dialog open automatically retries instead of showing a stale error. */
  priv->needs_refresh = !task->result.grabbed;

  if (priv->refresh_queued) {
    gboolean dev_only = priv->refresh_queued_device_only;
    priv->refresh_queued = FALSE;
    priv->refresh_queued_device_only = FALSE;

    if (!xfburn_device_is_burning () &&
        (dev_only || g_signal_has_handler_pending (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, FALSE))) {
      start_refresh_async (devlist, dev_only);
      return G_SOURCE_REMOVE;
    }
    priv->needs_refresh = TRUE;
  }

  g_signal_emit (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, task->device_changed, priv->curr_device);
  return G_SOURCE_REMOVE;
}

static gpointer
thread_probe_device (gpointer user_data)
{
  DeviceProbeTask *task = (DeviceProbeTask *) user_data;
  XfburnDeviceListPrivate *priv = GET_PRIVATE (task->devlist);

  xfburn_log_set_thread_name ("dev_probe");

  xfburn_device_probe_addr (task->addr,
                            task->name,
                            TRUE,
                            task->details_known,
                            task->cancellable,
                            &task->result);

  g_mutex_lock (&priv->probe_state_mutex);
  if (!priv->shutting_down) {
    priv->probe_complete_idle_id = gdk_threads_add_idle_full (G_PRIORITY_DEFAULT_IDLE,
                                                              cb_probe_complete,
                                                              task,
                                                              (GDestroyNotify) device_probe_task_free);
    g_mutex_unlock (&priv->probe_state_mutex);
  } else {
    g_mutex_unlock (&priv->probe_state_mutex);
    device_probe_task_free (task);
  }

  return NULL;
}

static void
rescan_udev_devices (XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);
#ifdef HAVE_GUDEV
  XfburnUdevManager *udevman = xfburn_udev_manager_ref_global ();
  gchar *prev_addr = NULL;

  if (udevman == NULL) {
    return;
  }

  if (priv->curr_device != NULL) {
    g_object_get (G_OBJECT (priv->curr_device), "address", &prev_addr, NULL);
  }

  g_list_free_full (priv->devices, (GDestroyNotify) g_object_unref);
  priv->devices = NULL;
  priv->curr_device = NULL;

  priv->devices = xfburn_udev_manager_get_devices (udevman, &priv->num_drives, &priv->num_burners);
  g_object_unref (udevman);

  if (priv->num_burners > 0 && priv->devices != NULL) {
    if (prev_addr != NULL) {
      GList *l;
      for (l = priv->devices; l != NULL; l = l->next) {
        gchar *addr = NULL;
        g_object_get (G_OBJECT (l->data), "address", &addr, NULL);
        if (g_strcmp0 (addr, prev_addr) == 0) {
          priv->curr_device = XFBURN_DEVICE (l->data);
          g_free (addr);
          break;
        }
        g_free (addr);
      }
    }
    if (priv->curr_device == NULL) {
      priv->curr_device = XFBURN_DEVICE (priv->devices->data);
    }
  }

  g_free (prev_addr);
#else
  g_list_free_full (priv->devices, (GDestroyNotify) g_object_unref);
  priv->devices = NULL;
  priv->curr_device = NULL;
  get_libburn_device_list (devlist);
  if (priv->num_burners > 0 && priv->devices != NULL) {
    priv->curr_device = XFBURN_DEVICE (priv->devices->data);
  }
#endif
}

static void
start_refresh_async (XfburnDeviceList *devlist, gboolean device_changed_only)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);
  DeviceProbeTask *task;

  if (priv->shutting_down) {
    return;
  }

  if (xfburn_device_is_burning ()) {
    priv->needs_refresh = TRUE;
    return;
  }

  /* If a probe is already running, cancel it and queue the latest request so
   * stale results are discarded and the fresh probe starts as soon as it exits. */
  if (priv->refresh_in_progress) {
    if (!priv->refresh_queued) {
      priv->refresh_queued_device_only = device_changed_only;
    } else if (!device_changed_only) {
      priv->refresh_queued_device_only = FALSE;
    }
    priv->refresh_queued = TRUE;
    if (priv->refresh_cancellable != NULL) {
      g_cancellable_cancel (priv->refresh_cancellable);
    }
    return;
  }

  priv->needs_refresh = FALSE;
  priv->refresh_queued = FALSE;
  priv->refresh_queued_device_only = FALSE;
  priv->refresh_generation++;

  if (!device_changed_only) {
    rescan_udev_devices (devlist);
  }

  if (priv->curr_device == NULL) {
    g_signal_emit (G_OBJECT (devlist), signals[VOLUME_CHANGE_START], 0, device_changed_only);
    g_signal_emit (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, device_changed_only, NULL);
    return;
  }

  priv->refresh_in_progress = TRUE;
  g_clear_object (&priv->refresh_cancellable);
  priv->refresh_cancellable = g_cancellable_new ();

  g_signal_emit (G_OBJECT (devlist), signals[VOLUME_CHANGE_START], 0, device_changed_only);

  task = g_new0 (DeviceProbeTask, 1);
  task->devlist = g_object_ref (devlist);
  task->device = g_object_ref (priv->curr_device);
  g_object_get (G_OBJECT (priv->curr_device),
                "address", &task->addr,
                "name", &task->name,
                NULL);
  task->details_known = xfburn_device_details_known (priv->curr_device);
  task->device_changed = device_changed_only;
  task->generation = priv->refresh_generation;
  task->cancellable = g_object_ref (priv->refresh_cancellable);

  priv->active_probe_thread = g_thread_new ("device_probe", thread_probe_device, task);
}

static void
cb_combo_device_changed (GtkComboBox *combo, XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);
  XfburnDevice *device;

  device = get_selected_device (combo);

  if (device != NULL) {
    if (g_list_find (priv->devices, device) != NULL) {
      priv->curr_device = device;
      start_refresh_async (devlist, TRUE);
    }
    g_object_unref (device);
  } else {
    priv->curr_device = NULL;
    g_signal_emit (G_OBJECT (devlist), signals[VOLUME_CHANGE_START], 0, TRUE);
    g_signal_emit (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, TRUE, NULL);
  }
}

#ifdef HAVE_GUDEV
static gboolean
cb_udev_refresh_timeout (gpointer user_data)
{
  XfburnDeviceList *devlist = XFBURN_DEVICE_LIST (user_data);
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);

  priv->udev_refresh_timeout = 0;

  if (priv->shutting_down) {
    return FALSE;
  }

  if (xfburn_device_is_burning () ||
      !g_signal_has_handler_pending (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, FALSE)) {
    priv->needs_refresh = TRUE;
    return FALSE;
  }

  start_refresh_async (devlist, FALSE);
  return FALSE;
}

static void
cb_volumes_changed (XfburnUdevManager *udevman, XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);

  DBG ("Udev volume changed");

  if (priv->shutting_down) {
    return;
  }

  /* When no device-box/burn dialog is open or when a burn/verify thread holds the drive,
   * defer refreshing until a dialog is actually opened so XfBurn never blocks the UI
   * or auto-unmounts a newly burned disc in the background. */
  if (xfburn_device_is_burning () ||
      !g_signal_has_handler_pending (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, FALSE)) {
    priv->needs_refresh = TRUE;
    return;
  }

  if (priv->udev_refresh_timeout > 0)
    g_source_remove (priv->udev_refresh_timeout);

  priv->udev_refresh_timeout = gdk_threads_add_timeout (1000, cb_udev_refresh_timeout, devlist);
}
#endif

static void
cb_refresh_clicked (GtkButton *button, XfburnDeviceList *devlist)
{
  start_refresh_async (devlist, FALSE);
}

/**************/
/* public API */
/**************/

void
xfburn_device_list_shutdown (XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv;
  GThread *thread_to_join = NULL;
  guint idle_id = 0;

  if (devlist == NULL) {
    return;
  }

  priv = GET_PRIVATE (devlist);

  g_mutex_lock (&priv->probe_state_mutex);
  priv->shutting_down = TRUE;
  g_mutex_unlock (&priv->probe_state_mutex);

#ifdef HAVE_GUDEV
  g_clear_handle_id (&priv->udev_refresh_timeout, g_source_remove);
#endif

  priv->refresh_queued = FALSE;
  if (priv->refresh_cancellable != NULL) {
    g_cancellable_cancel (priv->refresh_cancellable);
  }

  if (priv->active_probe_thread != NULL) {
    thread_to_join = priv->active_probe_thread;
    priv->active_probe_thread = NULL;
    g_thread_join (thread_to_join);
  }

  g_mutex_lock (&priv->probe_state_mutex);
  idle_id = priv->probe_complete_idle_id;
  priv->probe_complete_idle_id = 0;
  g_mutex_unlock (&priv->probe_state_mutex);

  if (idle_id > 0) {
    g_source_remove (idle_id);
  }

  priv->refresh_in_progress = FALSE;
  g_clear_object (&priv->refresh_cancellable);
}

gboolean
xfburn_device_list_is_refreshing (XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv;

  g_return_val_if_fail (XFBURN_IS_DEVICE_LIST (devlist), FALSE);
  priv = GET_PRIVATE (devlist);
  return priv->refresh_in_progress;
}

gboolean
xfburn_device_list_needs_refresh (XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv;

  g_return_val_if_fail (XFBURN_IS_DEVICE_LIST (devlist), FALSE);
  priv = GET_PRIVATE (devlist);
  return priv->needs_refresh;
}

void
xfburn_device_list_refresh_async (XfburnDeviceList *devlist)
{
  g_return_if_fail (XFBURN_IS_DEVICE_LIST (devlist));
  start_refresh_async (devlist, FALSE);
}

void
xfburn_device_list_rescan_devices_only (XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv;

  g_return_if_fail (XFBURN_IS_DEVICE_LIST (devlist));
  priv = GET_PRIVATE (devlist);

  if (priv->shutting_down || priv->refresh_in_progress || xfburn_device_is_burning ()) {
    return;
  }

  rescan_udev_devices (devlist);
  priv->needs_refresh = (priv->num_burners > 0);
}

void
xfburn_device_list_cancel_refresh (XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv;

  g_return_if_fail (XFBURN_IS_DEVICE_LIST (devlist));
  priv = GET_PRIVATE (devlist);

  priv->refresh_queued = FALSE;
  if (priv->refresh_in_progress && priv->refresh_cancellable != NULL) {
    g_cancellable_cancel (priv->refresh_cancellable);
    priv->needs_refresh = TRUE;
  }
}

void
xfburn_device_list_cancel_refresh_if_unobserved (XfburnDeviceList *devlist)
{
  g_return_if_fail (XFBURN_IS_DEVICE_LIST (devlist));

  if (!g_signal_has_handler_pending (G_OBJECT (devlist), signals[VOLUME_CHANGE_END], 0, FALSE)) {
    xfburn_device_list_cancel_refresh (devlist);
  }
}

XfburnDevice *
xfburn_device_list_lookup_by_name (XfburnDeviceList *devlist, const gchar * name)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);
  GList *device;

  device = priv->devices;

  while (device) {
    XfburnDevice *device_data = XFBURN_DEVICE (device->data);
    gchar *devname = NULL;

    g_object_get (G_OBJECT (device_data), "name", &devname, NULL);

    if (devname != NULL && g_ascii_strcasecmp (devname, name) == 0) {
      g_free (devname);
      return device_data;
    }

    g_free (devname);

    device = g_list_next (device);
  }

  return NULL;
}

/**
 * Replace all devices in the device combo drop-down menu.
 */
void
xfburn_device_list_refresh_device_combo (XfburnDeviceList *devlist, GtkWidget *combo_device)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);
  GtkListStore *store = GTK_LIST_STORE (gtk_combo_box_get_model (GTK_COMBO_BOX (combo_device)));
  GList *device = priv->devices;
  int i = 0;
  int selected = -1;

  /* Temporarily disconnect this signal handler to prevent an infinite loop
     (cb_combo_device_changed calling this function, which calls that). */
  g_signal_handlers_disconnect_by_func (G_OBJECT (combo_device), G_CALLBACK (cb_combo_device_changed), devlist);

  gtk_list_store_clear (store);

  while (device) {
    XfburnDevice *device_data = (XfburnDevice *) device->data;
    GtkTreeIter iter;
    gchar *name = NULL;

    g_object_get (G_OBJECT (device_data), "name", &name, NULL);

    gtk_list_store_append (store, &iter);
    gtk_list_store_set (store, &iter, DEVICE_NAME_COLUMN, name, DEVICE_POINTER_COLUMN, device_data, -1);
    g_free (name);

    if (device_data == priv->curr_device) {
      selected = i;
    }

    device = g_list_next (device);
    i++;
  }

  gtk_widget_set_sensitive (combo_device, priv->num_burners > 0);

  gtk_combo_box_set_active (GTK_COMBO_BOX (combo_device), selected);

  /* Reconnect the signal handler. */
  g_signal_connect (G_OBJECT (combo_device), "changed", G_CALLBACK (cb_combo_device_changed), devlist);
}

GtkWidget *
xfburn_device_list_get_device_combo (XfburnDeviceList *devlist)
{
  GtkWidget *combo_device;
  GtkListStore *store = NULL;
  GtkCellRenderer *cell;

  store = gtk_list_store_new (DEVICE_N_COLUMNS, G_TYPE_STRING, XFBURN_TYPE_DEVICE);
  combo_device = gtk_combo_box_new_with_model (GTK_TREE_MODEL (store));
  g_object_unref (store);

  cell = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo_device), cell, TRUE);
  gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo_device), cell, "text", DEVICE_NAME_COLUMN, NULL);
  gtk_widget_show (combo_device);

  xfburn_device_list_refresh_device_combo (devlist, combo_device);

  return combo_device;
}

GtkWidget *
xfburn_device_list_get_refresh_button (XfburnDeviceList *devlist)
{
  GtkWidget *img, *button;

  img = gtk_image_new_from_icon_name ("view-refresh", GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_widget_show (img);
  button = gtk_button_new ();
  gtk_container_add (GTK_CONTAINER (button), img);
  gtk_widget_show (button);

  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (cb_refresh_clicked), devlist);

  return button;
}

/* Note that the device is NOT ref'ed, so if the caller wants to hold on to a copy,
   g_object_ref () has to be called manually, or just use the property "current-device" */
XfburnDevice *
xfburn_device_list_get_current_device (XfburnDeviceList *devlist)
{
  XfburnDeviceListPrivate *priv = GET_PRIVATE (devlist);

  return priv->curr_device;
}


XfburnDeviceList*
xfburn_device_list_new (void)
{
  return g_object_new (XFBURN_TYPE_DEVICE_LIST, NULL);
}


