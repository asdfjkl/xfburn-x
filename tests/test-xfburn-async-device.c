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
#include <gio/gio.h>
#include <libburn.h>

#include "xfburn-device.h"
#include "xfburn-udev-manager.h"

static GMutex test_log_mutex;
static const gchar *expected_warning_substr = NULL;
static gint expected_warning_hits = 0;

static GLogWriterOutput
test_log_writer (GLogLevelFlags log_level,
                 const GLogField *fields,
                 gsize n_fields,
                 gpointer user_data)
{
  if ((log_level & G_LOG_LEVEL_WARNING) != 0) {
    const gchar *msg = NULL;
    gsize i;

    for (i = 0; i < n_fields; i++) {
      if (g_strcmp0 (fields[i].key, "MESSAGE") == 0 && fields[i].value != NULL) {
        msg = (const gchar *) fields[i].value;
        break;
      }
    }

    g_mutex_lock (&test_log_mutex);
    if (expected_warning_substr != NULL && msg != NULL &&
        strstr (msg, expected_warning_substr) != NULL) {
      expected_warning_hits++;
      g_mutex_unlock (&test_log_mutex);
      return G_LOG_WRITER_HANDLED;
    }
    g_mutex_unlock (&test_log_mutex);
  }

  return g_log_writer_default (log_level, fields, n_fields, user_data);
}

static void
expect_warning_begin (const gchar *substr)
{
  g_mutex_lock (&test_log_mutex);
  expected_warning_substr = substr;
  expected_warning_hits = 0;
  g_mutex_unlock (&test_log_mutex);
}

static void
expect_warning_end (void)
{
  gint hits;

  g_mutex_lock (&test_log_mutex);
  hits = expected_warning_hits;
  expected_warning_substr = NULL;
  expected_warning_hits = 0;
  g_mutex_unlock (&test_log_mutex);

  g_assert_cmpint (hits, >=, 1);
}

static void
test_probe_result_apply (void)
{
  XfburnDevice *device = xfburn_device_new ();
  XfburnDeviceProbeResult res;
  gint disc_status = 0;
  gint profile_no = 0;
  gchar *profile_name = NULL;
  gboolean erasable = FALSE;
  gint tao_types = 0;
  gint sao_types = 0;
  GSList *speeds = NULL;

  xfburn_device_set_name (device, "Test", "Burner");
  g_object_set (G_OBJECT (device),
                "address", "/dev/sr99",
                "revision", "1.00",
                NULL);

  g_assert_false (xfburn_device_details_known (device));
  g_assert_false (xfburn_device_last_umount_failed (device));

  /* Populate a successful probe result */
  memset (&res, 0, sizeof (res));
  res.grabbed = TRUE;
  res.cancelled = FALSE;
  res.umount_failed = FALSE;
  res.disc_status = BURN_DISC_BLANK;
  res.profile_no = XFBURN_PROFILE_CDR;
  g_strlcpy (res.profile_name, "CD-R", sizeof (res.profile_name));
  res.is_erasable = FALSE;
  res.supported_speeds = g_slist_append (NULL, GINT_TO_POINTER (1764));
  res.supported_speeds = g_slist_append (res.supported_speeds, GINT_TO_POINTER (4233));
  res.has_libburn_info = TRUE;
  res.buffer_size = 2048;
  res.dummy_write = TRUE;
  res.cdr = TRUE;
  res.cdrw = TRUE;
  res.dvdr = TRUE;
  res.tao_block_types = BURN_BLOCK_MODE1;
  res.sao_block_types = BURN_BLOCK_SAO;

  xfburn_device_apply_probe_result (device, &res);

  /* Ownership of supported_speeds was transferred to device */
  g_assert_null (res.supported_speeds);
  xfburn_device_probe_result_clear (&res);

  g_object_get (G_OBJECT (device),
                "disc-status", &disc_status,
                "profile-no", &profile_no,
                "profile-name", &profile_name,
                "erasable", &erasable,
                "supported-speeds", &speeds,
                "tao-block-types", &tao_types,
                "sao-block-types", &sao_types,
                NULL);

  g_assert_cmpint (disc_status, ==, BURN_DISC_BLANK);
  g_assert_cmpint (profile_no, ==, XFBURN_PROFILE_CDR);
  g_assert_cmpstr (profile_name, ==, "CD-R");
  g_assert_false (erasable);
  g_assert_cmpuint (g_slist_length (speeds), ==, 2);
  g_assert_cmpint (GPOINTER_TO_INT (speeds->data), ==, 1764);
  g_assert_cmpint (GPOINTER_TO_INT (speeds->next->data), ==, 4233);
  g_assert_cmpint (tao_types, ==, BURN_BLOCK_MODE1);
  g_assert_cmpint (sao_types, ==, BURN_BLOCK_SAO);
  g_assert_true (xfburn_device_details_known (device));
  g_assert_true (xfburn_device_can_dummy_write (device));
  g_assert_true (xfburn_device_can_burn (device));
  g_assert_false (xfburn_device_last_umount_failed (device));
  g_free (profile_name);

  /* Now simulate a failed probe caused by a busy mounted disc */
  memset (&res, 0, sizeof (res));
  res.grabbed = FALSE;
  res.cancelled = FALSE;
  res.umount_failed = TRUE;
  res.disc_status = BURN_DISC_UNGRABBED;
  res.profile_no = XFBURN_PROFILE_NONE;
  res.profile_name[0] = '\0';
  res.has_libburn_info = FALSE;

  xfburn_device_apply_probe_result (device, &res);
  xfburn_device_probe_result_clear (&res);

  g_object_get (G_OBJECT (device),
                "disc-status", &disc_status,
                "profile-no", &profile_no,
                "supported-speeds", &speeds,
                NULL);

  g_assert_cmpint (disc_status, ==, BURN_DISC_UNGRABBED);
  g_assert_cmpint (profile_no, ==, XFBURN_PROFILE_NONE);
  g_assert_null (speeds);
  g_assert_true (xfburn_device_last_umount_failed (device));
  /* Previously learned drive capabilities remain known */
  g_assert_true (xfburn_device_details_known (device));
  g_assert_true (xfburn_device_can_dummy_write (device));

  g_object_unref (device);
}

