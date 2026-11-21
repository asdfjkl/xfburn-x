/*
 *  Copyright (c) 2005-2006 Jean-François Wauthy (pollux@xfce.org)
 *  Copyright (c) 2008      David Mohr (dmohr@mcbf.net)
 *  Copyright (c) 2026      Dominik Klein
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

#ifdef HAVE_GUDEV

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <sys/stat.h>

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <gudev/gudev.h>

#include <errno.h>

#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>

#include "xfburn-global.h"
#include "xfburn-progress-dialog.h"
#include "xfburn-device-list.h"
#include "xfburn-log.h"

#include "xfburn-udev-manager.h"

#define PROBE_RELEASE_COOLDOWN_US (2 * G_USEC_PER_SEC)
#define SELF_UNMOUNT_COOLDOWN_US  (2 * G_USEC_PER_SEC)

static void xfburn_udev_manager_finalize (GObject * object);

static GObject * xfburn_udev_manager_new (void);

static void cb_device_monitor_uevent(GUdevClient  *client, const gchar  *action, GUdevDevice  *udevice, XfburnUdevManager *obj);
static void cb_volume_monitor_mount_removed (GVolumeMonitor *monitor, GMount *mount, XfburnUdevManager *obj);
static gboolean cb_idle_emit_volume_changed (gpointer user_data);

#define XFBURN_UDEV_MANAGER_GET_PRIVATE(obj) (xfburn_udev_manager_get_instance_private (XFBURN_UDEV_MANAGER (obj)))

enum {
  VOLUME_CHANGED,
  LAST_SIGNAL,
};

typedef struct _XfburnUdevManagerPrivate
{
  GUdevClient *client;
  GVolumeMonitor *volume_monitor;
  gulong uevent_handler_id;
  gulong mount_removed_handler_id;
  GThread *main_thread;
  gint self_unmount_count;
  gint64 last_self_unmount_us;
  gchar *error;
} XfburnUdevManagerPrivate;

typedef struct {
  gint ref_count;
  GMutex mutex;
  GCond cond;
  gboolean done;
  XfburnUmountResult result;
  gchar *device_file;
  gchar *mount_name;
  gchar *error_msg;
  XfburnUdevManager *udevman;
  GMount *mount;
  GCancellable *op_cancellable;
  gulong caller_cancel_id;
  guint timeout_id;
} XfburnUnmountOp;

static XfburnUdevManager *instance = NULL;
static GMutex instance_mutex;

static GMutex probe_release_mutex;
static gint64 last_probe_release_us = 0;

/*********************/
/* class declaration */
/*********************/
static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_PRIVATE(XfburnUdevManager, xfburn_udev_manager, G_TYPE_OBJECT);

static void
xfburn_udev_manager_class_init (XfburnUdevManagerClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xfburn_udev_manager_finalize;

  signals[VOLUME_CHANGED] = g_signal_new ("volume-changed", XFBURN_TYPE_UDEV_MANAGER, G_SIGNAL_ACTION,
                                          G_STRUCT_OFFSET (XfburnUdevManagerClass, volume_changed),
                                          NULL, NULL, g_cclosure_marshal_VOID__VOID,
                                          G_TYPE_NONE, 0);
}

static void
xfburn_udev_manager_init (XfburnUdevManager * obj)
{
  XfburnUdevManagerPrivate *priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (obj);
  const gchar* const subsystems[] = { "block", "scsi", "usb", NULL };

  priv->error = NULL;
  priv->volume_monitor = NULL;
  priv->uevent_handler_id = 0;
  priv->mount_removed_handler_id = 0;
  priv->main_thread = g_thread_self ();
  priv->self_unmount_count = 0;
  priv->last_self_unmount_us = 0;

  priv->client = g_udev_client_new (subsystems);
  if (G_LIKELY (priv->client != NULL)) {
    priv->uevent_handler_id = g_signal_connect (G_OBJECT (priv->client), "uevent",
                                                G_CALLBACK (cb_device_monitor_uevent), obj);

    priv->volume_monitor = g_volume_monitor_get();
    if (priv->volume_monitor == NULL) {
      g_warning ("Error trying to access g_volume_monitor!");
    } else {
      priv->mount_removed_handler_id = g_signal_connect (G_OBJECT (priv->volume_monitor),
                                                         "mount-removed",
                                                         G_CALLBACK (cb_volume_monitor_mount_removed),
                                                         obj);
    }
  }
}

