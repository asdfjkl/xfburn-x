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

#ifdef HAVE_XFCE_REVISION_H
#include "xfce-revision.h"
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <execinfo.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <libxfce4util/libxfce4util.h>
#include <libburn.h>
#include <libisofs/libisofs.h>

#ifdef HAVE_GST
#include <gst/gst.h>
#endif

#include "xfburn-log.h"
#include "xfburn-global.h"

#define XFBURN_LOG_MAX_SESSION_FILES 10

static GMutex log_mutex;
static gint log_fd = -1;
static gchar *log_file_path = NULL;
static gint64 log_start_monotonic_us = 0;
static gboolean log_initialized = FALSE;
static gboolean log_writer_installed = FALSE;
static gboolean backends_configured = FALSE;
static GPrivate thread_name_key = G_PRIVATE_INIT (g_free);

static struct sigaction old_sigsegv;
static struct sigaction old_sigabrt;
static struct sigaction old_sigbus;
static struct sigaction old_sigfpe;
static gboolean crash_handlers_installed = FALSE;

static const gchar *
level_to_string (XfburnLogLevel level)
{
  switch (level) {
    case XFBURN_LOG_LEVEL_TRACE:
      return "TRACE";
    case XFBURN_LOG_LEVEL_DEBUG:
      return "DEBUG";
    case XFBURN_LOG_LEVEL_INFO:
      return "INFO ";
    case XFBURN_LOG_LEVEL_WARN:
      return "WARN ";
    case XFBURN_LOG_LEVEL_ERROR:
      return "ERROR";
    case XFBURN_LOG_LEVEL_FATAL:
      return "FATAL";
    default:
      return "INFO ";
  }
}

static long
get_current_tid (void)
{
#ifdef SYS_gettid
  return (long) syscall (SYS_gettid);
#else
  return (long) getpid ();
#endif
}

void
xfburn_log_set_thread_name (const gchar *thread_name)
{
  if (thread_name != NULL) {
    g_private_replace (&thread_name_key, g_strdup (thread_name));
  } else {
    g_private_replace (&thread_name_key, NULL);
  }
}

const gchar *
xfburn_log_get_thread_name (void)
{
  const gchar *tname = (const gchar *) g_private_get (&thread_name_key);
  if (tname != NULL) {
    return tname;
  }
  if (g_main_context_is_owner (g_main_context_default ())) {
    return "main";
  }
  return "worker";
}

static void
write_all_fd (gint fd, const gchar *buf, gsize len)
{
  gsize total_written = 0;

  if (fd < 0 || buf == NULL || len == 0) {
    return;
  }

  while (total_written < len) {
    ssize_t w = write (fd, buf + total_written, len - total_written);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (w == 0) {
      break;
    }
    total_written += (gsize) w;
  }
}

static void
prune_old_logs (const gchar *log_dir)
{
  GDir *dir;
  const gchar *entry;
  GList *files = NULL;
  GList *iter;
  guint count = 0;

  dir = g_dir_open (log_dir, 0, NULL);
  if (dir == NULL) {
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    if (g_str_has_prefix (entry, "xfburn-") && g_str_has_suffix (entry, ".log")) {
      files = g_list_prepend (files, g_strdup (entry));
    }
  }
  g_dir_close (dir);

  /* Sort descending by filename (YYYYMMDD-HHMMSS sorts chronologically) */
  files = g_list_sort (files, (GCompareFunc) g_strcmp0);
  files = g_list_reverse (files);

  for (iter = files; iter != NULL; iter = iter->next) {
    count++;
    if (count > XFBURN_LOG_MAX_SESSION_FILES) {
      gchar *old_path = g_build_filename (log_dir, (const gchar *) iter->data, NULL);
      g_unlink (old_path);
      g_free (old_path);
    }
  }

  g_list_free_full (files, g_free);
}

static void
format_and_write_line (XfburnLogLevel level,
                       const gchar *category,
                       const gchar *file,
                       gint line,
                       const gchar *func,
                       const gchar *body)
{
  GDateTime *now;
  gchar *iso_time = NULL;
  gint64 mono_now_us;
  long elapsed_sec = 0;
  long elapsed_msec = 0;
  const gchar *tname;
  long tid;
  const gchar *cat_str = "GEN";
  GString *line_str;

  if (log_fd < 0) {
    return;
  }

  now = g_date_time_new_now_local ();
  if (now != NULL) {
    iso_time = g_date_time_format (now, "%Y-%m-%dT%H:%M:%S.%f%z");
    g_date_time_unref (now);
  }
  if (iso_time == NULL) {
    iso_time = g_strdup ("0000-00-00T00:00:00.000+0000");
  }

  mono_now_us = g_get_monotonic_time ();
  if (log_start_monotonic_us > 0 && mono_now_us >= log_start_monotonic_us) {
    gint64 delta_ms = (mono_now_us - log_start_monotonic_us) / 1000;
    elapsed_sec = (long) (delta_ms / 1000);
    elapsed_msec = (long) (delta_ms % 1000);
  }

  tname = xfburn_log_get_thread_name ();
  tid = get_current_tid ();

  if (category != NULL && category[0] != '\0') {
    cat_str = category;
  }

  line_str = g_string_sized_new (256);
  g_string_append_printf (line_str,
                          "%s [%04ld.%03lds] [%s] [T:%s:%ld] [%s] %s",
                          iso_time,
                          elapsed_sec,
                          elapsed_msec,
                          level_to_string (level),
                          tname,
                          tid,
                          cat_str,
                          body);

  if (file != NULL && line > 0) {
    const gchar *base = strrchr (file, '/');
    if (base != NULL) {
      base = base + 1;
    } else {
      base = file;
    }
    if (func != NULL && func[0] != '\0') {
      g_string_append_printf (line_str, " [%s:%d:%s]", base, line, func);
    } else {
      g_string_append_printf (line_str, " [%s:%d]", base, line);
    }
  }

  g_string_append_c (line_str, '\n');

  g_mutex_lock (&log_mutex);
  if (log_fd >= 0) {
    write_all_fd (log_fd, line_str->str, line_str->len);
    if (level >= XFBURN_LOG_LEVEL_WARN) {
      fsync (log_fd);
    }
  }
  g_mutex_unlock (&log_mutex);

  g_string_free (line_str, TRUE);
  g_free (iso_time);
}

