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
      priv->name = g_value_dup_string (value);
      break;
    case PROP_ADDRESS:
      priv->addr = g_value_dup_string (value);
      break;
    case PROP_REVISION:
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

static void
refresh_disc (XfburnDevice * device, struct burn_drive_info *drive_info)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);
  int wait_ticks = 0;
  int prof_ret;
  gint64 t_call;

  /* check if there is a disc in the drive */
  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_get_status", "addr=\"%s\"", priv->addr);
  while ((priv->disc_status = burn_disc_get_status (drive_info->drive)) == BURN_DISC_UNREADY && wait_ticks < 50) {
    usleep(100001);
    wait_ticks++;
  }
  XFBURN_LOG_CALL_EXIT ("burn_disc_get_status", t_call, "disc_status=%d wait_ticks=%d", priv->disc_status, wait_ticks);

  DBG ("disc_status = %d", priv->disc_status);

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_disc_get_profile", "addr=\"%s\"", priv->addr);
  prof_ret = burn_disc_get_profile (drive_info->drive, &(priv->profile_no), priv->profile_name);
  XFBURN_LOG_CALL_EXIT ("burn_disc_get_profile", t_call, "ret=%d profile_no=0x%x profile_name=\"%s\"",
                        prof_ret, priv->profile_no, priv->profile_name);

  if (prof_ret != 1) {
    g_warning ("no profile could be retrieved");
  }
  priv->is_erasable = burn_disc_erasable (drive_info->drive);
  xfburn_log_drain_backend_messages ("refresh_disc", NULL, 0);

  XFBURN_LOG_INFO ("MEDIA",
                   "addr=\"%s\" name=\"%s\" disc_status=%d profile_no=0x%x profile_name=\"%s\" erasable=%d wait_ticks=%d",
                   priv->addr, priv->name, priv->disc_status, priv->profile_no, priv->profile_name,
                   priv->is_erasable, wait_ticks);
}

static void
refresh_speed_list (XfburnDevice * device, struct burn_drive_info *drive_info)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);

  struct burn_speed_descriptor *speed_list = NULL;
  gint ret;
  gint atip_ret = 0;
  gint min_speed = 0;
  gint max_speed = 0;
  gint64 t_call;

  /* On CD-R / CD-RW media, read ATIP first so libburn populates ATIP min/max write speeds */
  if (priv->profile_no == XFBURN_PROFILE_CDR || priv->profile_no == XFBURN_PROFILE_CDRW) {
    t_call = g_get_monotonic_time ();
    XFBURN_LOG_CALL_ENTER ("burn_disc_read_atip", "addr=\"%s\" profile_no=0x%x", priv->addr, priv->profile_no);
    atip_ret = burn_disc_read_atip (drive_info->drive);
    XFBURN_LOG_CALL_EXIT ("burn_disc_read_atip", t_call, "ret=%d", atip_ret);
    xfburn_log_drain_backend_messages ("refresh_speed_list_atip", NULL, 0);
  }

  /* Query MMC speed descriptors (ACh GET PERFORMANCE / Mode Page 2Ah) */
  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_drive_get_speedlist", "addr=\"%s\"", priv->addr);
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

      if (speed > 0 && no_speed_duplicate (priv->supported_speeds, speed)) {
        priv->supported_speeds = g_slist_prepend (priv->supported_speeds, GINT_TO_POINTER (speed));
      }

      el = el->next;
    }

    XFBURN_LOG_DEBUG ("MEDIA", "addr=\"%s\" raw_speed_descriptors=\"%s\"", priv->addr, raw_desc->str);
    g_string_free (raw_desc, TRUE);
    burn_drive_free_speedlist (&speed_list);
  } else if (ret < 0) {
    /* ret < 0: Log diagnostic details and continue gracefully without crashing via g_error */
    XFBURN_LOG_ERROR ("MEDIA",
                      "severe error while retrieving speed list for addr=\"%s\" name=\"%s\" (ret=%d)",
                      priv->addr, priv->name, ret);
    xfburn_log_drain_backend_messages ("refresh_speed_list_error", NULL, 0);
    xfburn_log_dump_device_hardware (priv->addr, NULL);
    g_warning ("severe error while retrieving speed list for '%s' (ret=%d)", priv->addr, ret);
  }

  /* Supplement with libburn's min/max write speeds (populated from ATIP and drive capabilities) */
  min_speed = burn_drive_get_min_write_speed (drive_info->drive);
  max_speed = burn_drive_get_write_speed (drive_info->drive);

  if (min_speed > 0 && no_speed_duplicate (priv->supported_speeds, min_speed)) {
    priv->supported_speeds = g_slist_prepend (priv->supported_speeds, GINT_TO_POINTER (min_speed));
  }
  if (max_speed > 0 && no_speed_duplicate (priv->supported_speeds, max_speed)) {
    priv->supported_speeds = g_slist_prepend (priv->supported_speeds, GINT_TO_POINTER (max_speed));
  }

  if (priv->supported_speeds != NULL) {
    GString *speeds_str = g_string_new (NULL);
    GSList *it;

    priv->supported_speeds = g_slist_sort (priv->supported_speeds, &cmp_ints);

    for (it = priv->supported_speeds; it != NULL; it = it->next) {
      if (speeds_str->len > 0) {
        g_string_append_c (speeds_str, ',');
      }
      g_string_append_printf (speeds_str, "%d", GPOINTER_TO_INT (it->data));
    }
    XFBURN_LOG_INFO ("MEDIA",
                     "addr=\"%s\" atip_ret=%d min_write_speed_kbps=%d max_write_speed_kbps=%d supported_speeds_kbps=\"%s\"",
                     priv->addr, atip_ret, min_speed, max_speed, speeds_str->str);
    g_string_free (speeds_str, TRUE);
  } else if (ret >= 0) {
    XFBURN_LOG_WARN ("MEDIA",
                     "addr=\"%s\" reported speed list is empty (ret=%d atip_ret=%d min=%d max=%d)",
                     priv->addr, ret, atip_ret, min_speed, max_speed);
    g_warning ("reported speed list is empty for device:");
    g_warning ("%s can burn: %d [cdr: %d, cdrw: %d, dvdr: %d, dvdram: %d]", (priv)->name, ((priv)->cdr || (priv)->cdrw || (priv)->dvdr || (priv)->dvdram), (priv)->cdr, (priv)->cdrw, (priv)->dvdr, (priv)->dvdram);
  }
}

