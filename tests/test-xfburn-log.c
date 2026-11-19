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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <libburn.h>
#include <libisofs/libisofs.h>

#include "xfburn-log.h"

static void
remove_tree_recursive (const gchar *path)
{
  GDir *dir;
  const gchar *entry;

  if (path == NULL) {
    return;
  }

  if (g_file_test (path, G_FILE_TEST_IS_SYMLINK)) {
    g_unlink (path);
    return;
  }

  if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
    dir = g_dir_open (path, 0, NULL);
    if (dir != NULL) {
      while ((entry = g_dir_read_name (dir)) != NULL) {
        gchar *child = g_build_filename (path, entry, NULL);
        remove_tree_recursive (child);
        g_free (child);
      }
      g_dir_close (dir);
    }
    g_rmdir (path);
  } else {
    g_unlink (path);
  }
}

static void
write_text_file (const gchar *base_dir, const gchar *rel_path, const gchar *content)
{
  gchar *full_path;
  gchar *parent_dir;
  GError *error = NULL;
  gboolean ok;

  full_path = g_build_filename (base_dir, rel_path, NULL);
  parent_dir = g_path_get_dirname (full_path);
  g_mkdir_with_parents (parent_dir, 0700);

  ok = g_file_set_contents (full_path, content, -1, &error);
  g_assert_no_error (error);
  g_assert_true (ok);

  g_free (parent_dir);
  g_free (full_path);
}