static GLogWriterOutput
xfburn_glib_log_writer (GLogLevelFlags log_level,
                        const GLogField *fields,
                        gsize n_fields,
                        gpointer user_data)
{
  const gchar *message = NULL;
  const gchar *domain = NULL;
  const gchar *code_file = NULL;
  const gchar *code_line_str = NULL;
  const gchar *code_func = NULL;
  gint code_line = 0;
  gsize i;
  XfburnLogLevel xf_level = XFBURN_LOG_LEVEL_INFO;
  gchar *clean_msg = NULL;

  for (i = 0; i < n_fields; i++) {
    if (g_strcmp0 (fields[i].key, "MESSAGE") == 0 && fields[i].value != NULL) {
      message = (const gchar *) fields[i].value;
    } else if (g_strcmp0 (fields[i].key, "GLIB_DOMAIN") == 0 && fields[i].value != NULL) {
      domain = (const gchar *) fields[i].value;
    } else if (g_strcmp0 (fields[i].key, "CODE_FILE") == 0 && fields[i].value != NULL) {
      code_file = (const gchar *) fields[i].value;
    } else if (g_strcmp0 (fields[i].key, "CODE_LINE") == 0 && fields[i].value != NULL) {
      code_line_str = (const gchar *) fields[i].value;
    } else if (g_strcmp0 (fields[i].key, "CODE_FUNC") == 0 && fields[i].value != NULL) {
      code_func = (const gchar *) fields[i].value;
    }
  }

  if (code_line_str != NULL) {
    code_line = atoi (code_line_str);
  }

  if ((log_level & G_LOG_LEVEL_ERROR) != 0) {
    xf_level = XFBURN_LOG_LEVEL_FATAL;
  } else if ((log_level & G_LOG_LEVEL_CRITICAL) != 0) {
    xf_level = XFBURN_LOG_LEVEL_ERROR;
  } else if ((log_level & G_LOG_LEVEL_WARNING) != 0) {
    xf_level = XFBURN_LOG_LEVEL_WARN;
  } else if ((log_level & G_LOG_LEVEL_MESSAGE) != 0 || (log_level & G_LOG_LEVEL_INFO) != 0) {
    xf_level = XFBURN_LOG_LEVEL_INFO;
  } else {
    xf_level = XFBURN_LOG_LEVEL_DEBUG;
  }

  if (log_fd >= 0 && message != NULL) {
    const gchar *dom_str = "default";
    gchar *formatted_body;

    if (domain != NULL && domain[0] != '\0') {
      dom_str = domain;
    }
    clean_msg = g_strdelimit (g_strdup (message), "\r\n", ' ');
    g_strstrip (clean_msg);
    formatted_body = g_strdup_printf ("domain=\"%s\" msg=\"%s\"", dom_str, clean_msg);
    format_and_write_line (xf_level, "GLIB", code_file, code_line, code_func, formatted_body);
    g_free (formatted_body);
    g_free (clean_msg);
  }

  return g_log_writer_default (log_level, fields, n_fields, user_data);
}

static void
write_async_signal_str (gint fd, const char *s)
{
  size_t len = 0;

  if (fd < 0 || s == NULL) {
    return;
  }
  while (s[len] != '\0') {
    len++;
  }
  if (len > 0) {
    ssize_t unused = write (fd, s, len);
    (void) unused;
  }
}

static void
write_async_signal_int (gint fd, int val)
{
  char buf[32];
  int pos = 30;
  int is_neg = 0;
  unsigned int uval;

  buf[31] = '\0';
  if (val < 0) {
    is_neg = 1;
    uval = (unsigned int) (-val);
  } else {
    uval = (unsigned int) val;
  }

  if (uval == 0) {
    buf[pos] = '0';
    pos--;
  } else {
    while (uval > 0 && pos > 1) {
      buf[pos] = (char) ('0' + (uval % 10));
      uval /= 10;
      pos--;
    }
  }

  if (is_neg && pos > 0) {
    buf[pos] = '-';
    pos--;
  }

  write_async_signal_str (fd, &buf[pos + 1]);
}