/*******************/
/*- public methods-*/
/*******************/

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

static gint active_drive_grabs = 0;

gboolean
xfburn_device_is_busy (void)
{
  return g_atomic_int_get (&active_drive_grabs) > 0;
}

gboolean
xfburn_device_grab (XfburnDevice * device, struct burn_drive_info **drive_info)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);
  int ret = 0;
  gchar drive_addr[BURN_DRIVE_ADR_LEN];
  int i;
  const int max_checks = 4;
  gint64 t_call;
#ifdef HAVE_GUDEV
  XfburnUdevManager *udevman = xfburn_udev_manager_get_global ();
#endif

  if (g_atomic_int_get (&active_drive_grabs) > 0) {
    DBG ("Drive already grabbed, skipping concurrent grab attempt for '%s'", priv->addr);
    XFBURN_LOG_DEBUG ("DEV", "Drive already grabbed, skipping concurrent grab attempt for addr=\"%s\"", priv->addr);
    return FALSE;
  }

  t_call = g_get_monotonic_time ();
  XFBURN_LOG_CALL_ENTER ("burn_drive_convert_fs_adr", "addr=\"%s\"", priv->addr);
  ret = burn_drive_convert_fs_adr (priv->addr, drive_addr);
  XFBURN_LOG_CALL_EXIT ("burn_drive_convert_fs_adr", t_call, "ret=%d drive_addr=\"%s\"", ret, drive_addr);
  xfburn_log_drain_backend_messages ("convert_fs_adr", NULL, 0);

  if (ret <= 0) {
    XFBURN_LOG_ERROR ("DEV", "Device address does not lead to a burner addr=\"%s\" (ret=%d)", priv->addr, ret);
    xfburn_log_dump_device_hardware (priv->addr, NULL);
    g_warning ("Device address does not lead to a burner '%s' (ret=%d).", priv->addr, ret);
    return FALSE;
  }

  g_atomic_int_inc (&active_drive_grabs);

  /* we need to try to grab several times, because
   * the drive might be busy detecting the disc */
  for (i = 1; i <= max_checks; i++) {
    t_call = g_get_monotonic_time ();
    XFBURN_LOG_CALL_ENTER ("burn_drive_scan_and_grab", "drive_addr=\"%s\" attempt=%d/%d", drive_addr, i, max_checks);
    ret = burn_drive_scan_and_grab (drive_info, drive_addr, 0);
    XFBURN_LOG_CALL_EXIT ("burn_drive_scan_and_grab", t_call, "ret=%d attempt=%d/%d", ret, i, max_checks);
    xfburn_log_drain_backend_messages ("scan_and_grab", NULL, 0);

    if (ret > 0) {
      break;
    } else if (i < max_checks) {
#ifdef HAVE_GUDEV
      if (!g_main_context_is_owner (g_main_context_default ()) ||
          !xfburn_udev_manager_check_ask_umount (udevman, device)) {
        usleep (i * 100001);
      }
#else
      usleep (i * 100001);
#endif
    }
  }

  if (ret <= 0) {
    g_atomic_int_add (&active_drive_grabs, -1);
    XFBURN_LOG_ERROR ("DEV", "Unable to grab drive at addr=\"%s\" after %d attempts (ret=%d)",
                      priv->addr, max_checks, ret);
    xfburn_log_dump_device_hardware (priv->addr, NULL);
    g_warning ("Unable to grab the drive at path '%s' (ret=%d).", priv->addr, ret);
    return FALSE;
  }

  return TRUE;
}

gboolean
xfburn_device_refresh_info (XfburnDevice * device, gboolean get_speed_info)
{
  XfburnDevicePrivate *priv = GET_PRIVATE (device);

  struct burn_drive_info *drive_info = NULL;
  gboolean ret;

  if (G_UNLIKELY (device == NULL)) {
    g_warning ("Hmm, why can we refresh when there is no drive?");
    return FALSE;
  }

  if (xfburn_device_is_busy ()) {
    DBG ("Skipping device refresh while drive is busy");
    return FALSE;
  }

  /* reset other internal structures */
  priv->profile_no = 0;
  *(priv->profile_name) = '\0';
  priv->is_erasable = 0;

  /* empty previous speed list */
  g_clear_slist (&priv->supported_speeds, NULL);

  if (!xfburn_device_grab (device, &drive_info)) {
    ret = FALSE;
    g_warning ("Couldn't grab drive in order to update speed list.");
    priv->disc_status = BURN_DISC_UNGRABBED;
  } else {
    if (!priv->details_known)
      xfburn_device_fillin_libburn_info (device, drive_info);

    ret = TRUE;
    refresh_disc (device, drive_info);
    if (get_speed_info)
      refresh_speed_list (device, drive_info);

    xfburn_device_release (drive_info, 0);
  }

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

  g_atomic_int_add (&active_drive_grabs, -1);

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