#ifdef HAVE_GUDEV
static void
test_uevent_cooldown (void)
{
  gint64 now_us;

  xfburn_udev_manager_note_probe_release ();
  now_us = g_get_monotonic_time ();

  /* Synthetic close-write change events without media change are ignored within cooldown */
  g_assert_true (xfburn_udev_manager_should_ignore_uevent ("change", "0", now_us + 50 * 1000));
  g_assert_true (xfburn_udev_manager_should_ignore_uevent ("change", NULL, now_us + 500 * 1000));

  /* Real media insertion/ejection and add/remove events are never ignored */
  g_assert_false (xfburn_udev_manager_should_ignore_uevent ("change", "1", now_us + 50 * 1000));
  g_assert_false (xfburn_udev_manager_should_ignore_uevent ("add", "0", now_us + 50 * 1000));
  g_assert_false (xfburn_udev_manager_should_ignore_uevent ("remove", "0", now_us + 50 * 1000));

  /* After the 2-second cooldown expires, change events are delivered again */
  g_assert_false (xfburn_udev_manager_should_ignore_uevent ("change", "0", now_us + 3 * G_USEC_PER_SEC));
}

static void
test_kernel_mount_check (void)
{
  g_assert_false (xfburn_udev_manager_is_mounted_in_kernel (NULL));
  g_assert_false (xfburn_udev_manager_is_mounted_in_kernel (""));
  g_assert_false (xfburn_udev_manager_is_mounted_in_kernel ("/dev/nonexistent-xfburn-test-sr99"));
}

typedef struct {
  XfburnUdevManager *udevman;
  GMainLoop *loop;
  GThread *main_thread;
  XfburnUmountResult umount_result;
  gchar *error_msg;
  gboolean worker_done;
  gboolean volume_signal_received;
  gboolean volume_signal_on_main_thread;
  gint timer_ticks;
} AsyncUmountTestCtx;

static gboolean
cb_quit_loop_idle (gpointer user_data)
{
  AsyncUmountTestCtx *ctx = (AsyncUmountTestCtx *) user_data;
  if (ctx->worker_done && ctx->volume_signal_received) {
    g_main_loop_quit (ctx->loop);
  }
  return G_SOURCE_REMOVE;
}