static void
crash_signal_handler (int sig, siginfo_t *info, void *ucontext)
{
  void *frames[64];
  int n_frames;
  const char *sig_name = "UNKNOWN";
  struct sigaction *prev_action = NULL;

  if (sig == SIGSEGV) {
    sig_name = "SIGSEGV";
    prev_action = &old_sigsegv;
  } else if (sig == SIGABRT) {
    sig_name = "SIGABRT";
    prev_action = &old_sigabrt;
  } else if (sig == SIGBUS) {
    sig_name = "SIGBUS";
    prev_action = &old_sigbus;
  } else if (sig == SIGFPE) {
    sig_name = "SIGFPE";
    prev_action = &old_sigfpe;
  }

  if (log_fd >= 0) {
    write_async_signal_str (log_fd, "\n0000-00-00T00:00:00.000+0000 [0000.000s] [FATAL] [T:signal:");
    write_async_signal_int (log_fd, (int) get_current_tid ());
    write_async_signal_str (log_fd, "] [SIGNAL] Caught fatal signal ");
    write_async_signal_int (log_fd, sig);
    write_async_signal_str (log_fd, " (");
    write_async_signal_str (log_fd, sig_name);
    write_async_signal_str (log_fd, ") pid=");
    write_async_signal_int (log_fd, (int) getpid ());
    write_async_signal_str (log_fd, " - Stack trace:\n");

    n_frames = backtrace (frames, 64);
    if (n_frames > 0) {
      backtrace_symbols_fd (frames, n_frames, log_fd);
    }
    fsync (log_fd);
  }

  if (prev_action != NULL) {
    if ((prev_action->sa_flags & SA_SIGINFO) != 0 && prev_action->sa_sigaction != NULL) {
      prev_action->sa_sigaction (sig, info, ucontext);
      return;
    }
    if (prev_action->sa_handler != SIG_DFL &&
        prev_action->sa_handler != SIG_IGN &&
        prev_action->sa_handler != NULL) {
      prev_action->sa_handler (sig);
      return;
    }
  }

  signal (sig, SIG_DFL);
  raise (sig);
}