static void
xfburn_udev_manager_finalize (GObject * object)
{
  XfburnUdevManagerPrivate *priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (object);

  if (priv->volume_monitor != NULL) {
    if (priv->mount_removed_handler_id != 0) {
      g_signal_handler_disconnect (priv->volume_monitor, priv->mount_removed_handler_id);
      priv->mount_removed_handler_id = 0;
    }
    g_clear_object (&priv->volume_monitor);
  }

  if (priv->client != NULL) {
    if (priv->uevent_handler_id != 0) {
      g_signal_handler_disconnect (priv->client, priv->uevent_handler_id);
      priv->uevent_handler_id = 0;
    }
    g_clear_object (&priv->client);
  }

  g_clear_pointer (&priv->error, g_free);

  g_mutex_lock (&instance_mutex);
  if (instance == XFBURN_UDEV_MANAGER (object)) {
    instance = NULL;
  }
  g_mutex_unlock (&instance_mutex);

  G_OBJECT_CLASS (xfburn_udev_manager_parent_class)->finalize (object);
}

static gboolean
udevice_is_cdrom (GUdevDevice *udevice)
{
  const gchar *id_type;

  if (udevice == NULL) {
    return FALSE;
  }

  id_type = g_udev_device_get_property (udevice, "ID_TYPE");
  return g_udev_device_get_property_as_boolean (udevice, "ID_CDROM") ||
         g_strcmp0 (id_type, "cd") == 0 ||
         g_strcmp0 (id_type, "cd/dvd") == 0;
}