static void
test_init_and_header (void)
{
  GError *error = NULL;
  gchar *tmp_dir;
  gchar *log_path;
  gchar *export_path;
  gchar *contents = NULL;
  gchar *export_contents = NULL;
  gboolean ok;

  tmp_dir = g_dir_make_tmp ("xfburn-test-init-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (tmp_dir);

  log_path = g_build_filename (tmp_dir, "session.log", NULL);
  export_path = g_build_filename (tmp_dir, "exported.log", NULL);

  ok = xfburn_log_init (log_path);
  g_assert_true (ok);
  g_assert_true (xfburn_log_is_initialized ());
  g_assert_cmpstr (xfburn_log_get_file_path (), ==, log_path);

  XFBURN_LOG_INFO ("TEST", "hello world %d", 42);
  g_message ("test glib structured message capture");

  ok = xfburn_log_copy_to_file (export_path, &error);
  g_assert_no_error (error);
  g_assert_true (ok);

  xfburn_log_shutdown ();
  g_assert_false (xfburn_log_is_initialized ());

  ok = g_file_get_contents (log_path, &contents, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_nonnull (contents);

  g_assert_nonnull (strstr (contents, "[XFBURN-X DIAGNOSTIC FLIGHT RECORDER LOG - FORMAT v1.0]"));
  g_assert_nonnull (strstr (contents, "[ENV] app=\"xfburn-x\""));
  g_assert_nonnull (strstr (contents, "[ENV] libs gtk="));
  g_assert_nonnull (strstr (contents, "[SYS] "));
  g_assert_nonnull (strstr (contents, "[TEST] hello world 42"));
  g_assert_nonnull (strstr (contents, "[GLIB] domain=\"xfburn-x\" msg=\"test glib structured message capture\""));
  g_assert_nonnull (strstr (contents, "Session log closing cleanly."));

  ok = g_file_get_contents (export_path, &export_contents, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_nonnull (export_contents);
  g_assert_nonnull (strstr (export_contents, "[TEST] hello world 42"));

  g_free (export_contents);
  g_free (contents);
  g_free (export_path);
  g_free (log_path);
  remove_tree_recursive (tmp_dir);
  g_free (tmp_dir);
}

typedef struct
{
  gint worker_id;
  gint iterations;
} WorkerArgs;

static gpointer
multithreaded_worker_func (gpointer data)
{
  WorkerArgs *args = (WorkerArgs *) data;
  gchar *tname;
  gint i;

  tname = g_strdup_printf ("worker-%d", args->worker_id);
  xfburn_log_set_thread_name (tname);
  g_assert_cmpstr (xfburn_log_get_thread_name (), ==, tname);

  for (i = 0; i < args->iterations; i++) {
    gint64 t0 = g_get_monotonic_time ();
    XFBURN_LOG_CALL_ENTER ("mock_burn_op", "worker=%d iter=%d", args->worker_id, i);
    XFBURN_LOG_INFO ("MT_WORK", "processing worker=%d sector=%d", args->worker_id, i * 16);
    XFBURN_LOG_CALL_EXIT ("mock_burn_op", t0, "ret=1");
  }

  g_free (tname);
  return NULL;
}

static void
test_multithreaded_trace (void)
{
  GError *error = NULL;
  gchar *tmp_dir;
  gchar *log_path;
  gchar *contents = NULL;
  gchar **lines;
  GThread *threads[4];
  WorkerArgs args[4];
  gint i;
  gint call_count = 0;
  gint ret_count = 0;
  gint work_count = 0;
  gboolean ok;

  tmp_dir = g_dir_make_tmp ("xfburn-test-mt-XXXXXX", &error);
  g_assert_no_error (error);
  log_path = g_build_filename (tmp_dir, "mt.log", NULL);

  ok = xfburn_log_init (log_path);
  g_assert_true (ok);

  for (i = 0; i < 4; i++) {
    args[i].worker_id = i;
    args[i].iterations = 25;
    threads[i] = g_thread_new ("test-worker", multithreaded_worker_func, &args[i]);
  }

  for (i = 0; i < 4; i++) {
    g_thread_join (threads[i]);
  }

  xfburn_log_shutdown ();

  ok = g_file_get_contents (log_path, &contents, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ok);

  lines = g_strsplit (contents, "\n", -1);
  for (i = 0; lines[i] != NULL; i++) {
    if (lines[i][0] == '\0') {
      continue;
    }
    if (strstr (lines[i], "[CALL] >>> mock_burn_op(") != NULL) {
      call_count++;
    }
    if (strstr (lines[i], "[RET ] <<< mock_burn_op elapsed_ms=") != NULL) {
      ret_count++;
    }
    if (strstr (lines[i], "[MT_WORK] processing worker=") != NULL) {
      work_count++;
    }
  }

  g_assert_cmpint (call_count, ==, 100);
  g_assert_cmpint (ret_count, ==, 100);
  g_assert_cmpint (work_count, ==, 100);

  g_assert_nonnull (strstr (contents, "[T:worker-0:"));
  g_assert_nonnull (strstr (contents, "[T:worker-1:"));
  g_assert_nonnull (strstr (contents, "[T:worker-2:"));
  g_assert_nonnull (strstr (contents, "[T:worker-3:"));

  g_strfreev (lines);
  g_free (contents);
  g_free (log_path);
  remove_tree_recursive (tmp_dir);
  g_free (tmp_dir);
}

static void
test_backend_message_drain (void)
{
  GError *error = NULL;
  gchar *tmp_dir;
  gchar *log_path;
  gchar *contents = NULL;
  gchar last_err[512];
  gint failures;
  gboolean ok;

  tmp_dir = g_dir_make_tmp ("xfburn-test-backend-XXXXXX", &error);
  g_assert_no_error (error);
  log_path = g_build_filename (tmp_dir, "backend.log", NULL);

  ok = xfburn_log_init (log_path);
  g_assert_true (ok);

  g_assert_true (burn_initialize () != 0);
  g_assert_true (iso_init () >= 0);

  xfburn_log_configure_backends ();

  burn_msgs_submit (0, "Simulated note from libburn", 0, "NOTE", 0);
  burn_msgs_submit (0x52102, "Simulated SCSI sense 5/21/02 Invalid address for write", 5, "FAILURE", 0);
  iso_msgs_submit (0, "Simulated RockRidge warning from libisofs", 0, "WARNING", 0);

  last_err[0] = '\0';
  failures = xfburn_log_drain_backend_messages ("unit_test_ctx", last_err, sizeof (last_err));
  g_assert_cmpint (failures, >=, 1);
  g_assert_nonnull (strstr (last_err, "Simulated SCSI sense 5/21/02 Invalid address for write"));

  xfburn_log_shutdown ();
  iso_finish ();
  burn_finish ();

  ok = g_file_get_contents (log_path, &contents, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ok);

  g_assert_nonnull (strstr (contents, "[LIBBURN] ctx=\"unit_test_ctx\" sev=\"NOTE\""));
  g_assert_nonnull (strstr (contents, "Simulated note from libburn"));
  g_assert_nonnull (strstr (contents, "[LIBBURN] ctx=\"unit_test_ctx\" sev=\"FAILURE\" code=336130 (0x52102) errno=5"));
  g_assert_nonnull (strstr (contents, "Simulated SCSI sense 5/21/02 Invalid address for write"));
  g_assert_nonnull (strstr (contents, "[LIBISOFS] ctx=\"unit_test_ctx\" sev=\"WARNING\""));
  g_assert_nonnull (strstr (contents, "Simulated RockRidge warning from libisofs"));

  g_free (contents);
  g_free (log_path);
  remove_tree_recursive (tmp_dir);
  g_free (tmp_dir);
}

static void
test_sysfs_telemetry (void)
{
  GError *error = NULL;
  gchar *tmp_dir;
  gchar *log_path;
  gchar *mock_sys;
  gchar *usb_dir;
  gchar *scsi_dir;
  gchar *block_dir;
  gchar *symlink_path;
  gchar *contents = NULL;
  XfburnDeviceIoCounters counters;
  gboolean ok;

  tmp_dir = g_dir_make_tmp ("xfburn-test-sysfs-XXXXXX", &error);
  g_assert_no_error (error);

  log_path = g_build_filename (tmp_dir, "sysfs.log", NULL);
  mock_sys = g_build_filename (tmp_dir, "sys", NULL);
  usb_dir = g_build_filename (mock_sys, "devices", "pci0000:00", "usb2", "2-1", NULL);
  scsi_dir = g_build_filename (usb_dir, "host0", "target0:0:0", "0:0:0:0", NULL);
  block_dir = g_build_filename (mock_sys, "class", "block", "sr_mock0", NULL);

  g_mkdir_with_parents (scsi_dir, 0700);
  g_mkdir_with_parents (block_dir, 0700);

  write_text_file (usb_dir, "idVendor", "05e3\n");
  write_text_file (usb_dir, "idProduct", "0718\n");
  write_text_file (usb_dir, "speed", "480\n");
  write_text_file (usb_dir, "version", " 2.00\n");
  write_text_file (usb_dir, "bMaxPower", "500mA\n");
  write_text_file (usb_dir, "manufacturer", "MockBridge\n");
  write_text_file (usb_dir, "product", "USB3-SATA-Bridge\n");
  write_text_file (usb_dir, "power/control", "on\n");
  write_text_file (usb_dir, "power/runtime_status", "active\n");

  write_text_file (scsi_dir, "vendor", "HL-DT-ST\n");
  write_text_file (scsi_dir, "model", "BD-RE BH16NS55  \n");
  write_text_file (scsi_dir, "rev", "1.05\n");
  write_text_file (scsi_dir, "state", "running\n");
  write_text_file (scsi_dir, "timeout", "30\n");
  write_text_file (scsi_dir, "iorequest_cnt", "0x100\n");
  write_text_file (scsi_dir, "iodone_cnt", "0xfd\n");
  write_text_file (scsi_dir, "ioerr_cnt", "0x3\n");

  write_text_file (block_dir, "queue/max_sectors_kb", "128\n");

  symlink_path = g_build_filename (block_dir, "device", NULL);
  g_assert_cmpint (symlink (scsi_dir, symlink_path), ==, 0);

  ok = xfburn_log_init (log_path);
  g_assert_true (ok);

  ok = xfburn_log_read_device_io_counters ("/dev/sr_mock0", mock_sys, &counters);
  g_assert_true (ok);
  g_assert_true (counters.valid);
  g_assert_cmpint (counters.iorequest_cnt, ==, 256);
  g_assert_cmpint (counters.iodone_cnt, ==, 253);
  g_assert_cmpint (counters.ioerr_cnt, ==, 3);
  g_assert_cmpstr (counters.state, ==, "running");

  xfburn_log_dump_device_hardware ("/dev/sr_mock0", mock_sys);
  xfburn_log_shutdown ();

  ok = g_file_get_contents (log_path, &contents, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ok);

  g_assert_nonnull (strstr (contents,
                            "[HW_DEV] dev=\"/dev/sr_mock0\" vendor=\"HL-DT-ST\" model=\"BD-RE BH16NS55\" rev=\"1.05\" state=\"running\" timeout=\"30\" max_sectors_kb=\"128\" iorequest_cnt=256 iodone_cnt=253 ioerr_cnt=3"));
  g_assert_nonnull (strstr (contents,
                            "[HW_USB] dev=\"/dev/sr_mock0\" usb_node=\"2-1\" id=\"05e3:0718\" speed_mbps=\"480\" usb_ver=\"2.00\" max_power=\"500mA\" power_control=\"on\" runtime_status=\"active\" manufacturer=\"MockBridge\" product=\"USB3-SATA-Bridge\""));

  g_free (contents);
  g_free (symlink_path);
  g_free (block_dir);
  g_free (scsi_dir);
  g_free (usb_dir);
  g_free (mock_sys);
  g_free (log_path);
  remove_tree_recursive (tmp_dir);
  g_free (tmp_dir);
}

static void
test_crash_signal_handler (void)
{
  GError *error = NULL;
  gchar *tmp_dir;
  gchar *crash_log_path;
  gchar *contents = NULL;
  pid_t pid;
  int status = 0;
  gboolean ok;

  tmp_dir = g_dir_make_tmp ("xfburn-test-crash-XXXXXX", &error);
  g_assert_no_error (error);
  crash_log_path = g_build_filename (tmp_dir, "crash.log", NULL);

  pid = fork ();
  g_assert_cmpint (pid, >=, 0);

  if (pid == 0) {
    struct rlimit rl;
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    setrlimit (RLIMIT_CORE, &rl);

    if (!xfburn_log_init (crash_log_path)) {
      _exit (2);
    }
    xfburn_log_install_crash_handlers ();
    XFBURN_LOG_INFO ("CRASH_TEST", "About to raise SIGSEGV in child process");
    raise (SIGSEGV);
    _exit (1);
  }

  g_assert_cmpint (waitpid (pid, &status, 0), ==, pid);
  g_assert_true (WIFSIGNALED (status));
  g_assert_cmpint (WTERMSIG (status), ==, SIGSEGV);

  ok = g_file_get_contents (crash_log_path, &contents, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ok);

  g_assert_nonnull (strstr (contents, "[CRASH_TEST] About to raise SIGSEGV in child process"));
  g_assert_nonnull (strstr (contents, "[FATAL]"));
  g_assert_nonnull (strstr (contents, "[SIGNAL] Caught fatal signal 11 (SIGSEGV)"));

  g_free (contents);
  g_free (crash_log_path);
  remove_tree_recursive (tmp_dir);
  g_free (tmp_dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/xfburn/log/init-and-header", test_init_and_header);
  g_test_add_func ("/xfburn/log/multithreaded-trace", test_multithreaded_trace);
  g_test_add_func ("/xfburn/log/backend-message-drain", test_backend_message_drain);
  g_test_add_func ("/xfburn/log/sysfs-telemetry", test_sysfs_telemetry);
  g_test_add_func ("/xfburn/log/crash-signal-handler", test_crash_signal_handler);

  return g_test_run ();
}