void
xfburn_log_install_crash_handlers (void)
{
  struct sigaction sa;

  if (crash_handlers_installed) {
    return;
  }

  memset (&sa, 0, sizeof (sa));
  sa.sa_sigaction = crash_signal_handler;
  sigemptyset (&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;

  sigaction (SIGSEGV, &sa, &old_sigsegv);
  sigaction (SIGABRT, &sa, &old_sigabrt);
  sigaction (SIGBUS, &sa, &old_sigbus);
  sigaction (SIGFPE, &sa, &old_sigfpe);

  crash_handlers_installed = TRUE;
}

static void
write_session_header (void)
{
  struct utsname uts;
  int burn_maj = 0, burn_min = 0, burn_pl = 0;
  int iso_maj = 0, iso_min = 0, iso_pl = 0;
  const gchar *tmp_dir;
  struct statvfs vfs;
  guint64 tmp_free_mb = 0;
#ifdef HAVE_GST
  guint gst_maj = 0, gst_min = 0, gst_mic = 0, gst_nano = 0;
#endif
  const gchar *gudev_state;
  const gchar *header_banner =
    "================================================================================\n"
    "[XFBURN DIAGNOSTIC FLIGHT RECORDER LOG - FORMAT v1.0]\n"
    "================================================================================\n";

  g_mutex_lock (&log_mutex);
  if (log_fd >= 0) {
    write_all_fd (log_fd, header_banner, strlen (header_banner));
  }
  g_mutex_unlock (&log_mutex);

  burn_version (&burn_maj, &burn_min, &burn_pl);
  iso_lib_version (&iso_maj, &iso_min, &iso_pl);

#ifdef HAVE_GUDEV
  gudev_state = "enabled";
#else
  gudev_state = "disabled";
#endif

  xfburn_log_message (XFBURN_LOG_LEVEL_INFO, "ENV", NULL, 0, NULL,
                      "app=\"%s\" version=\"%s\" xfce=\"%s\" pid=%d log_file=\"%s\"",
                      PACKAGE, VERSION_FULL, xfce_version_string (), (int) getpid (),
                      log_file_path);

#ifdef HAVE_GST
  gst_version (&gst_maj, &gst_min, &gst_mic, &gst_nano);
  xfburn_log_message (XFBURN_LOG_LEVEL_INFO, "ENV", NULL, 0, NULL,
                      "libs gtk=\"%u.%u.%u\" glib=\"%u.%u.%u\" libburn=\"%d.%d.%d\" libisofs=\"%d.%d.%d\" gst=\"%u.%u.%u\" gudev=\"%s\"",
                      gtk_major_version, gtk_minor_version, gtk_micro_version,
                      glib_major_version, glib_minor_version, glib_micro_version,
                      burn_maj, burn_min, burn_pl,
                      iso_maj, iso_min, iso_pl,
                      gst_maj, gst_min, gst_mic,
                      gudev_state);
#else
  xfburn_log_message (XFBURN_LOG_LEVEL_INFO, "ENV", NULL, 0, NULL,
                      "libs gtk=\"%u.%u.%u\" glib=\"%u.%u.%u\" libburn=\"%d.%d.%d\" libisofs=\"%d.%d.%d\" gst=\"disabled\" gudev=\"%s\"",
                      gtk_major_version, gtk_minor_version, gtk_micro_version,
                      glib_major_version, glib_minor_version, glib_micro_version,
                      burn_maj, burn_min, burn_pl,
                      iso_maj, iso_min, iso_pl,
                      gudev_state);
#endif

  tmp_dir = g_get_tmp_dir ();
  if (statvfs (tmp_dir, &vfs) == 0) {
    tmp_free_mb = ((guint64) vfs.f_bavail * (guint64) vfs.f_frsize) / (1024ULL * 1024ULL);
  }

  if (uname (&uts) == 0) {
    xfburn_log_message (XFBURN_LOG_LEVEL_INFO, "SYS", NULL, 0, NULL,
                        "sysname=\"%s\" release=\"%s\" version=\"%s\" machine=\"%s\" tmpdir=\"%s\" tmpdir_free_mb=%" G_GUINT64_FORMAT,
                        uts.sysname, uts.release, uts.version, uts.machine,
                        tmp_dir, tmp_free_mb);
  } else {
    xfburn_log_message (XFBURN_LOG_LEVEL_INFO, "SYS", NULL, 0, NULL,
                        "tmpdir=\"%s\" tmpdir_free_mb=%" G_GUINT64_FORMAT,
                        tmp_dir, tmp_free_mb);
  }
}

gboolean
xfburn_log_init (const gchar *custom_path)
{
  const gchar *env_path;
  const gchar *target_path = NULL;
  gchar *auto_dir = NULL;
  gchar *auto_filename = NULL;
  GDateTime *now;
  gchar *ts_str = NULL;

  if (log_initialized) {
    return TRUE;
  }

  log_start_monotonic_us = g_get_monotonic_time ();
  xfburn_log_set_thread_name ("main");

  if (custom_path != NULL && custom_path[0] != '\0') {
    target_path = custom_path;
  } else {
    env_path = g_getenv ("XFBURN_LOG_FILE");
    if (env_path != NULL && env_path[0] != '\0') {
      target_path = env_path;
    }
  }

  if (target_path != NULL) {
    gchar *parent_dir = g_path_get_dirname (target_path);
    if (parent_dir != NULL) {
      g_mkdir_with_parents (parent_dir, 0700);
      g_free (parent_dir);
    }
    log_file_path = g_strdup (target_path);
  } else {
    const gchar *xdg_state = g_getenv ("XDG_STATE_HOME");
    if (xdg_state != NULL && xdg_state[0] != '\0') {
      auto_dir = g_build_filename (xdg_state, "xfburn", "logs", NULL);
    } else {
      auto_dir = g_build_filename (g_get_home_dir (), ".local", "state", "xfburn", "logs", NULL);
    }
    g_mkdir_with_parents (auto_dir, 0700);
    prune_old_logs (auto_dir);

    now = g_date_time_new_now_local ();
    if (now != NULL) {
      ts_str = g_date_time_format (now, "%Y%m%d-%H%M%S");
      g_date_time_unref (now);
    }
    if (ts_str == NULL) {
      ts_str = g_strdup ("00000000-000000");
    }

    auto_filename = g_strdup_printf ("xfburn-%s-%d.log", ts_str, (int) getpid ());
    log_file_path = g_build_filename (auto_dir, auto_filename, NULL);
    g_free (ts_str);
    g_free (auto_filename);
    g_free (auto_dir);
  }

  log_fd = open (log_file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (log_fd < 0) {
    g_free (log_file_path);
    log_file_path = NULL;
    return FALSE;
  }

  log_initialized = TRUE;

  if (!log_writer_installed) {
    g_log_set_writer_func (xfburn_glib_log_writer, NULL, NULL);
    log_writer_installed = TRUE;
  }

  xfburn_log_install_crash_handlers ();
  write_session_header ();

  return TRUE;
}

void
xfburn_log_shutdown (void)
{
  if (!log_initialized) {
    return;
  }

  if (backends_configured) {
    xfburn_log_drain_backend_messages ("shutdown", NULL, 0);
    backends_configured = FALSE;
  }
  xfburn_log_message (XFBURN_LOG_LEVEL_INFO, "ENV", NULL, 0, NULL, "Session log closing cleanly.");

  g_mutex_lock (&log_mutex);
  if (log_fd >= 0) {
    fsync (log_fd);
    close (log_fd);
    log_fd = -1;
  }
  g_free (log_file_path);
  log_file_path = NULL;
  log_initialized = FALSE;
  g_mutex_unlock (&log_mutex);
}

gboolean
xfburn_log_is_initialized (void)
{
  return log_initialized;
}

const gchar *
xfburn_log_get_file_path (void)
{
  return log_file_path;
}

void
xfburn_log_flush (void)
{
  g_mutex_lock (&log_mutex);
  if (log_fd >= 0) {
    fsync (log_fd);
  }
  g_mutex_unlock (&log_mutex);
}

gboolean
xfburn_log_copy_to_file (const gchar *dest_path, GError **error)
{
  gchar *contents = NULL;
  gsize length = 0;
  gboolean ok;

  if (dest_path == NULL || dest_path[0] == '\0') {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Invalid destination path");
    return FALSE;
  }

  if (!log_initialized || log_file_path == NULL) {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Diagnostic logger is not initialized");
    return FALSE;
  }

  xfburn_log_drain_backend_messages ("export_log", NULL, 0);
  xfburn_log_flush ();

  if (!g_file_get_contents (log_file_path, &contents, &length, error)) {
    return FALSE;
  }

  ok = g_file_set_contents (dest_path, contents, (gssize) length, error);
  g_free (contents);

  return ok;
}

void
xfburn_log_message (XfburnLogLevel level,
                    const gchar *category,
                    const gchar *file,
                    gint line,
                    const gchar *func,
                    const gchar *format,
                    ...)
{
  va_list args;
  gchar *body;

  if (!log_initialized || log_fd < 0 || format == NULL) {
    return;
  }

  va_start (args, format);
  body = g_strdup_vprintf (format, args);
  va_end (args);

  g_strdelimit (body, "\r\n", ' ');
  g_strstrip (body);

  format_and_write_line (level, category, file, line, func, body);
  g_free (body);
}

void
xfburn_log_call_enter (const gchar *call_name,
                       const gchar *file,
                       gint line,
                       const gchar *func,
                       const gchar *details_fmt,
                       ...)
{
  va_list args;
  gchar *details = NULL;
  gchar *body;

  if (!log_initialized || log_fd < 0) {
    return;
  }

  if (details_fmt != NULL && details_fmt[0] != '\0') {
    va_start (args, details_fmt);
    details = g_strdup_vprintf (details_fmt, args);
    va_end (args);
  }

  if (details != NULL && details[0] != '\0') {
    body = g_strdup_printf (">>> %s(%s)", call_name, details);
  } else {
    body = g_strdup_printf (">>> %s()", call_name);
  }

  format_and_write_line (XFBURN_LOG_LEVEL_TRACE, "CALL", file, line, func, body);

  g_free (body);
  g_free (details);
}

void
xfburn_log_call_exit (const gchar *call_name,
                      gint64 start_time_us,
                      const gchar *file,
                      gint line,
                      const gchar *func,
                      const gchar *result_fmt,
                      ...)
{
  va_list args;
  gchar *result = NULL;
  gchar *body;
  gint64 now_us;
  long elapsed_ms_whole = 0;
  long elapsed_ms_frac = 0;

  if (!log_initialized || log_fd < 0) {
    return;
  }

  now_us = g_get_monotonic_time ();
  if (start_time_us > 0 && now_us >= start_time_us) {
    gint64 delta_us = now_us - start_time_us;
    elapsed_ms_whole = (long) (delta_us / 1000);
    elapsed_ms_frac = (long) ((delta_us % 1000) / 10);
  }

  if (result_fmt != NULL && result_fmt[0] != '\0') {
    va_start (args, result_fmt);
    result = g_strdup_vprintf (result_fmt, args);
    va_end (args);
  }

  if (result != NULL && result[0] != '\0') {
    body = g_strdup_printf ("<<< %s elapsed_ms=%ld.%02ld %s",
                            call_name, elapsed_ms_whole, elapsed_ms_frac, result);
  } else {
    body = g_strdup_printf ("<<< %s elapsed_ms=%ld.%02ld",
                            call_name, elapsed_ms_whole, elapsed_ms_frac);
  }

  format_and_write_line (XFBURN_LOG_LEVEL_TRACE, "RET ", file, line, func, body);

  g_free (body);
  g_free (result);
}

void
xfburn_log_configure_backends (void)
{
  int ret_burn;
  int ret_iso;

  if (burn_initialize () == 0) {
    xfburn_log_message (XFBURN_LOG_LEVEL_WARN, "LIBBURN", __FILE__, __LINE__, G_STRFUNC,
                        "burn_initialize() returned 0 in xfburn_log_configure_backends");
    return;
  }

  if (iso_init () < 0) {
    xfburn_log_message (XFBURN_LOG_LEVEL_WARN, "LIBISOFS", __FILE__, __LINE__, G_STRFUNC,
                        "iso_init() failed in xfburn_log_configure_backends");
    return;
  }

  ret_burn = burn_msgs_set_severities ("ALL", "NEVER", "libburn-");
  if (ret_burn <= 0) {
    xfburn_log_message (XFBURN_LOG_LEVEL_WARN, "LIBBURN", __FILE__, __LINE__, G_STRFUNC,
                        "burn_msgs_set_severities(\"ALL\", \"NEVER\") returned %d", ret_burn);
  }

  ret_iso = iso_set_msgs_severities ("ALL", "NEVER", "libisofs-");
  if (ret_iso <= 0) {
    xfburn_log_message (XFBURN_LOG_LEVEL_WARN, "LIBISOFS", __FILE__, __LINE__, G_STRFUNC,
                        "iso_set_msgs_severities(\"ALL\", \"NEVER\") returned %d", ret_iso);
  }

  backends_configured = TRUE;
}

static gboolean
is_failure_severity (const gchar *severity)
{
  if (severity == NULL) {
    return FALSE;
  }
  if (g_strcmp0 (severity, "FATAL") == 0 ||
      g_strcmp0 (severity, "ABORT") == 0 ||
      g_strcmp0 (severity, "FAILURE") == 0 ||
      g_strcmp0 (severity, "MISHAP") == 0 ||
      g_strcmp0 (severity, "SORRY") == 0) {
    return TRUE;
  }
  return FALSE;
}

static XfburnLogLevel
severity_to_log_level (const gchar *severity)
{
  if (severity == NULL) {
    return XFBURN_LOG_LEVEL_INFO;
  }
  if (g_strcmp0 (severity, "FATAL") == 0 ||
      g_strcmp0 (severity, "ABORT") == 0) {
    return XFBURN_LOG_LEVEL_FATAL;
  }
  if (g_strcmp0 (severity, "FAILURE") == 0 ||
      g_strcmp0 (severity, "MISHAP") == 0 ||
      g_strcmp0 (severity, "SORRY") == 0) {
    return XFBURN_LOG_LEVEL_ERROR;
  }
  if (g_strcmp0 (severity, "WARNING") == 0 ||
      g_strcmp0 (severity, "HINT") == 0) {
    return XFBURN_LOG_LEVEL_WARN;
  }
  if (g_strcmp0 (severity, "NOTE") == 0 ||
      g_strcmp0 (severity, "UPDATE") == 0) {
    return XFBURN_LOG_LEVEL_INFO;
  }
  return XFBURN_LOG_LEVEL_DEBUG;
}

gint
xfburn_log_drain_backend_messages (const gchar *context,
                                   gchar *last_failure_buf,
                                   gsize last_failure_buf_len)
{
  gint failure_count = 0;
  int ret;
  int error_code = 0;
  int os_errno = 0;
  int imgid = 0;
  char msg_text[BURN_MSGS_MESSAGE_LEN];
  char iso_msg_text[ISO_MSGS_MESSAGE_LEN];
  char severity[80];
  const gchar *ctx_str = "general";

  if (!backends_configured) {
    return 0;
  }

  if (context != NULL && context[0] != '\0') {
    ctx_str = context;
  }

  while (TRUE) {
    error_code = 0;
    os_errno = 0;
    msg_text[0] = '\0';
    severity[0] = '\0';

    ret = burn_msgs_obtain ("ALL", &error_code, msg_text, &os_errno, severity);
    if (ret <= 0) {
      if (ret < 0) {
        xfburn_log_message (XFBURN_LOG_LEVEL_ERROR, "LIBBURN", __FILE__, __LINE__, G_STRFUNC,
                            "ctx=\"%s\" burn_msgs_obtain returned error %d", ctx_str, ret);
      }
      break;
    }

    g_strdelimit (msg_text, "\r\n", ' ');
    g_strstrip (msg_text);

    if (os_errno != 0) {
      xfburn_log_message (severity_to_log_level (severity), "LIBBURN", NULL, 0, NULL,
                          "ctx=\"%s\" sev=\"%s\" code=%d (0x%X) errno=%d (\"%s\") msg=\"%s\"",
                          ctx_str, severity, error_code, (unsigned int) error_code,
                          os_errno, g_strerror (os_errno), msg_text);
    } else {
      xfburn_log_message (severity_to_log_level (severity), "LIBBURN", NULL, 0, NULL,
                          "ctx=\"%s\" sev=\"%s\" code=%d (0x%X) errno=0 msg=\"%s\"",
                          ctx_str, severity, error_code, (unsigned int) error_code, msg_text);
    }

    if (is_failure_severity (severity)) {
      failure_count++;
      if (last_failure_buf != NULL && last_failure_buf_len > 0) {
        g_strlcpy (last_failure_buf, msg_text, last_failure_buf_len);
      }
    }
  }

  while (TRUE) {
    error_code = 0;
    imgid = 0;
    iso_msg_text[0] = '\0';
    severity[0] = '\0';

    ret = iso_obtain_msgs ("ALL", &error_code, &imgid, iso_msg_text, severity);
    if (ret <= 0) {
      if (ret < 0) {
        xfburn_log_message (XFBURN_LOG_LEVEL_ERROR, "LIBISOFS", __FILE__, __LINE__, G_STRFUNC,
                            "ctx=\"%s\" iso_obtain_msgs returned error %d", ctx_str, ret);
      }
      break;
    }

    g_strdelimit (iso_msg_text, "\r\n", ' ');
    g_strstrip (iso_msg_text);

    xfburn_log_message (severity_to_log_level (severity), "LIBISOFS", NULL, 0, NULL,
                        "ctx=\"%s\" sev=\"%s\" code=%d (0x%X) imgid=%d msg=\"%s\"",
                        ctx_str, severity, error_code, (unsigned int) error_code, imgid, iso_msg_text);

    if (is_failure_severity (severity)) {
      failure_count++;
      if (last_failure_buf != NULL && last_failure_buf_len > 0 && last_failure_buf[0] == '\0') {
        g_strlcpy (last_failure_buf, iso_msg_text, last_failure_buf_len);
      }
    }
  }

  return failure_count;
}

static gchar *
resolve_block_sysfs_dir (const gchar *dev_node, const gchar *sysfs_root_override)
{
  const gchar *sys_root = "/sys";
  gchar *dev_name;
  gchar *block_dir;

  if (dev_node == NULL || dev_node[0] == '\0') {
    return NULL;
  }
  if (g_str_has_prefix (dev_node, "stdio:")) {
    return NULL;
  }

  if (sysfs_root_override != NULL && sysfs_root_override[0] != '\0') {
    sys_root = sysfs_root_override;
  }

  dev_name = g_path_get_basename (dev_node);
  if (dev_name == NULL || dev_name[0] == '\0' || g_strcmp0 (dev_name, ".") == 0 || g_strcmp0 (dev_name, "/") == 0) {
    g_free (dev_name);
    return NULL;
  }

  block_dir = g_build_filename (sys_root, "class", "block", dev_name, NULL);
  if (g_file_test (block_dir, G_FILE_TEST_EXISTS)) {
    g_free (dev_name);
    return block_dir;
  }
  g_free (block_dir);

  block_dir = g_build_filename (sys_root, "block", dev_name, NULL);
  if (g_file_test (block_dir, G_FILE_TEST_EXISTS)) {
    g_free (dev_name);
    return block_dir;
  }
  g_free (block_dir);
  g_free (dev_name);

  return NULL;
}

static gchar *
read_sysfs_attr_str (const gchar *base_dir, const gchar *rel_path)
{
  gchar *full_path;
  gchar *contents = NULL;
  gsize len = 0;

  if (base_dir == NULL || rel_path == NULL) {
    return NULL;
  }

  full_path = g_build_filename (base_dir, rel_path, NULL);
  if (!g_file_get_contents (full_path, &contents, &len, NULL)) {
    g_free (full_path);
    return NULL;
  }
  g_free (full_path);

  if (contents != NULL) {
    g_strdelimit (contents, "\r\n", ' ');
    g_strstrip (contents);
  }

  return contents;
}

static gboolean
read_sysfs_attr_i64 (const gchar *base_dir, const gchar *rel_path, gint64 *out_val)
{
  gchar *str;
  gchar *endptr = NULL;
  guint64 val;

  if (out_val != NULL) {
    *out_val = 0;
  }

  str = read_sysfs_attr_str (base_dir, rel_path);
  if (str == NULL || str[0] == '\0') {
    g_free (str);
    return FALSE;
  }

  val = g_ascii_strtoull (str, &endptr, 0);
  if (endptr == str) {
    g_free (str);
    return FALSE;
  }

  if (out_val != NULL) {
    *out_val = (gint64) val;
  }
  g_free (str);
  return TRUE;
}

static const gchar *
non_null_or_unknown (const gchar *str)
{
  if (str != NULL && str[0] != '\0') {
    return str;
  }
  return "unknown";
}

gboolean
xfburn_log_read_device_io_counters (const gchar *dev_node,
                                    const gchar *sysfs_root_override,
                                    XfburnDeviceIoCounters *counters)
{
  gchar *block_dir;
  gchar *state_str;
  gboolean has_req;
  gboolean has_done;
  gboolean has_err;

  if (counters == NULL) {
    return FALSE;
  }
  memset (counters, 0, sizeof (*counters));

  block_dir = resolve_block_sysfs_dir (dev_node, sysfs_root_override);
  if (block_dir == NULL) {
    return FALSE;
  }

  has_req = read_sysfs_attr_i64 (block_dir, "device/iorequest_cnt", &counters->iorequest_cnt);
  has_done = read_sysfs_attr_i64 (block_dir, "device/iodone_cnt", &counters->iodone_cnt);
  has_err = read_sysfs_attr_i64 (block_dir, "device/ioerr_cnt", &counters->ioerr_cnt);
  state_str = read_sysfs_attr_str (block_dir, "device/state");
  if (state_str != NULL) {
    g_strlcpy (counters->state, state_str, sizeof (counters->state));
    g_free (state_str);
  } else {
    g_strlcpy (counters->state, "unknown", sizeof (counters->state));
  }

  g_free (block_dir);

  if (has_req || has_done || has_err) {
    counters->valid = TRUE;
    return TRUE;
  }

  return FALSE;
}

void
xfburn_log_dump_device_hardware (const gchar *dev_node,
                                 const gchar *sysfs_root_override)
{
  gchar *block_dir;
  gchar *vendor;
  gchar *model;
  gchar *rev;
  gchar *state;
  gchar *timeout;
  gchar *max_sectors_kb;
  XfburnDeviceIoCounters counters;
  gchar *dev_subdir;
  char *resolved_dev;
  gchar *cur_dir;
  gint depth;

  if (!log_initialized || dev_node == NULL || dev_node[0] == '\0') {
    return;
  }

  block_dir = resolve_block_sysfs_dir (dev_node, sysfs_root_override);
  if (block_dir == NULL) {
    xfburn_log_message (XFBURN_LOG_LEVEL_DEBUG, "HW_DEV", NULL, 0, NULL,
                        "dev=\"%s\" sysfs_status=\"not_found\"", dev_node);
    return;
  }

  vendor = read_sysfs_attr_str (block_dir, "device/vendor");
  model = read_sysfs_attr_str (block_dir, "device/model");
  rev = read_sysfs_attr_str (block_dir, "device/rev");
  state = read_sysfs_attr_str (block_dir, "device/state");
  timeout = read_sysfs_attr_str (block_dir, "device/timeout");
  max_sectors_kb = read_sysfs_attr_str (block_dir, "queue/max_sectors_kb");
  xfburn_log_read_device_io_counters (dev_node, sysfs_root_override, &counters);

  xfburn_log_message (XFBURN_LOG_LEVEL_INFO, "HW_DEV", NULL, 0, NULL,
                      "dev=\"%s\" vendor=\"%s\" model=\"%s\" rev=\"%s\" state=\"%s\" timeout=\"%s\" max_sectors_kb=\"%s\" iorequest_cnt=%" G_GINT64_FORMAT " iodone_cnt=%" G_GINT64_FORMAT " ioerr_cnt=%" G_GINT64_FORMAT,
                      dev_node,
                      non_null_or_unknown (vendor),
                      non_null_or_unknown (model),
                      non_null_or_unknown (rev),
                      non_null_or_unknown (state),
                      non_null_or_unknown (timeout),
                      non_null_or_unknown (max_sectors_kb),
                      counters.iorequest_cnt,
                      counters.iodone_cnt,
                      counters.ioerr_cnt);

  /* Walk parent directories of <block_dir>/device to locate any parent USB device node */
  dev_subdir = g_build_filename (block_dir, "device", NULL);
  resolved_dev = realpath (dev_subdir, NULL);
  if (resolved_dev != NULL) {
    cur_dir = g_strdup (resolved_dev);
    free (resolved_dev);
  } else {
    cur_dir = g_strdup (dev_subdir);
  }
  g_free (dev_subdir);

  for (depth = 0; depth < 10 && cur_dir != NULL; depth++) {
    gchar *usb_speed = read_sysfs_attr_str (cur_dir, "speed");
    gchar *usb_max_power = read_sysfs_attr_str (cur_dir, "bMaxPower");
    gchar *usb_id_vendor = read_sysfs_attr_str (cur_dir, "idVendor");

    if (usb_speed != NULL || usb_max_power != NULL || usb_id_vendor != NULL) {
      gchar *usb_id_product = read_sysfs_attr_str (cur_dir, "idProduct");
      gchar *usb_version = read_sysfs_attr_str (cur_dir, "version");
      gchar *power_control = read_sysfs_attr_str (cur_dir, "power/control");
      gchar *runtime_status = read_sysfs_attr_str (cur_dir, "power/runtime_status");
      gchar *manufacturer = read_sysfs_attr_str (cur_dir, "manufacturer");
      gchar *product = read_sysfs_attr_str (cur_dir, "product");
      gchar *usb_dir_base = g_path_get_basename (cur_dir);

      xfburn_log_message (XFBURN_LOG_LEVEL_INFO, "HW_USB", NULL, 0, NULL,
                          "dev=\"%s\" usb_node=\"%s\" id=\"%s:%s\" speed_mbps=\"%s\" usb_ver=\"%s\" max_power=\"%s\" power_control=\"%s\" runtime_status=\"%s\" manufacturer=\"%s\" product=\"%s\"",
                          dev_node,
                          non_null_or_unknown (usb_dir_base),
                          non_null_or_unknown (usb_id_vendor),
                          non_null_or_unknown (usb_id_product),
                          non_null_or_unknown (usb_speed),
                          non_null_or_unknown (usb_version),
                          non_null_or_unknown (usb_max_power),
                          non_null_or_unknown (power_control),
                          non_null_or_unknown (runtime_status),
                          non_null_or_unknown (manufacturer),
                          non_null_or_unknown (product));

      g_free (usb_dir_base);
      g_free (product);
      g_free (manufacturer);
      g_free (runtime_status);
      g_free (power_control);
      g_free (usb_version);
      g_free (usb_id_product);
      g_free (usb_id_vendor);
      g_free (usb_max_power);
      g_free (usb_speed);
      break;
    }

    g_free (usb_id_vendor);
    g_free (usb_max_power);
    g_free (usb_speed);

    if (g_strcmp0 (cur_dir, "/") == 0 || g_strcmp0 (cur_dir, ".") == 0) {
      break;
    }
    {
      gchar *parent = g_path_get_dirname (cur_dir);
      g_free (cur_dir);
      cur_dir = parent;
    }
  }

  g_free (cur_dir);
  g_free (max_sectors_kb);
  g_free (timeout);
  g_free (state);
  g_free (rev);
  g_free (model);
  g_free (vendor);
  g_free (block_dir);
}
