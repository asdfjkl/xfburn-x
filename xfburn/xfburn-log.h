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

#ifndef __XFBURN_LOG_H__
#define __XFBURN_LOG_H__

#include <glib.h>

typedef enum
{
  XFBURN_LOG_LEVEL_TRACE = 0,
  XFBURN_LOG_LEVEL_DEBUG = 1,
  XFBURN_LOG_LEVEL_INFO  = 2,
  XFBURN_LOG_LEVEL_WARN  = 3,
  XFBURN_LOG_LEVEL_ERROR = 4,
  XFBURN_LOG_LEVEL_FATAL = 5
} XfburnLogLevel;

typedef struct
{
  gboolean valid;
  gint64 iorequest_cnt;
  gint64 iodone_cnt;
  gint64 ioerr_cnt;
  gchar state[64];
} XfburnDeviceIoCounters;

gboolean xfburn_log_init (const gchar *custom_path);
void xfburn_log_shutdown (void);
gboolean xfburn_log_is_initialized (void);
const gchar *xfburn_log_get_file_path (void);
gboolean xfburn_log_copy_to_file (const gchar *dest_path, GError **error);
void xfburn_log_flush (void);

void xfburn_log_set_thread_name (const gchar *thread_name);
const gchar *xfburn_log_get_thread_name (void);

void xfburn_log_message (XfburnLogLevel level,
                         const gchar *category,
                         const gchar *file,
                         gint line,
                         const gchar *func,
                         const gchar *format,
                         ...) G_GNUC_PRINTF (6, 7);

void xfburn_log_call_enter (const gchar *call_name,
                            const gchar *file,
                            gint line,
                            const gchar *func,
                            const gchar *details_fmt,
                            ...) G_GNUC_PRINTF (5, 6);

void xfburn_log_call_exit (const gchar *call_name,
                           gint64 start_time_us,
                           const gchar *file,
                           gint line,
                           const gchar *func,
                           const gchar *result_fmt,
                           ...) G_GNUC_PRINTF (6, 7);

void xfburn_log_configure_backends (void);
gint xfburn_log_drain_backend_messages (const gchar *context,
                                        gchar *last_failure_buf,
                                        gsize last_failure_buf_len);

void xfburn_log_dump_device_hardware (const gchar *dev_node,
                                      const gchar *sysfs_root_override);
gboolean xfburn_log_read_device_io_counters (const gchar *dev_node,
                                             const gchar *sysfs_root_override,
                                             XfburnDeviceIoCounters *counters);

void xfburn_log_install_crash_handlers (void);

#define XFBURN_LOG_TRACE(cat, ...) \
  xfburn_log_message (XFBURN_LOG_LEVEL_TRACE, (cat), __FILE__, __LINE__, G_STRFUNC, __VA_ARGS__)

#define XFBURN_LOG_DEBUG(cat, ...) \
  xfburn_log_message (XFBURN_LOG_LEVEL_DEBUG, (cat), __FILE__, __LINE__, G_STRFUNC, __VA_ARGS__)

#define XFBURN_LOG_INFO(cat, ...) \
  xfburn_log_message (XFBURN_LOG_LEVEL_INFO, (cat), __FILE__, __LINE__, G_STRFUNC, __VA_ARGS__)

#define XFBURN_LOG_WARN(cat, ...) \
  xfburn_log_message (XFBURN_LOG_LEVEL_WARN, (cat), __FILE__, __LINE__, G_STRFUNC, __VA_ARGS__)

#define XFBURN_LOG_ERROR(cat, ...) \
  xfburn_log_message (XFBURN_LOG_LEVEL_ERROR, (cat), __FILE__, __LINE__, G_STRFUNC, __VA_ARGS__)

#define XFBURN_LOG_CALL_ENTER(call_name, ...) \
  xfburn_log_call_enter ((call_name), __FILE__, __LINE__, G_STRFUNC, __VA_ARGS__)

#define XFBURN_LOG_CALL_EXIT(call_name, start_us, ...) \
  xfburn_log_call_exit ((call_name), (start_us), __FILE__, __LINE__, G_STRFUNC, __VA_ARGS__)

#endif /* __XFBURN_LOG_H__ */