static void
cb_volume_monitor_mount_removed (GVolumeMonitor *monitor,
                                 GMount *mount,
                                 XfburnUdevManager *obj)
{
  XfburnUdevManagerPrivate *priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (obj);
  GVolume *vol;
  gchar *devname;
  GUdevDevice *udevice;
  gboolean is_cdrom = FALSE;
  gint64 now_us = g_get_monotonic_time ();

  if (priv->self_unmount_count > 0 ||
      (priv->last_self_unmount_us > 0 && (now_us - priv->last_self_unmount_us) < SELF_UNMOUNT_COOLDOWN_US)) {
    return;
  }

  if (mount == NULL || priv->client == NULL) {
    return;
  }

  vol = g_mount_get_volume (mount);
  if (vol == NULL) {
    return;
  }

  devname = g_volume_get_identifier (vol, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
  g_object_unref (vol);
  if (devname == NULL) {
    return;
  }

  udevice = g_udev_client_query_by_device_file (priv->client, devname);
  if (udevice != NULL) {
    is_cdrom = udevice_is_cdrom (udevice);
    g_object_unref (udevice);
  }

  if (is_cdrom) {
    XFBURN_LOG_INFO ("UDEV", "External mount removed for optical device=\"%s\"", devname);
    gdk_threads_add_idle_full (G_PRIORITY_DEFAULT_IDLE,
                               cb_idle_emit_volume_changed,
                               g_object_ref (obj),
                               g_object_unref);
  }

  g_free (devname);
}

static void cb_device_monitor_uevent(GUdevClient  *client,
				     const gchar  *action,
				     GUdevDevice  *udevice,
				     XfburnUdevManager *obj)
{
  const gchar *subsys;
  const gchar *devfile;
  const gchar *sysfs_path;
  const gchar *id_bus;
  const gchar *id_model;
  const gchar *media_chg = NULL;
  gboolean is_cdrom;

  DBG ("UDEV: device uevent: %s", action);

  if (udevice == NULL || action == NULL) {
    return;
  }

  subsys = g_udev_device_get_subsystem (udevice);
  devfile = g_udev_device_get_device_file (udevice);
  sysfs_path = g_udev_device_get_sysfs_path (udevice);
  id_bus = g_udev_device_get_property (udevice, "ID_BUS");
  id_model = g_udev_device_get_property (udevice, "ID_MODEL");

  is_cdrom = udevice_is_cdrom (udevice);

  if (!is_cdrom) {
    if (g_strcmp0 (subsys, "usb") == 0 || g_strcmp0 (subsys, "scsi") == 0) {
      const gchar *act_str = action;
      const gchar *sub_str = "unknown";
      const gchar *dev_str = "none";
      const gchar *sys_str = "unknown";
      const gchar *mod_str = "unknown";

      if (subsys != NULL) {
        sub_str = subsys;
      }
      if (devfile != NULL) {
        dev_str = devfile;
      }
      if (sysfs_path != NULL) {
        sys_str = sysfs_path;
      }
      if (id_model != NULL) {
        mod_str = id_model;
      }

      if (g_str_equal (action, "remove") || g_str_equal (action, "unbind")) {
        XFBURN_LOG_WARN ("UDEV",
                         "uevent action=\"%s\" subsys=\"%s\" dev=\"%s\" model=\"%s\" sysfs=\"%s\"",
                         act_str, sub_str, dev_str, mod_str, sys_str);
      } else {
        XFBURN_LOG_INFO ("UDEV",
                         "uevent action=\"%s\" subsys=\"%s\" dev=\"%s\" model=\"%s\" sysfs=\"%s\"",
                         act_str, sub_str, dev_str, mod_str, sys_str);
      }
    }
    return;
  }

  media_chg = g_udev_device_get_property (udevice, "DISK_MEDIA_CHANGE");

  {
    const gchar *sub_str = "block";
    const gchar *dev_str = "unknown";
    const gchar *bus_str = "unknown";
    const gchar *mod_str = "unknown";
    const gchar *fs_type = g_udev_device_get_property (udevice, "ID_FS_TYPE");
    const gchar *fs_str = "none";
    const gchar *chg_str = "0";

    if (subsys != NULL) {
      sub_str = subsys;
    }
    if (devfile != NULL) {
      dev_str = devfile;
    }
    if (id_bus != NULL) {
      bus_str = id_bus;
    }
    if (id_model != NULL) {
      mod_str = id_model;
    }
    if (fs_type != NULL) {
      fs_str = fs_type;
    }
    if (media_chg != NULL) {
      chg_str = media_chg;
    }

    if (g_str_equal (action, "remove")) {
      XFBURN_LOG_WARN ("UDEV",
                       "cdrom uevent action=\"%s\" subsys=\"%s\" dev=\"%s\" bus=\"%s\" model=\"%s\" fs=\"%s\" media_change=\"%s\"",
                       action, sub_str, dev_str, bus_str, mod_str, fs_str, chg_str);
    } else {
      XFBURN_LOG_INFO ("UDEV",
                       "cdrom uevent action=\"%s\" subsys=\"%s\" dev=\"%s\" bus=\"%s\" model=\"%s\" fs=\"%s\" media_change=\"%s\"",
                       action, sub_str, dev_str, bus_str, mod_str, fs_str, chg_str);
    }
  }

  if (g_str_equal (action, "remove") || g_str_equal (action, "add") ||
      g_str_equal (action, "change")) {
    if (xfburn_udev_manager_should_ignore_uevent (action, media_chg, g_get_monotonic_time ())) {
      XFBURN_LOG_DEBUG ("UDEV",
                        "Ignoring synthetic change uevent within probe-release cooldown (dev=\"%s\")",
                        devfile != NULL ? devfile : "unknown");
      return;
    }

    gdk_threads_add_idle_full (G_PRIORITY_DEFAULT_IDLE,
                               cb_idle_emit_volume_changed,
                               g_object_ref (obj),
                               g_object_unref);
  }
}

static GObject *
xfburn_udev_manager_new (void)
{
  if (G_UNLIKELY (instance != NULL))
    g_error ("Trying to create a second instance of udev manager!");
  return g_object_new (XFBURN_TYPE_UDEV_MANAGER, NULL);
}

static gboolean
cb_idle_emit_volume_changed (gpointer user_data)
{
  XfburnUdevManager *udevman = XFBURN_UDEV_MANAGER (user_data);
  gboolean active;

  g_mutex_lock (&instance_mutex);
  active = (instance == udevman);
  g_mutex_unlock (&instance_mutex);

  if (active) {
    g_signal_emit (udevman, signals[VOLUME_CHANGED], 0);
  }
  return G_SOURCE_REMOVE;
}

/*        */
/* public */
/*        */

gchar *
xfburn_udev_manager_create_global (void)
{
  XfburnUdevManagerPrivate *priv;
  XfburnUdevManager *obj;

  obj = XFBURN_UDEV_MANAGER (xfburn_udev_manager_new ());

  g_mutex_lock (&instance_mutex);
  instance = obj;
  g_mutex_unlock (&instance_mutex);

  priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (obj);

  if (priv->error) {
    gchar *error_msg, *ret;

    error_msg = g_strdup (priv->error);
    xfburn_udev_manager_shutdown ();
    ret = g_strdup_printf ("Failed to initialize %s!", error_msg);
    g_free (error_msg);
    return ret;
  } else
    return NULL;
}

XfburnUdevManager *
xfburn_udev_manager_get_global (void)
{
  XfburnUdevManager *obj;

  g_mutex_lock (&instance_mutex);
  obj = instance;
  g_mutex_unlock (&instance_mutex);

  if (G_UNLIKELY (obj == NULL))
    g_error ("There is no instance of a udev manager!");
  return obj;
}

XfburnUdevManager *
xfburn_udev_manager_ref_global (void)
{
  XfburnUdevManager *obj = NULL;

  g_mutex_lock (&instance_mutex);
  if (instance != NULL) {
    obj = g_object_ref (instance);
  }
  g_mutex_unlock (&instance_mutex);

  return obj;
}

void
xfburn_udev_manager_shutdown (void)
{
  XfburnUdevManager *obj = NULL;

  g_mutex_lock (&instance_mutex);
  obj = instance;
  instance = NULL;
  g_mutex_unlock (&instance_mutex);

  if (G_UNLIKELY (obj == NULL))
    g_error ("There is no instance of a udev manager!");
  g_object_unref (obj);
}

void
xfburn_udev_manager_send_volume_changed (void)
{
  XfburnUdevManager *udevman = xfburn_udev_manager_ref_global ();
  XfburnUdevManagerPrivate *priv;

  if (udevman == NULL) {
    return;
  }

  priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (udevman);
  if (g_thread_self () == priv->main_thread) {
    g_signal_emit (udevman, signals[VOLUME_CHANGED], 0);
    g_object_unref (udevman);
  } else {
    gdk_threads_add_idle_full (G_PRIORITY_DEFAULT_IDLE,
                               cb_idle_emit_volume_changed,
                               udevman,
                               g_object_unref);
  }
}

void
xfburn_udev_manager_note_probe_release (void)
{
  g_mutex_lock (&probe_release_mutex);
  last_probe_release_us = g_get_monotonic_time ();
  g_mutex_unlock (&probe_release_mutex);
}

gboolean
xfburn_udev_manager_should_ignore_uevent (const gchar *action,
                                          const gchar *media_change,
                                          gint64 now_us)
{
  gint64 last_us;

  if (g_strcmp0 (action, "change") != 0) {
    return FALSE;
  }

  /* Never ignore an actual hardware media insertion/ejection event */
  if (g_strcmp0 (media_change, "1") == 0) {
    return FALSE;
  }

  g_mutex_lock (&probe_release_mutex);
  last_us = last_probe_release_us;
  g_mutex_unlock (&probe_release_mutex);

  if (last_us > 0 && now_us >= last_us && (now_us - last_us) < PROBE_RELEASE_COOLDOWN_US) {
    return TRUE;
  }

  return FALSE;
}

gboolean
xfburn_udev_manager_is_mounted_in_kernel (const gchar *device_file)
{
  GList *mounts;
  GList *l;
  struct stat target_st;
  gboolean have_target_st = FALSE;
  gboolean found = FALSE;

  if (device_file == NULL || device_file[0] == '\0') {
    return FALSE;
  }

  if (stat (device_file, &target_st) == 0 && S_ISBLK (target_st.st_mode)) {
    have_target_st = TRUE;
  }

  mounts = g_unix_mounts_get (NULL);
  for (l = mounts; l != NULL; l = l->next) {
    GUnixMountEntry *entry = (GUnixMountEntry *) l->data;
    const gchar *dev_path = g_unix_mount_get_device_path (entry);

    if (dev_path != NULL && !found) {
      if (g_strcmp0 (dev_path, device_file) == 0) {
        found = TRUE;
      } else if (have_target_st && dev_path[0] == '/') {
        struct stat entry_st;
        if (stat (dev_path, &entry_st) == 0 &&
            S_ISBLK (entry_st.st_mode) &&
            entry_st.st_rdev == target_st.st_rdev) {
          found = TRUE;
        }
      }
    }

    g_unix_mount_free (entry);
  }
  g_list_free (mounts);

  return found;
}

GList *
xfburn_udev_manager_get_devices (XfburnUdevManager *udevman, gint *drives, gint *burners)
{
  XfburnUdevManagerPrivate *priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (udevman);
  GList *devices, *l;
  GList *device_list = NULL;

  (*drives) = 0;
  (*burners) = 0;

  if (priv->client == NULL)
    return NULL;

  devices = g_udev_client_query_by_subsystem (priv->client, "block");
  for (l = devices; l != NULL; l = l->next) {
    const gchar *id_type = g_udev_device_get_property (l->data, "ID_TYPE");
    if (g_strcmp0 (id_type, "cd") == 0 || g_strcmp0 (id_type, "cd/dvd") == 0) {
      gboolean cdr = g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_CD_R");
      gboolean cdrw = g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_CD_RW");
      gboolean dvdr = g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_DVD_R")
      	           || g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_DVD_RW")
                   || g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_DVD_PLUS_R")
                   || g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_DVD_PLUS_RW")
                   || g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_DVD_PLUS_R_DL");
      gboolean dvdram = g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_DVD_RAM");
      gboolean bdr = g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_BD")
                  || g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_BD_R")
                  || g_udev_device_get_property_as_boolean(l->data, "ID_CDROM_BD_RE");

      (*drives)++;

      if (cdr || dvdr || dvdram || bdr) {
        XfburnDevice *device;
        const gchar *addr, *name, *rev, *str_model, *str_vendor;

        device = xfburn_device_new ();

	/* vendor */
	str_vendor = g_udev_device_get_sysfs_attr(l->data, "device/vendor");
        if (str_vendor == NULL)
	  str_vendor = g_udev_device_get_property (l->data, "ID_VENDOR_FROM_DATABASE");
        if (str_vendor == NULL)
          str_vendor = g_udev_device_get_property (l->data, "ID_VENDOR");
	if (str_vendor == NULL)
          str_vendor = g_udev_device_get_sysfs_attr (l->data, "manufacturer");

        /* model */
	str_model = g_udev_device_get_sysfs_attr(l->data, "device/model");
        if (str_model == NULL)
          str_model = g_udev_device_get_property (l->data, "ID_MODEL_FROM_DATABASE");
        if (str_model == NULL)
          str_model = g_udev_device_get_property (l->data, "ID_MODEL");
        if (str_model == NULL)
          str_model = g_udev_device_get_sysfs_attr (l->data, "product");

        name = xfburn_device_set_name (device, str_vendor, str_model);

        /* revision */
	rev = g_udev_device_get_sysfs_attr (l->data, "device/rev");
        if (rev == NULL)
          rev = g_udev_device_get_property (l->data, "ID_REVISION");
        g_object_set (G_OBJECT (device), "revision", rev, NULL);

        addr = g_udev_device_get_device_file(l->data);
#ifdef DEBUG_NULL_DEVICE
        g_object_set (G_OBJECT (device), "address", "stdio:/dev/null", NULL);
#else
        g_object_set (G_OBJECT (device), "address", addr, NULL);
#endif

        g_object_set (G_OBJECT (device), "cdr", cdr, NULL);
        g_object_set (G_OBJECT (device), "cdrw", cdrw, NULL);
        g_object_set (G_OBJECT (device), "dvdr", dvdr, NULL);
        g_object_set (G_OBJECT (device), "dvdram", dvdram, NULL);
        g_object_set (G_OBJECT (device), "bd", bdr, NULL);

        if (!xfburn_device_can_burn (device)) {
          g_message ("Ignoring reader '%s' at '%s'", name, addr);
          g_object_unref (device);
        } else {
          (*burners)++;
          device_list = g_list_append (device_list, device);
          DBG ("Found writer '%s' at '%s'", name, addr);
          XFBURN_LOG_INFO ("UDEV", "Discovered writer name=\"%s\" addr=\"%s\" rev=\"%s\" cdr=%d cdrw=%d dvdr=%d dvdram=%d bd=%d",
                           name, addr, rev, cdr, cdrw, dvdr, dvdram, bdr);
          xfburn_log_dump_device_hardware (addr, NULL);
        }
      }
    }
    g_object_unref (l->data);
  }
  g_list_free (devices);
  return device_list;
}

static XfburnUnmountOp *
unmount_op_ref (XfburnUnmountOp *op)
{
  g_atomic_int_inc (&op->ref_count);
  return op;
}

static void
unmount_op_unref (XfburnUnmountOp *op)
{
  if (op == NULL) {
    return;
  }

  if (g_atomic_int_dec_and_test (&op->ref_count)) {
    g_clear_object (&op->mount);
    g_clear_object (&op->op_cancellable);
    g_clear_object (&op->udevman);
    g_free (op->device_file);
    g_free (op->mount_name);
    g_free (op->error_msg);
    g_cond_clear (&op->cond);
    g_mutex_clear (&op->mutex);
    g_free (op);
  }
}

static void
unmount_op_complete (XfburnUnmountOp *op,
                     XfburnUmountResult result,
                     const gchar *error_msg)
{
  g_mutex_lock (&op->mutex);
  if (!op->done) {
    op->result = result;
    if (error_msg != NULL && op->error_msg == NULL) {
      op->error_msg = g_strdup (error_msg);
    }
    op->done = TRUE;
    g_cond_broadcast (&op->cond);
  }
  g_mutex_unlock (&op->mutex);
}

static void
cb_caller_unmount_cancelled (GCancellable *cancellable,
                             gpointer user_data)
{
  XfburnUnmountOp *op = (XfburnUnmountOp *) user_data;

  g_cancellable_cancel (op->op_cancellable);
  unmount_op_complete (op, XFBURN_UMOUNT_CANCELLED, NULL);
}

static gboolean
cb_main_unmount_timeout (gpointer user_data)
{
  XfburnUnmountOp *op = (XfburnUnmountOp *) user_data;

  op->timeout_id = 0;

  XFBURN_LOG_WARN ("UDEV",
                   "Volume/Disc GIO unmount operation timed out after 15 seconds for device=\"%s\"",
                   op->device_file);
  g_warning ("Volume/Disc unmount operation timed out for '%s'", op->device_file);

  g_cancellable_cancel (op->op_cancellable);
  unmount_op_complete (op, XFBURN_UMOUNT_FAILED, _("Unmount operation timed out"));

  return G_SOURCE_REMOVE;
}

static void
cb_main_unmount_finish (GObject *source,
                        GAsyncResult *result,
                        gpointer user_data)
{
  XfburnUnmountOp *op = (XfburnUnmountOp *) user_data;
  XfburnUdevManagerPrivate *priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (op->udevman);
  GError *err = NULL;
  gboolean ok;
  XfburnUmountResult res_code;
  const gchar *err_text = NULL;

  ok = g_mount_unmount_with_operation_finish (G_MOUNT (source), result, &err);

  if (priv->self_unmount_count > 0) {
    priv->self_unmount_count--;
  }

  g_clear_handle_id (&op->timeout_id, g_source_remove);

  g_debug ("Umount operation completed (result = %d)", ok);

  if (err != NULL) {
    if (err->code == G_IO_ERROR_NOT_MOUNTED) {
      g_clear_error (&err);
      ok = TRUE;
    } else if (err->code == G_IO_ERROR_CANCELLED) {
      XFBURN_LOG_INFO ("UDEV", "GIO unmount cancelled for device=\"%s\"", op->device_file);
      res_code = XFBURN_UMOUNT_CANCELLED;
      g_clear_error (&err);
      goto done;
    } else {
      XFBURN_LOG_ERROR ("UDEV",
                        "GIO unmount error for device=\"%s\": code=%d msg=\"%s\"",
                        op->device_file, err->code, err->message);
      g_warning ("Failed to unmount '%s': %s",
                 op->mount_name != NULL ? op->mount_name : op->device_file,
                 err->message);
      err_text = err->message;
      res_code = XFBURN_UMOUNT_FAILED;
      goto done;
    }
  }

  if (ok) {
    priv->last_self_unmount_us = g_get_monotonic_time ();
    res_code = XFBURN_UMOUNT_UNMOUNTED;
    g_message ("Unmounted '%s'", op->mount_name != NULL ? op->mount_name : op->device_file);
  } else {
    res_code = XFBURN_UMOUNT_FAILED;
  }

done:
  XFBURN_LOG_INFO ("UDEV",
                   "Finished async GIO unmount for device=\"%s\" result=%d",
                   op->device_file, (int) res_code);

  g_clear_object (&op->mount);
  unmount_op_complete (op, res_code, err_text);
  g_clear_error (&err);
  unmount_op_unref (op);
}

static gboolean
device_paths_match (const gchar *path_a, const gchar *path_b)
{
  struct stat st_a;
  struct stat st_b;

  if (path_a == NULL || path_b == NULL) {
    return FALSE;
  }

  if (g_strcmp0 (path_a, path_b) == 0) {
    return TRUE;
  }

  if (path_a[0] == '/' && path_b[0] == '/' &&
      stat (path_a, &st_a) == 0 && stat (path_b, &st_b) == 0 &&
      S_ISBLK (st_a.st_mode) && S_ISBLK (st_b.st_mode) &&
      st_a.st_rdev == st_b.st_rdev) {
    return TRUE;
  }

  return FALSE;
}

static gboolean
cb_main_start_unmount (gpointer user_data)
{
  XfburnUnmountOp *op = (XfburnUnmountOp *) user_data;
  XfburnUdevManagerPrivate *priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (op->udevman);
  GList *mounts;
  GList *iter;
  GMount *matched_mount = NULL;

  if (g_cancellable_is_cancelled (op->op_cancellable)) {
    unmount_op_complete (op, XFBURN_UMOUNT_CANCELLED, NULL);
    return G_SOURCE_REMOVE;
  }

  if (priv->volume_monitor == NULL) {
    unmount_op_complete (op, XFBURN_UMOUNT_NOT_MOUNTED, NULL);
    return G_SOURCE_REMOVE;
  }

  mounts = g_volume_monitor_get_mounts (priv->volume_monitor);
  for (iter = mounts; iter != NULL; iter = iter->next) {
    GMount *m = G_MOUNT (iter->data);

    if (matched_mount == NULL) {
      GVolume *v = g_mount_get_volume (m);
      if (v != NULL) {
        gchar *devname = g_volume_get_identifier (v, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
        if (device_paths_match (devname, op->device_file)) {
          matched_mount = g_object_ref (m);
        }
        g_free (devname);
        g_object_unref (v);
      }
    }

    g_object_unref (m);
  }
  g_list_free (mounts);

  if (matched_mount == NULL) {
    unmount_op_complete (op, XFBURN_UMOUNT_NOT_MOUNTED, NULL);
    return G_SOURCE_REMOVE;
  }

  if (!g_mount_can_unmount (matched_mount)) {
    gchar *mp = g_mount_get_name (matched_mount);
    XFBURN_LOG_WARN ("UDEV", "Mount \"%s\" on device=\"%s\" cannot be unmounted",
                     mp != NULL ? mp : "unknown", op->device_file);
    g_free (mp);
    g_object_unref (matched_mount);
    unmount_op_complete (op, XFBURN_UMOUNT_FAILED, _("Mounted volume cannot be unmounted"));
    return G_SOURCE_REMOVE;
  }

  op->mount = matched_mount;
  op->mount_name = g_mount_get_name (matched_mount);
  priv->self_unmount_count++;

  XFBURN_LOG_INFO ("UDEV",
                   "Starting async GIO unmount for device=\"%s\" mount=\"%s\"",
                   op->device_file,
                   op->mount_name != NULL ? op->mount_name : "unknown");

  op->timeout_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                               15,
                                               cb_main_unmount_timeout,
                                               unmount_op_ref (op),
                                               (GDestroyNotify) unmount_op_unref);

  g_mount_unmount_with_operation (matched_mount,
                                  G_MOUNT_UNMOUNT_NONE,
                                  NULL,
                                  op->op_cancellable,
                                  cb_main_unmount_finish,
                                  unmount_op_ref (op));

  return G_SOURCE_REMOVE;
}

XfburnUmountResult
xfburn_udev_manager_unmount_device (XfburnUdevManager *udevman,
                                    const gchar *device_file,
                                    GCancellable *cancellable,
                                    gchar **error_msg)
{
  XfburnUdevManagerPrivate *priv;
  XfburnUnmountOp *op;
  XfburnUmountResult res;
  gint64 deadline;
  gboolean timed_out = FALSE;

  if (error_msg != NULL) {
    *error_msg = NULL;
  }

  if (udevman == NULL || device_file == NULL || device_file[0] == '\0') {
    return XFBURN_UMOUNT_NOT_MOUNTED;
  }

  if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
    return XFBURN_UMOUNT_CANCELLED;
  }

  priv = XFBURN_UDEV_MANAGER_GET_PRIVATE (udevman);

  /* Never block or spin a nested main loop on the GTK main thread. */
  if (g_thread_self () == priv->main_thread) {
    XFBURN_LOG_WARN ("UDEV",
                     "Refusing synchronous unmount on main thread for device=\"%s\"",
                     device_file);
    g_warning ("xfburn_udev_manager_unmount_device called on main thread; refusing to block UI");
    return XFBURN_UMOUNT_FAILED;
  }

  op = g_new0 (XfburnUnmountOp, 1);
  op->ref_count = 1;
  g_mutex_init (&op->mutex);
  g_cond_init (&op->cond);
  op->done = FALSE;
  op->result = XFBURN_UMOUNT_FAILED;
  op->device_file = g_strdup (device_file);
  op->udevman = g_object_ref (udevman);
  op->op_cancellable = g_cancellable_new ();

  if (cancellable != NULL) {
    op->caller_cancel_id = g_cancellable_connect (cancellable,
                                                  G_CALLBACK (cb_caller_unmount_cancelled),
                                                  unmount_op_ref (op),
                                                  (GDestroyNotify) unmount_op_unref);
  }

  g_main_context_invoke_full (NULL,
                              G_PRIORITY_DEFAULT,
                              cb_main_start_unmount,
                              unmount_op_ref (op),
                              (GDestroyNotify) unmount_op_unref);

  deadline = g_get_monotonic_time () + 16 * G_USEC_PER_SEC;
  g_mutex_lock (&op->mutex);
  while (!op->done) {
    if (!g_cond_wait_until (&op->cond, &op->mutex, deadline)) {
      XFBURN_LOG_WARN ("UDEV",
                       "Worker wait for GIO unmount timed out for device=\"%s\"",
                       device_file);
      op->result = XFBURN_UMOUNT_FAILED;
      op->done = TRUE;
      timed_out = TRUE;
      break;
    }
  }
  res = op->result;
  if (error_msg != NULL && op->error_msg != NULL) {
    *error_msg = g_strdup (op->error_msg);
  }
  g_mutex_unlock (&op->mutex);

  if (timed_out) {
    g_cancellable_cancel (op->op_cancellable);
  }

  if (cancellable != NULL && op->caller_cancel_id != 0) {
    g_cancellable_disconnect (cancellable, op->caller_cancel_id);
    op->caller_cancel_id = 0;
  }

  unmount_op_unref (op);
  return res;
}

/* @Return TRUE if the drive is now accessible (not mounted or successfully unmounted), FALSE if not.
 */
gboolean
xfburn_udev_manager_check_ask_umount (XfburnUdevManager *udevman, XfburnDevice *device)
{
  gchar *device_file = NULL;
  XfburnUmountResult res;

  if (udevman == NULL || device == NULL) {
    return FALSE;
  }

  g_object_get (G_OBJECT (device), "address", &device_file, NULL);
  res = xfburn_udev_manager_unmount_device (udevman, device_file, NULL, NULL);
  g_free (device_file);

  return (res == XFBURN_UMOUNT_NOT_MOUNTED || res == XFBURN_UMOUNT_UNMOUNTED);
}

#endif /* HAVE_GUDEV */
