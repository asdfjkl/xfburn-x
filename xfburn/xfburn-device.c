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

#include <gtk/gtk.h>
#include <libxfce4util/libxfce4util.h>
#include <unistd.h>

#include "xfburn-device.h"
#include "xfburn-device-list.h"
#include "xfburn-udev-manager.h"
#include "xfburn-log.h"

/*- globals -*/

enum {
  PROP_0,
  PROP_NAME,
  PROP_ADDRESS,
  PROP_REVISION,
  PROP_SUPPORTED_SPEEDS,
  PROP_DISC_STATUS,
  PROP_PROFILE_NO,
  PROP_PROFILE_NAME,
  PROP_ERASABLE,
  PROP_CDR,
  PROP_CDRW,
  PROP_DVDR,
  PROP_DVDPLUSR,
  PROP_DVDRAM,
  PROP_BD,
  PROP_TAO_BLOCK_TYPES,
  PROP_SAO_BLOCK_TYPES,
  PROP_RAW_BLOCK_TYPES,
  PROP_PACKET_BLOCK_TYPES,
};

/*- private prototypes -*/

#define CAN_BURN(priv) ((priv)->cdr || (priv)->cdrw || (priv)->dvdr || (priv)->dvdram)
#define DEVICE_INFO_PRINTF(device) "%s can burn: %d [cdr: %d, cdrw: %d, dvdr: %d, dvdram: %d]", (device)->name, CAN_BURN (device), (device)->cdr, (device)->cdrw, (device)->dvdr, (device)->dvdram


/*****************/
/*- class setup -*/
/*****************/
typedef struct _XfburnDevicePrivate XfburnDevicePrivate;

struct _XfburnDevicePrivate {
  gchar *name;
  gchar *addr;
  gchar *rev;
  gboolean details_known;

  gint buffer_size;
  gboolean dummy_write;

  gboolean cdr;
  gboolean cdrw;
  gboolean dvdr;
  gboolean dvdplusr;
  gboolean dvdram;
  gboolean bd;

  GSList *supported_speeds;

  gint tao_block_types;
  gint sao_block_types;
  gint raw_block_types;
  gint packet_block_types;

  enum burn_disc_status disc_status;
  int profile_no;
  char profile_name[80];
  int is_erasable;
  gboolean last_umount_failed;
};

struct _XfburnDevice
{
  GObject parent;
};