static void
cb_test_volume_changed (XfburnUdevManager *udevman, gpointer user_data)
{
  AsyncUmountTestCtx *ctx = (AsyncUmountTestCtx *) user_data;
  ctx->volume_signal_received = TRUE;
  ctx->volume_signal_on_main_thread = (g_thread_self () == ctx->main_thread);
  if (ctx->worker_done) {
    g_main_loop_quit (ctx->loop);
  }
}

static gboolean
cb_responsiveness_tick (gpointer user_data)
{
  AsyncUmountTestCtx *ctx = (AsyncUmountTestCtx *) user_data;
  ctx->timer_ticks++;
  return G_SOURCE_CONTINUE;
}

static gboolean
cb_safety_timeout (gpointer user_data)
{
  AsyncUmountTestCtx *ctx = (AsyncUmountTestCtx *) user_data;
  g_main_loop_quit (ctx->loop);
  return G_SOURCE_REMOVE;
}

static gpointer
worker_unmount_thread (gpointer user_data)
{
  AsyncUmountTestCtx *ctx = (AsyncUmountTestCtx *) user_data;
  GCancellable *cancellable = g_cancellable_new ();

  /* Allow at least one responsiveness tick on main loop while worker is active */
  g_usleep (30 * 1000);

  ctx->umount_result = xfburn_udev_manager_unmount_device (ctx->udevman,
                                                           "/dev/nonexistent-xfburn-test-sr99",
                                                           cancellable,
                                                           &ctx->error_msg);
  g_object_unref (cancellable);

  /* Also verify worker-thread volume-changed marshals onto main thread */
  xfburn_udev_manager_send_volume_changed ();

  ctx->worker_done = TRUE;
  g_idle_add (cb_quit_loop_idle, ctx);
  return NULL;
}

static void
test_unmount_never_blocks_main_thread (void)
{
  gchar *err = NULL;
  XfburnUdevManager *udevman;
  GCancellable *cancelled;
  XfburnUmountResult res;
  AsyncUmountTestCtx ctx;
  GThread *worker;
  guint tick_id;
  guint safety_id;
  gulong sig_id;
  gint64 t_start;

  err = xfburn_udev_manager_create_global ();
  if (err != NULL) {
    /* In minimal containers without udev, skip cleanly */
    g_test_skip (err);
    g_free (err);
    return;
  }

  udevman = xfburn_udev_manager_ref_global ();
  g_assert_nonnull (udevman);

  /* Pre-cancelled cancellable returns immediately even on main thread */
  cancelled = g_cancellable_new ();
  g_cancellable_cancel (cancelled);
  res = xfburn_udev_manager_unmount_device (udevman, "/dev/nonexistent-xfburn-test-sr99", cancelled, NULL);
  g_assert_cmpint (res, ==, XFBURN_UMOUNT_CANCELLED);
  g_object_unref (cancelled);

  /* Calling unmount synchronously on the main thread refuses to block or spin nested loop */
  expect_warning_begin ("xfburn_udev_manager_unmount_device called on main thread");
  t_start = g_get_monotonic_time ();
  res = xfburn_udev_manager_unmount_device (udevman, "/dev/nonexistent-xfburn-test-sr99", NULL, NULL);
  expect_warning_end ();
  g_assert_cmpint (res, ==, XFBURN_UMOUNT_FAILED);
  g_assert_cmpint (g_get_monotonic_time () - t_start, <, 500 * 1000);

  /* Calling unmount from a worker thread dispatches to main context while keeping loop responsive */
  memset (&ctx, 0, sizeof (ctx));
  ctx.udevman = udevman;
  ctx.loop = g_main_loop_new (NULL, FALSE);
  ctx.main_thread = g_thread_self ();
  ctx.umount_result = XFBURN_UMOUNT_FAILED;

  sig_id = g_signal_connect (udevman, "volume-changed", G_CALLBACK (cb_test_volume_changed), &ctx);
  tick_id = g_timeout_add (10, cb_responsiveness_tick, &ctx);
  safety_id = g_timeout_add_seconds (5, cb_safety_timeout, &ctx);

  worker = g_thread_new ("test_unmount_worker", worker_unmount_thread, &ctx);
  g_main_loop_run (ctx.loop);
  g_thread_join (worker);

  g_source_remove (tick_id);
  g_source_remove (safety_id);
  g_signal_handler_disconnect (udevman, sig_id);
  g_main_loop_unref (ctx.loop);

  g_assert_true (ctx.worker_done);
  g_assert_cmpint (ctx.umount_result, ==, XFBURN_UMOUNT_NOT_MOUNTED);
  g_assert_null (ctx.error_msg);
  g_assert_true (ctx.volume_signal_received);
  g_assert_true (ctx.volume_signal_on_main_thread);
  g_assert_cmpint (ctx.timer_ticks, >=, 1);

  g_object_unref (udevman);
  xfburn_udev_manager_shutdown ();
  g_assert_null (xfburn_udev_manager_ref_global ());
}
#endif