G_DEFINE_TYPE_WITH_PRIVATE (XfburnDevice, xfburn_device, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (xfburn_device_get_instance_private (o))

static void
xfburn_device_get_property (GObject *object, guint property_id,
                              GValue *value, GParamSpec *pspec)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (XFBURN_DEVICE (object));

  switch (property_id) {
    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;
    case PROP_ADDRESS:
      g_value_set_string (value, priv->addr);
      break;
    case PROP_REVISION:
      g_value_set_string (value, priv->rev);
      break;
    case PROP_SUPPORTED_SPEEDS:
      g_value_set_pointer (value, priv->supported_speeds);
      break;
    case PROP_DISC_STATUS:
      g_value_set_int (value, priv->disc_status);
      break;
    case PROP_PROFILE_NO:
      g_value_set_int (value, priv->profile_no);
      break;
    case PROP_PROFILE_NAME:
      g_value_set_string (value, priv->profile_name);
      break;
    case PROP_ERASABLE:
      g_value_set_boolean (value, priv->is_erasable);
      break;
    case PROP_CDR:
      g_value_set_boolean (value, priv->cdr);
      break;
    case PROP_CDRW:
      g_value_set_boolean (value, priv->cdrw);
      break;
    case PROP_DVDR:
      g_value_set_boolean (value, priv->dvdr);
      break;
    case PROP_DVDPLUSR:
      g_value_set_boolean (value, priv->dvdplusr);
      break;
    case PROP_DVDRAM:
      g_value_set_boolean (value, priv->dvdram);
      break;
    case PROP_BD:
      g_value_set_boolean (value, priv->bd);
      break;
    case PROP_TAO_BLOCK_TYPES:
      g_value_set_int (value, priv->tao_block_types);
      break;
    case PROP_SAO_BLOCK_TYPES:
      g_value_set_int (value, priv->sao_block_types);
      break;
    case PROP_RAW_BLOCK_TYPES:
      g_value_set_int (value, priv->raw_block_types);
      break;
    case PROP_PACKET_BLOCK_TYPES:
      g_value_set_int (value, priv->packet_block_types);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
xfburn_device_set_property (GObject *object, guint property_id,
                              const GValue *value, GParamSpec *pspec)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (XFBURN_DEVICE (object));

  switch (property_id) {
    case PROP_NAME:
      g_free (priv->name);
      priv->name = g_value_dup_string (value);
      break;
    case PROP_ADDRESS:
      g_free (priv->addr);
      priv->addr = g_value_dup_string (value);
      break;
    case PROP_REVISION:
      g_free (priv->rev);
      priv->rev = g_value_dup_string (value);
      break;
    case PROP_SUPPORTED_SPEEDS:
      priv->supported_speeds = g_value_get_pointer (value);
      break;
    case PROP_DISC_STATUS:
      priv->disc_status = g_value_get_int (value);
      break;
    case PROP_PROFILE_NO:
      priv->profile_no = g_value_get_int (value);
      break;
    case PROP_PROFILE_NAME:
      g_strlcpy (priv->profile_name, g_value_get_string(value), 80);
      break;
    case PROP_ERASABLE:
      priv->is_erasable = g_value_get_boolean (value);
      break;
    case PROP_CDR:
      priv->cdr = g_value_get_boolean (value);
      break;
    case PROP_CDRW:
      priv->cdrw = g_value_get_boolean (value);
      break;
    case PROP_DVDR:
      priv->dvdr = g_value_get_boolean (value);
      break;
    case PROP_DVDPLUSR:
      priv->dvdplusr = g_value_get_boolean (value);
      break;
    case PROP_DVDRAM:
      priv->dvdram = g_value_get_boolean (value);
      break;
    case PROP_BD:
      priv->bd = g_value_get_boolean (value);
      break;
    case PROP_TAO_BLOCK_TYPES:
      priv->tao_block_types = g_value_get_int (value);
      break;
    case PROP_SAO_BLOCK_TYPES:
      priv->sao_block_types = g_value_get_int (value);
      break;
    case PROP_RAW_BLOCK_TYPES:
      priv->raw_block_types = g_value_get_int (value);
      break;
    case PROP_PACKET_BLOCK_TYPES:
      priv->packet_block_types = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
xfburn_device_finalize (GObject *object)
{
  XfburnDevice *dev = XFBURN_DEVICE (object);
  XfburnDevicePrivate *priv = GET_PRIVATE (dev);

  g_free (priv->name);
  g_free (priv->addr);
  g_free (priv->rev);

  g_slist_free (priv->supported_speeds);

  G_OBJECT_CLASS (xfburn_device_parent_class)->finalize (object);
}

static void
xfburn_device_class_init (XfburnDeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xfburn_device_get_property;
  object_class->set_property = xfburn_device_set_property;
  object_class->finalize = xfburn_device_finalize;

  g_object_class_install_property (object_class, PROP_NAME,
                                   g_param_spec_string ("name", _("Display name"),
                                                        _("Display name"), NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ADDRESS,
                                   g_param_spec_string ("address", _("Device address"),
                                                        _("Device address"), "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_REVISION,
                                   g_param_spec_string ("revision", _("Device revision"),
                                                        _("Device Revision"), "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_SUPPORTED_SPEEDS,
                                   g_param_spec_pointer ("supported-speeds", _("Burn speeds supported by the device"),
                                                        _("Burn speeds supported by the device"), G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_DISC_STATUS,
                                   g_param_spec_int ("disc-status", _("Disc status"),
                                                     _("Disc status"), 0, 6, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_PROFILE_NO,
                                   g_param_spec_int ("profile-no", _("Profile no. as reported by libburn"),
                                                     _("Profile no. as reported by libburn"), 0, 0xffff, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_PROFILE_NAME,
                                   g_param_spec_string ("profile-name", _("Profile name as reported by libburn"),
                                                        _("Profile name as reported by libburn"), "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ERASABLE,
                                   g_param_spec_boolean ("erasable", _("Is the disc erasable"),
                                                        _("Is the disc erasable"), FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_CDR,
                                   g_param_spec_boolean ("cdr", _("Can burn CDR"),
                                                        _("Can burn CDR"), FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_CDRW,
                                   g_param_spec_boolean ("cdrw", _("Can burn CDRW"),
                                                        _("Can burn CDRW"), FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_DVDR,
                                   g_param_spec_boolean ("dvdr", _("Can burn DVDR"),
                                                        _("Can burn DVDR"), FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_DVDPLUSR,
                                   g_param_spec_boolean ("dvdplusr", _("Can burn DVDPLUSR"),
                                                        _("Can burn DVDPLUSR"), FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_DVDRAM,
                                   g_param_spec_boolean ("dvdram", _("Can burn DVDRAM"),
                                                        _("Can burn DVDRAM"), FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_BD,
                                   g_param_spec_boolean ("bd", _("Can burn Blu-ray"),
                                                        _("Can burn Blu-ray"), FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_TAO_BLOCK_TYPES,
                                   g_param_spec_int ("tao-block-types", _("libburn TAO block types"),
                                                     _("libburn TAO block types"), 0, G_MAXINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_SAO_BLOCK_TYPES,
                                   g_param_spec_int ("sao-block-types", _("libburn SAO block types"),
                                                     _("libburn SAO block types"), 0, G_MAXINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_RAW_BLOCK_TYPES,
                                   g_param_spec_int ("raw-block-types", _("libburn RAW block types"),
                                                     _("libburn RAW block types"), 0, G_MAXINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_PACKET_BLOCK_TYPES,
                                   g_param_spec_int ("packet-block-types", _("libburn PACKET block types"),
                                                     _("libburn PACKET block types"), 0, G_MAXINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

static void
xfburn_device_init (XfburnDevice *self)
{
  /* FIXME: initialize profile_name, or is that handled by the property? */
}


/***************/
/*- internals -*/
/***************/

static gboolean
no_speed_duplicate (GSList *speed_list, gint speed)
{
  GSList *el = speed_list;

  while (el) {
    gint el_speed = GPOINTER_TO_INT (el->data);

    if (el_speed == speed)
      return FALSE;

    el = g_slist_next (el);
  }

  return TRUE;
}

/* sort the speed list in ascending order */
static gint
cmp_ints (gconstpointer a, gconstpointer b)
{
  return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}

static gboolean
cancellable_sleep_ms (guint total_ms, GCancellable *cancellable)
{
  guint remaining = total_ms;

  while (remaining > 0) {
    guint step = MIN (remaining, 50U);

    if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
      return FALSE;
    }

    g_usleep ((gulong) step * 1000UL);
    remaining -= step;
  }

  return !(cancellable != NULL && g_cancellable_is_cancelled (cancellable));
}

static void
probe_disc_into_result (const gchar *addr,
                        const gchar *name,
                        struct burn_drive_info *drive_info,
                        GCancellable *cancellable,
                        XfburnDeviceProbeResult *res)
{
  int wait_ticks = 0;
  int prof_ret;
  gint64 t_call;

  /* check if there is a disc in the drive */
  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_get_status", "addr=\"%s\"", addr);
  while ((res->disc_status = burn_disc_get_status (drive_info->drive)) == BURN_DISC_UNREADY && wait_ticks < 50) {
    if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
      res->cancelled = TRUE;
      break;
    }
    g_usleep (100001);
    wait_ticks++;
  }
  XFBURN_LOG_CALL_EXIT ("burn_disc_get_status", t_call, "disc_status=%d wait_ticks=%d", res->disc_status, wait_ticks);

  if (res->cancelled) {
    return;
  }

  DBG ("disc_status = %d", res->disc_status);

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_get_profile", "addr=\"%s\"", addr);
  prof_ret = burn_disc_get_profile (drive_info->drive, &(res->profile_no), res->profile_name);
  XFBURN_LOG_CALL_EXIT ("burn_disc_get_profile", t_call, "ret=%d profile_no=0x%x profile_name=\"%s\"",
                        prof_ret, res->profile_no, res->profile_name);

  if (prof_ret != 1) {
    g_warning ("no profile could be retrieved");
  }
  res->is_erasable = burn_disc_erasable (drive_info->drive);
  xfburn_log_drain_backend_messages ("refresh_disc", NULL, 0);

  XFBURN_LOG_INFO ("MEDIA",
                   "addr=\"%s\" name=\"%s\" disc_status=%d profile_no=0x%x profile_name=\"%s\" erasable=%d wait_ticks=%d",
                   addr, name != NULL ? name : "", res->disc_status, res->profile_no, res->profile_name,
                   res->is_erasable, wait_ticks);
}

static void
probe_speed_list_into_result (const gchar *addr,
                              const gchar *name,
                              struct burn_drive_info *drive_info,
                              XfburnDeviceProbeResult *res)
{
  struct burn_speed_descriptor *speed_list = NULL;
  gint ret;
  gint atip_ret = 0;
  gint min_speed = 0;
  gint max_speed = 0;
  gint64 t_call;

  /* On CD-R / CD-RW media, read ATIP first so libburn populates ATIP min/max write speeds */
  if (res->profile_no == XFBURN_PROFILE_CDR || res->profile_no == XFBURN_PROFILE_CDRW) {
    t_call = g_get_monotonic_time ();
    XFBURN_LOG_CALL_ENTER ("burn_disc_read_atip", "addr=\"%s\" profile_no=0x%x", addr, res->profile_no);
    atip_ret = burn_disc_read_atip (drive_info->drive);
    XFBURN_LOG_CALL_EXIT ("burn_disc_read_atip", t_call, "ret=%d", atip_ret);
    xfburn_log_drain_backend_messages ("refresh_speed_list_atip", NULL, 0);
  }

  /* Query MMC speed descriptors (ACh GET PERFORMANCE / Mode Page 2Ah) */
  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_drive_get_speedlist", "addr=\"%s\"", addr);
  ret = burn_drive_get_speedlist (drive_info->drive, &speed_list);
  XFBURN_LOG_CALL_EXIT ("burn_drive_get_speedlist", t_call, "ret=%d speed_list=%p", ret, (void *) speed_list);
  xfburn_log_drain_backend_messages ("refresh_speed_list", NULL, 0);

  if (ret > 0 && speed_list != NULL) {
    struct burn_speed_descriptor *el = speed_list;
    GString *raw_desc = g_string_new (NULL);

    while (el != NULL) {
      gint speed = el->write_speed;

      if (raw_desc->len > 0) {
        g_string_append (raw_desc, "; ");
      }
      g_string_append_printf (raw_desc, "src=%d w=%d r=%d end_lba=%d",
                              el->source, el->write_speed, el->read_speed, el->end_lba);

      if (speed > 0 && no_speed_duplicate (res->supported_speeds, speed)) {
        res->supported_speeds = g_slist_prepend (res->supported_speeds, GINT_TO_POINTER (speed));
      }

      el = el->next;
    }

    XFBURN_LOG_DEBUG ("MEDIA", "addr=\"%s\" raw_speed_descriptors=\"%s\"", addr, raw_desc->str);
    g_string_free (raw_desc, TRUE);
    burn_drive_free_speedlist (&speed_list);
  } else if (ret < 0) {
    /* ret < 0: Log diagnostic details and continue gracefully without crashing via g_error */
    XFBURN_LOG_ERROR ("MEDIA",
                      "severe error while retrieving speed list for addr=\"%s\" name=\"%s\" (ret=%d)",
                      addr, name != NULL ? name : "", ret);
    xfburn_log_drain_backend_messages ("refresh_speed_list_error", NULL, 0);
    xfburn_log_dump_device_hardware (addr, NULL);
    g_warning ("severe error while retrieving speed list for '%s' (ret=%d)", addr, ret);
  }

  /* Supplement with libburn's min/max write speeds (populated from ATIP and drive capabilities) */
  min_speed = burn_drive_get_min_write_speed (drive_info->drive);
  max_speed = burn_drive_get_write_speed (drive_info->drive);

  if (min_speed > 0 && no_speed_duplicate (res->supported_speeds, min_speed)) {
    res->supported_speeds = g_slist_prepend (res->supported_speeds, GINT_TO_POINTER (min_speed));
  }
  if (max_speed > 0 && no_speed_duplicate (res->supported_speeds, max_speed)) {
    res->supported_speeds = g_slist_prepend (res->supported_speeds, GINT_TO_POINTER (max_speed));
  }

  if (res->supported_speeds != NULL) {
    GString *speeds_str = g_string_new (NULL);
    GSList *it;

    res->supported_speeds = g_slist_sort (res->supported_speeds, &cmp_ints);

    for (it = res->supported_speeds; it != NULL; it = it->next) {
      if (speeds_str->len > 0) {
        g_string_append_c (speeds_str, ',');
      }
      g_string_append_printf (speeds_str, "%d", GPOINTER_TO_INT (it->data));
    }
    XFBURN_LOG_INFO ("MEDIA",
                     "addr=\"%s\" atip_ret=%d min_write_speed_kbps=%d max_write_speed_kbps=%d supported_speeds_kbps=\"%s\"",
                     addr, atip_ret, min_speed, max_speed, speeds_str->str);
    g_string_free (speeds_str, TRUE);
  } else if (ret >= 0) {
    XFBURN_LOG_WARN ("MEDIA",
                     "addr=\"%s\" reported speed list is empty (ret=%d atip_ret=%d min=%d max=%d)",
                     addr, ret, atip_ret, min_speed, max_speed);
    g_warning ("reported speed list is empty for device '%s'", name != NULL ? name : addr);
  }
}

static GMutex drive_grab_mutex;
static GCond drive_grab_cond;
static gboolean drive_grabbed = FALSE;
static gboolean drive_grabbed_by_burn = FALSE;
static GCancellable *active_probe_cancellable = NULL;
static gint active_drive_grabs = 0;
static gint active_burn_grabs = 0;

static gboolean
acquire_grab_slot (gboolean is_probe, GCancellable *probe_cancellable)
{
  g_mutex_lock (&drive_grab_mutex);

  if (is_probe) {
    if (drive_grabbed && !drive_grabbed_by_burn) {
      gint64 deadline = g_get_monotonic_time () + 8 * G_USEC_PER_SEC;

      while (drive_grabbed && !drive_grabbed_by_burn) {
        gint64 step_end;

        if (probe_cancellable != NULL && g_cancellable_is_cancelled (probe_cancellable)) {
          g_mutex_unlock (&drive_grab_mutex);
          return FALSE;
        }
        step_end = MIN (deadline, g_get_monotonic_time () + 200 * 1000);
        g_cond_wait_until (&drive_grab_cond, &drive_grab_mutex, step_end);
        if (g_get_monotonic_time () >= deadline) {
          break;
        }
      }
    }

    if (drive_grabbed || (probe_cancellable != NULL && g_cancellable_is_cancelled (probe_cancellable))) {
      g_mutex_unlock (&drive_grab_mutex);
      return FALSE;
    }
    drive_grabbed = TRUE;
    drive_grabbed_by_burn = FALSE;
    g_clear_object (&active_probe_cancellable);
    if (probe_cancellable != NULL) {
      active_probe_cancellable = g_object_ref (probe_cancellable);
    }
    g_atomic_int_set (&active_drive_grabs, 1);
    g_atomic_int_set (&active_burn_grabs, 0);
    g_mutex_unlock (&drive_grab_mutex);
    return TRUE;
  }

  /* Burn/blank job: cancel any active UI probe and wait briefly for it to release the drive */
  if (drive_grabbed && !drive_grabbed_by_burn) {
    gint64 deadline = g_get_monotonic_time () + 12 * G_USEC_PER_SEC;

    if (active_probe_cancellable != NULL) {
      GCancellable *to_cancel = g_object_ref (active_probe_cancellable);
      g_mutex_unlock (&drive_grab_mutex);
      XFBURN_LOG_INFO ("DEV", "Cancelling in-flight device probe so burn/blank job can acquire drive");
      g_cancellable_cancel (to_cancel);
      g_object_unref (to_cancel);
      g_mutex_lock (&drive_grab_mutex);
    }

    while (drive_grabbed && !drive_grabbed_by_burn) {
      if (!g_cond_wait_until (&drive_grab_cond, &drive_grab_mutex, deadline)) {
        XFBURN_LOG_WARN ("DEV", "Timed out waiting for in-flight probe to release drive");
        break;
      }
    }
  }

  if (drive_grabbed) {
    g_mutex_unlock (&drive_grab_mutex);
    return FALSE;
  }

  drive_grabbed = TRUE;
  drive_grabbed_by_burn = TRUE;
  g_clear_object (&active_probe_cancellable);
  g_atomic_int_set (&active_drive_grabs, 1);
  g_atomic_int_set (&active_burn_grabs, 1);
  g_mutex_unlock (&drive_grab_mutex);
  return TRUE;
}

static void
release_grab_slot (void)
{
  g_mutex_lock (&drive_grab_mutex);
  drive_grabbed = FALSE;
  drive_grabbed_by_burn = FALSE;
  g_clear_object (&active_probe_cancellable);
  g_atomic_int_set (&active_drive_grabs, 0);
  g_atomic_int_set (&active_burn_grabs, 0);
  g_cond_broadcast (&drive_grab_cond);
  g_mutex_unlock (&drive_grab_mutex);
}

#ifdef HAVE_GUDEV
static XfburnUmountResult
try_unmount_for_grab (XfburnUdevManager *udevman,
                      const gchar *addr,
                      GCancellable *cancellable,
                      gchar **error_msg)
{
  XfburnUmountResult res;

  if (udevman == NULL || addr == NULL || addr[0] == '\0') {
    return XFBURN_UMOUNT_NOT_MOUNTED;
  }

  res = xfburn_udev_manager_unmount_device (udevman, addr, cancellable, error_msg);
  if (res != XFBURN_UMOUNT_NOT_MOUNTED) {
    return res;
  }

  /* If the kernel mount table already shows this device mounted, GVFS/GVolumeMonitor
   * may still be delivering the D-Bus mount-added event. Give it a brief cancellable
   * window before attempting an exclusive libburn open (which would otherwise fail
   * after 6 seconds with EBUSY). */
  if (xfburn_udev_manager_is_mounted_in_kernel (addr)) {
    gint wait_attempt;

    XFBURN_LOG_INFO ("DEV",
                     "Device addr=\"%s\" is mounted in /proc/self/mounts but not yet in GVolumeMonitor; waiting briefly for mount sync",
                     addr);
    for (wait_attempt = 0; wait_attempt < 3; wait_attempt++) {
      if (!cancellable_sleep_ms (200, cancellable)) {
        return XFBURN_UMOUNT_CANCELLED;
      }
      res = xfburn_udev_manager_unmount_device (udevman, addr, cancellable, error_msg);
      if (res != XFBURN_UMOUNT_NOT_MOUNTED) {
        return res;
      }
      if (!xfburn_udev_manager_is_mounted_in_kernel (addr)) {
        break;
      }
    }
  }

  return XFBURN_UMOUNT_NOT_MOUNTED;
}
#endif

static gboolean
grab_drive_internal (const gchar *addr,
                     struct burn_drive_info **drive_info,
                     gboolean is_probe,
                     GCancellable *cancellable,
                     gboolean *out_cancelled,
                     gboolean *out_umount_failed,
                     gchar **out_error_msg)
{
  int ret = 0;
  gchar drive_addr[BURN_DRIVE_ADR_LEN];
  gchar *addr_mut;
  int i;
  const int max_checks = is_probe ? 2 : 3;
  gint64 t_call;
#ifdef HAVE_GUDEV
  XfburnUdevManager *udevman = NULL;
  XfburnUmountResult umount_res;
#endif

  if (out_cancelled != NULL) {
    *out_cancelled = FALSE;
  }
  if (out_umount_failed != NULL) {
    *out_umount_failed = FALSE;
  }
  if (out_error_msg != NULL) {
    *out_error_msg = NULL;
  }

  if (addr == NULL || addr[0] == '\0') {
    return FALSE;
  }

  if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
    if (out_cancelled != NULL) {
      *out_cancelled = TRUE;
    }
    return FALSE;
  }

  if (!acquire_grab_slot (is_probe, cancellable)) {
    if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
      if (out_cancelled != NULL) {
        *out_cancelled = TRUE;
      }
      return FALSE;
    }
    DBG ("Drive already grabbed, skipping concurrent grab attempt for '%s'", addr);
    XFBURN_LOG_DEBUG ("DEV", "Drive already grabbed, skipping concurrent grab attempt for addr=\"%s\" is_probe=%d",
                      addr, is_probe);
    return FALSE;
  }

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_drive_convert_fs_adr", "addr=\"%s\"", addr);
  addr_mut = g_strdup (addr);
  ret = burn_drive_convert_fs_adr (addr_mut, drive_addr);
  g_free (addr_mut);
  XFBURN_LOG_CALL_EXIT ("burn_drive_convert_fs_adr", t_call, "ret=%d drive_addr=\"%s\"", ret, drive_addr);
  xfburn_log_drain_backend_messages ("convert_fs_adr", NULL, 0);

  if (ret <= 0) {
    release_grab_slot ();
    XFBURN_LOG_ERROR ("DEV", "Device address does not lead to a burner addr=\"%s\" (ret=%d)", addr, ret);
    xfburn_log_dump_device_hardware (addr, NULL);
    g_warning ("Device address does not lead to a burner '%s' (ret=%d).", addr, ret);
    return FALSE;
  }

#ifdef HAVE_GUDEV
  udevman = xfburn_udev_manager_ref_global ();

  /* Unmount any active filesystem on the optical drive BEFORE calling libburn,
   * avoiding a guaranteed 6-second EBUSY wait inside burn_drive_scan_and_grab(). */
  umount_res = try_unmount_for_grab (udevman, addr, cancellable, out_error_msg);
  if (umount_res == XFBURN_UMOUNT_CANCELLED) {
    if (out_cancelled != NULL) {
      *out_cancelled = TRUE;
    }
    g_clear_object (&udevman);
    release_grab_slot ();
    return FALSE;
  }
  if (umount_res == XFBURN_UMOUNT_FAILED) {
    if (out_umount_failed != NULL) {
      *out_umount_failed = TRUE;
    }
    XFBURN_LOG_WARN ("DEV", "Aborting grab for addr=\"%s\" because volume unmount failed", addr);
    g_clear_object (&udevman);
    release_grab_slot ();
    return FALSE;
  }
#endif

  /* Try to grab the drive; retries handle transient drive spin-up or OS automount races. */
  for (i = 1; i <= max_checks; i++) {
    if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
      if (out_cancelled != NULL) {
        *out_cancelled = TRUE;
      }
      ret = 0;
      break;
    }

    t_call = g_get_monotonic_time ();
    XFBURN_LOG_CALL_ENTER ("burn_drive_scan_and_grab", "drive_addr=\"%s\" attempt=%d/%d", drive_addr, i, max_checks);
    ret = burn_drive_scan_and_grab (drive_info, drive_addr, 0);
    XFBURN_LOG_CALL_EXIT ("burn_drive_scan_and_grab", t_call, "ret=%d attempt=%d/%d", ret, i, max_checks);
    xfburn_log_drain_backend_messages ("scan_and_grab", NULL, 0);

    if (ret > 0) {
      break;
    }

    if (i < max_checks) {
      if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
        if (out_cancelled != NULL) {
          *out_cancelled = TRUE;
        }
        break;
      }

#ifdef HAVE_GUDEV
      if (out_error_msg != NULL) {
        g_clear_pointer (out_error_msg, g_free);
      }
      umount_res = try_unmount_for_grab (udevman, addr, cancellable, out_error_msg);
      if (umount_res == XFBURN_UMOUNT_CANCELLED) {
        if (out_cancelled != NULL) {
          *out_cancelled = TRUE;
        }
        break;
      }
      if (umount_res == XFBURN_UMOUNT_FAILED) {
        if (out_umount_failed != NULL) {
          *out_umount_failed = TRUE;
        }
        XFBURN_LOG_WARN ("DEV", "Stopping grab retries for addr=\"%s\" because volume unmount failed", addr);
        break;
      }
      if (umount_res == XFBURN_UMOUNT_NOT_MOUNTED) {
        if (!cancellable_sleep_ms ((guint) (i * 150), cancellable)) {
          if (out_cancelled != NULL) {
            *out_cancelled = TRUE;
          }
          break;
        }
      }
#else
      if (!cancellable_sleep_ms ((guint) (i * 150), cancellable)) {
        if (out_cancelled != NULL) {
          *out_cancelled = TRUE;
        }
        break;
      }
#endif
    }
  }

#ifdef HAVE_GUDEV
  g_clear_object (&udevman);
#endif

  if (ret <= 0) {
    release_grab_slot ();
    if (out_cancelled == NULL || !(*out_cancelled)) {
      XFBURN_LOG_ERROR ("DEV", "Unable to grab drive at addr=\"%s\" after %d attempts (ret=%d)",
                        addr, max_checks, ret);
      xfburn_log_dump_device_hardware (addr, NULL);
      g_warning ("Unable to grab the drive at path '%s' (ret=%d).", addr, ret);
    }
    return FALSE;
  }

  return TRUE;
}

/*******************/
/*- public methods-*/
/*******************/

void
xfburn_device_probe_result_clear (XfburnDeviceProbeResult *result)
{
  if (result == NULL) {
    return;
  }

  g_clear_pointer (&result->addr, g_free);
  g_clear_pointer (&result->error_msg, g_free);
  g_clear_slist (&result->supported_speeds, NULL);
}

gboolean
xfburn_device_probe_addr (const gchar *addr,
                          const gchar *name,
                          gboolean get_speed_info,
                          gboolean details_known,
                          GCancellable *cancellable,
                          XfburnDeviceProbeResult *out_result)
{
  struct burn_drive_info *drive_info = NULL;

  g_return_val_if_fail (out_result != NULL, FALSE);

  memset (out_result, 0, sizeof (*out_result));
  out_result->addr = g_strdup (addr);
  out_result->disc_status = BURN_DISC_UNGRABBED;

  if (addr == NULL || addr[0] == '\0') {
    return FALSE;
  }

  if (!grab_drive_internal (addr, &drive_info, TRUE, cancellable,
                            &out_result->cancelled, &out_result->umount_failed,
                            &out_result->error_msg)) {
    if (!out_result->cancelled) {
      g_warning ("Couldn't grab drive in order to update speed list.");
    }
    out_result->disc_status = BURN_DISC_UNGRABBED;
    return FALSE;
  }

  if (!details_known) {
    out_result->has_libburn_info = TRUE;
    out_result->cdr = drive_info->write_cdr;
    out_result->cdrw = drive_info->write_cdrw;
    out_result->dvdr = drive_info->write_dvdr;
    out_result->dvdram = drive_info->write_dvdram;
    out_result->buffer_size = drive_info->buffer_size;
    out_result->dummy_write = drive_info->write_simulate;
    out_result->tao_block_types = drive_info->tao_block_types;
    out_result->sao_block_types = drive_info->sao_block_types;
    out_result->raw_block_types = drive_info->raw_block_types;
    out_result->packet_block_types = drive_info->packet_block_types;
  }

  probe_disc_into_result (addr, name, drive_info, cancellable, out_result);
  if (!out_result->cancelled && get_speed_info) {
    probe_speed_list_into_result (addr, name, drive_info, out_result);
  }

  xfburn_device_release_probe (drive_info);

  if (out_result->cancelled || (cancellable != NULL && g_cancellable_is_cancelled (cancellable))) {
    out_result->cancelled = TRUE;
    return FALSE;
  }

  out_result->grabbed = TRUE;
  return TRUE;
}

gboolean
xfburn_device_grab_addr_for_probe (const gchar *addr,
                                   GCancellable *cancellable,
                                   struct burn_drive_info **drive_info,
                                   gboolean *out_cancelled,
                                   gboolean *out_umount_failed,
                                   gchar **out_error_msg)
{
  return grab_drive_internal (addr,
                              drive_info,
                              TRUE,
                              cancellable,
                              out_cancelled,
                              out_umount_failed,
                              out_error_msg);
}

void
xfburn_device_release_probe (struct burn_drive_info *drive_info)
{
  if (drive_info == NULL) {
    return;
  }

#ifdef HAVE_GUDEV
  xfburn_udev_manager_note_probe_release ();
#endif
  xfburn_device_release (drive_info, 0);
#ifdef HAVE_GUDEV
  xfburn_udev_manager_note_probe_release ();
#endif
}

void
xfburn_device_wait_for_probes (void)
{
  gint64 deadline = g_get_monotonic_time () + 15 * G_USEC_PER_SEC;

  g_mutex_lock (&drive_grab_mutex);
  if (drive_grabbed && !drive_grabbed_by_burn && active_probe_cancellable != NULL) {
    GCancellable *to_cancel = g_object_ref (active_probe_cancellable);
    g_mutex_unlock (&drive_grab_mutex);
    g_cancellable_cancel (to_cancel);
    g_object_unref (to_cancel);
    g_mutex_lock (&drive_grab_mutex);
  }

  while (drive_grabbed && !drive_grabbed_by_burn) {
    if (!g_cond_wait_until (&drive_grab_cond, &drive_grab_mutex, deadline)) {
      XFBURN_LOG_WARN ("DEV", "Timed out waiting for background probes to finish during shutdown");
      break;
    }
  }
  g_mutex_unlock (&drive_grab_mutex);
}

void
xfburn_device_apply_probe_result (XfburnDevice *device,
                                  XfburnDeviceProbeResult *result)
{
  XfburnDevicePrivate *priv;

  g_return_if_fail (XFBURN_IS_DEVICE (device));
  g_return_if_fail (result != NULL);

  priv = GET_PRIVATE (device);

  priv->last_umount_failed = result->umount_failed;

  if (!result->grabbed) {
    priv->profile_no = 0;
    *(priv->profile_name) = '\0';
    priv->is_erasable = 0;
    g_clear_slist (&priv->supported_speeds, NULL);
    priv->disc_status = BURN_DISC_UNGRABBED;
    return;
  }

  if (result->has_libburn_info) {
    priv->details_known = TRUE;
    priv->cdr = result->cdr;
    priv->cdrw = result->cdrw;
    priv->dvdr = result->dvdr;
    priv->dvdram = result->dvdram;
    priv->buffer_size = result->buffer_size;
    priv->dummy_write = result->dummy_write;
    priv->tao_block_types = result->tao_block_types;
    priv->sao_block_types = result->sao_block_types;
    priv->raw_block_types = result->raw_block_types;
    priv->packet_block_types = result->packet_block_types;
  }

  priv->disc_status = result->disc_status;
  priv->profile_no = result->profile_no;
  g_strlcpy (priv->profile_name, result->profile_name, sizeof (priv->profile_name));
  priv->is_erasable = result->is_erasable;

  g_clear_slist (&priv->supported_speeds, NULL);
  priv->supported_speeds = result->supported_speeds;
  result->supported_speeds = NULL;
}

void
xfburn_device_fillin_libburn_info (XfburnDevice *device, struct burn_drive_info *drive)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);

  priv->details_known = TRUE;

  priv->cdr = drive->write_cdr;
  priv->cdrw = drive->write_cdrw;

  priv->dvdr = drive->write_dvdr;
  priv->dvdram = drive->write_dvdram;

  priv->buffer_size = drive->buffer_size;
  priv->dummy_write = drive->write_simulate;

  DBG ("libburn will determine BD support based on the disk in the drive");

  /* write modes */
  priv->tao_block_types = drive->tao_block_types;
  priv->sao_block_types = drive->sao_block_types;
  priv->raw_block_types = drive->raw_block_types;
  priv->packet_block_types = drive->packet_block_types;

  DBG (DEVICE_INFO_PRINTF (priv));
}

gboolean
xfburn_device_is_busy (void)
{
  return g_atomic_int_get (&active_drive_grabs) > 0;
}

gboolean
xfburn_device_is_burning (void)
{
  return g_atomic_int_get (&active_burn_grabs) > 0;
}

gboolean
xfburn_device_details_known (XfburnDevice *device)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);

  return priv->details_known;
}

gboolean
xfburn_device_last_umount_failed (XfburnDevice *device)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);

  return priv->last_umount_failed;
}

gboolean
xfburn_device_grab (XfburnDevice * device, struct burn_drive_info **drive_info)
{
  XfburnDevicePrivate *priv;
  gchar *addr_copy;
  gboolean ret;
  gchar *err_msg = NULL;

  g_return_val_if_fail (XFBURN_IS_DEVICE (device), FALSE);

  priv = GET_PRIVATE (device);
  addr_copy = g_strdup (priv->addr);

  ret = grab_drive_internal (addr_copy, drive_info, FALSE, NULL, NULL, NULL, &err_msg);

  g_free (err_msg);
  g_free (addr_copy);
  return ret;
}

gboolean
xfburn_device_refresh_info (XfburnDevice * device, gboolean get_speed_info)
{
  XfburnDevicePrivate *priv;
  XfburnDeviceProbeResult result;
  gboolean ret;

  if (G_UNLIKELY (device == NULL)) {
    g_warning ("Hmm, why can we refresh when there is no drive?");
    return FALSE;
  }

  if (xfburn_device_is_busy ()) {
    DBG ("Skipping device refresh while drive is busy");
    return FALSE;
  }

  priv = GET_PRIVATE (device);
  ret = xfburn_device_probe_addr (priv->addr, priv->name, get_speed_info, priv->details_known, NULL, &result);
  xfburn_device_apply_probe_result (device, &result);
  xfburn_device_probe_result_clear (&result);

  return ret;
}

gboolean
xfburn_device_release (struct burn_drive_info *drive_info, gboolean eject)
{
  int ret;
  gint64 t_call;

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_drive_release", "eject=%d", eject);
  burn_drive_snooze (drive_info->drive, 0);
  burn_drive_release (drive_info->drive, eject);
  ret = burn_drive_info_forget (drive_info, 0);
  XFBURN_LOG_CALL_EXIT ("burn_drive_release", t_call, "forget_ret=%d eject=%d", ret, eject);
  xfburn_log_drain_backend_messages ("device_release", NULL, 0);

  release_grab_slot ();

  if (G_LIKELY (ret == 1))
    return TRUE;
  else if (ret == 2) {
    DBG ("drive_info already forgotten");
    return TRUE;
  } else if (ret == 0) {
    DBG ("could not forget drive_info");
    return FALSE;
  } else if (ret < 0) {
    DBG ("some other failure while forgetting drive_info");
    return FALSE;
  }

  /* we should never reach this */
  return FALSE;
}

const gchar *
xfburn_device_set_name (XfburnDevice *device, const gchar *vendor, const gchar *product)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);

  g_free (priv->name);
  priv->name = g_strconcat (vendor, " ", product, NULL);

  return priv->name;
}

gboolean
xfburn_device_can_burn (XfburnDevice *device)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);

  return CAN_BURN (priv);
}

gboolean
xfburn_device_can_dummy_write (XfburnDevice *device)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);

  return priv->dummy_write;
}

XfburnDevice*
xfburn_device_new (void)
{
  return g_object_new (XFBURN_TYPE_DEVICE, NULL);
}