typedef struct {
  const gchar *addr;
  GCancellable *cancellable;
  gboolean ret;
  XfburnDeviceProbeResult result;
} ProbeThreadCtx;

static gpointer
worker_probe_thread (gpointer user_data)
{
  ProbeThreadCtx *ctx = (ProbeThreadCtx *) user_data;

  ctx->ret = xfburn_device_probe_addr (ctx->addr,
                                       "Test Burner",
                                       TRUE,
                                       FALSE,
                                       ctx->cancellable,
                                       &ctx->result);
  return NULL;
}

static void
test_probe_cancelled_and_invalid_fast (void)
{
  ProbeThreadCtx ctx;
  GThread *worker;
  gint64 t_start;

  g_assert_true (burn_initialize ());

  /* 1. Pre-cancelled probe returns immediately without blocking or leaking grab state */
  memset (&ctx, 0, sizeof (ctx));
  ctx.addr = "/dev/nonexistent-xfburn-test-sr99";
  ctx.cancellable = g_cancellable_new ();
  g_cancellable_cancel (ctx.cancellable);

  t_start = g_get_monotonic_time ();
  worker = g_thread_new ("test_probe_cancel", worker_probe_thread, &ctx);
  g_thread_join (worker);

  g_assert_false (ctx.ret);
  g_assert_true (ctx.result.cancelled);
  g_assert_false (ctx.result.grabbed);
  g_assert_cmpint (g_get_monotonic_time () - t_start, <, 500 * 1000);
  g_assert_false (xfburn_device_is_busy ());
  g_assert_false (xfburn_device_is_burning ());

  xfburn_device_probe_result_clear (&ctx.result);
  g_object_unref (ctx.cancellable);

  /* 2. Non-existent device path fails cleanly and releases grab slot */
  memset (&ctx, 0, sizeof (ctx));
  ctx.addr = "/dev/nonexistent-xfburn-test-sr99";
  ctx.cancellable = g_cancellable_new ();

  expect_warning_begin ("Device address does not lead to a burner");
  worker = g_thread_new ("test_probe_invalid", worker_probe_thread, &ctx);
  g_thread_join (worker);
  expect_warning_end ();

  g_assert_false (ctx.ret);
  g_assert_false (ctx.result.cancelled);
  g_assert_false (ctx.result.grabbed);
  g_assert_cmpint (ctx.result.disc_status, ==, BURN_DISC_UNGRABBED);
  g_assert_false (xfburn_device_is_busy ());
  g_assert_false (xfburn_device_is_burning ());

  xfburn_device_probe_result_clear (&ctx.result);
  g_object_unref (ctx.cancellable);

  burn_finish ();
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_log_set_always_fatal (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);
  g_log_set_writer_func (test_log_writer, NULL, NULL);

  g_test_add_func ("/xfburn/async-device/probe-result-apply", test_probe_result_apply);
#ifdef HAVE_GUDEV
  g_test_add_func ("/xfburn/async-device/uevent-cooldown", test_uevent_cooldown);
  g_test_add_func ("/xfburn/async-device/kernel-mount-check", test_kernel_mount_check);
  g_test_add_func ("/xfburn/async-device/unmount-never-blocks-main-thread", test_unmount_never_blocks_main_thread);
#endif
  g_test_add_func ("/xfburn/async-device/probe-cancelled-and-invalid-fast", test_probe_cancelled_and_invalid_fast);

  return g_test_run ();
}
