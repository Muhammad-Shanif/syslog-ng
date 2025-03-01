/*
 * Copyright (c) 2023 Attila Szakacs
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include <criterion/criterion.h>
#include "libtest/queue_utils_lib.h"
#include "libtest/grab-logging.h"
#include "test_diskq_tools.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "apphook.h"

#include "logqueue-disk.h"
#include "logqueue-disk-reliable.h"
#include "logqueue-disk-non-reliable.h"

static void
_assert_file_exists(const gchar *filename)
{
  struct stat st;
  cr_assert_neq(stat(filename, &st), -1, "File does not exist: %s", filename);
}

static void
_push_corrupted_msg(LogQueue *queue)
{
  feed_some_messages(queue, 1);

  gint fd = open(log_queue_disk_get_filename(queue), O_RDWR, 0600);
  cr_assert_gt(fd, -1, "Failed to open disk queue file: %s", log_queue_disk_get_filename(queue));
  cr_assert_eq(ftruncate(fd, QDISK_RESERVED_SPACE), 0,
               "Failed to make disk queue file corrupted: %s", log_queue_disk_get_filename(queue));
  close(fd);
}

static void
_pop_msg(LogQueue *queue)
{
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
  LogMessage *msg = log_queue_pop_head(queue, &path_options);

  // If the message was stored in qout in addition to qdisk, it can be popped successfully.
  if (msg)
    log_msg_unref(msg);
}

static void
_assert_log_queue_disk_reliable_is_empty(LogQueue *q)
{
  LogQueueDiskReliable *queue = (LogQueueDiskReliable *) q;

  cr_assert_eq(g_queue_get_length(queue->qout), 0);
  cr_assert_eq(g_queue_get_length(queue->qreliable), 0);
  cr_assert_eq(g_queue_get_length(queue->qbacklog), 0);
  cr_assert_eq(qdisk_get_length(queue->super.qdisk), 0);

  cr_assert(q->memory_usage);
  cr_assert(q->queued_messages);

  cr_assert_eq(stats_counter_get(q->memory_usage), 0);
  cr_assert_eq(stats_counter_get(q->queued_messages), 0);
  cr_assert_eq(atomic_gssize_get_unsigned(&q->stats_cache.memory_usage), 0);
  cr_assert_eq(atomic_gssize_get_unsigned(&q->stats_cache.queued_messages), 0);
}

static void
_register_stats_counters(LogQueue *q)
{
  stats_lock();
  {
    StatsClusterKey sc_key;
    StatsClusterLabel labels[] = {};
    stats_cluster_logpipe_key_set(&sc_key, "dummy-name", labels, G_N_ELEMENTS(labels));
    log_queue_register_stats_counters(q, STATS_LEVEL1, &sc_key);
  }
  stats_unlock();
}

Test(logqueue_disk, restart_corrupted_reliable)
{
  start_grabbing_messages();

  const gchar *filename = "restart_corrupted_reliable.rqf";
  const gchar *corrupted_filename = "restart_corrupted_reliable.rqf.corrupted";
  const gchar *corrupted2_filename = "restart_corrupted_reliable.rqf.corrupted-2";
  const gchar *corrupted3_filename = "restart_corrupted_reliable.rqf.corrupted-3";

  DiskQueueOptions options = {0};
  disk_queue_options_set_default_options(&options);
  disk_queue_options_reliable_set(&options, TRUE);
  disk_queue_options_disk_buf_size_set(&options, MIN_DISK_BUF_SIZE);
  disk_queue_options_mem_buf_size_set(&options, 4096);
  disk_queue_options_qout_size_set(&options, 2);

  LogQueue *queue = log_queue_disk_reliable_new(&options, "restart_corrupted_reliable");
  _register_stats_counters(queue);

  cr_assert(log_queue_disk_start(queue, filename));
  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_reliable_is_empty(queue);

  _push_corrupted_msg(queue);
  _push_corrupted_msg(queue);
  _pop_msg(queue);

  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(corrupted_filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_reliable_is_empty(queue);

  _push_corrupted_msg(queue);
  _push_corrupted_msg(queue);
  _pop_msg(queue);

  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(corrupted_filename);
  _assert_file_exists(corrupted2_filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_reliable_is_empty(queue);

  _push_corrupted_msg(queue);
  _push_corrupted_msg(queue);
  _pop_msg(queue);

  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(corrupted_filename);
  _assert_file_exists(corrupted2_filename);
  _assert_file_exists(corrupted3_filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_reliable_is_empty(queue);

  gboolean persistent;
  log_queue_disk_stop(queue, &persistent);
  log_queue_unref(queue);
  disk_queue_options_destroy(&options);
  unlink(filename);
  unlink(corrupted_filename);
  unlink(corrupted2_filename);
  unlink(corrupted3_filename);

  stop_grabbing_messages();
}

static void
_assert_log_queue_disk_non_reliable_is_empty(LogQueue *q)
{
  LogQueueDiskNonReliable *queue = (LogQueueDiskNonReliable *) q;

  cr_assert_eq(g_queue_get_length(queue->qout), 0);
  cr_assert_eq(g_queue_get_length(queue->qoverflow), 0);
  cr_assert_eq(g_queue_get_length(queue->qbacklog), 0);
  cr_assert_eq(qdisk_get_length(queue->super.qdisk), 0);

  cr_assert(q->memory_usage);
  cr_assert(q->queued_messages);

  cr_assert_eq(stats_counter_get(q->memory_usage), 0);
  cr_assert_eq(stats_counter_get(q->queued_messages), 0);
  cr_assert_eq(atomic_gssize_get_unsigned(&q->stats_cache.memory_usage), 0);
  cr_assert_eq(atomic_gssize_get_unsigned(&q->stats_cache.queued_messages), 0);
}

Test(logqueue_disk, restart_corrupted_non_reliable)
{
  start_grabbing_messages();

  const gchar *filename = "restart_corrupted_non_reliable.qf";
  const gchar *corrupted_filename = "restart_corrupted_non_reliable.qf.corrupted";
  const gchar *corrupted2_filename = "restart_corrupted_non_reliable.qf.corrupted-2";
  const gchar *corrupted3_filename = "restart_corrupted_non_reliable.qf.corrupted-3";

  DiskQueueOptions options = {0};
  disk_queue_options_set_default_options(&options);
  disk_queue_options_reliable_set(&options, FALSE);
  disk_queue_options_disk_buf_size_set(&options, MIN_DISK_BUF_SIZE);
  disk_queue_options_mem_buf_size_set(&options, 4096);
  disk_queue_options_qout_size_set(&options, 0);

  LogQueue *queue = log_queue_disk_non_reliable_new(&options, "restart_corrupted_non_reliable");
  _register_stats_counters(queue);

  cr_assert(log_queue_disk_start(queue, filename));
  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_non_reliable_is_empty(queue);

  _push_corrupted_msg(queue);
  _push_corrupted_msg(queue);
  _pop_msg(queue);

  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(corrupted_filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_non_reliable_is_empty(queue);

  _push_corrupted_msg(queue);
  _push_corrupted_msg(queue);
  _pop_msg(queue);

  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(corrupted_filename);
  _assert_file_exists(corrupted2_filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_non_reliable_is_empty(queue);

  _push_corrupted_msg(queue);
  _push_corrupted_msg(queue);
  _pop_msg(queue);

  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(corrupted_filename);
  _assert_file_exists(corrupted2_filename);
  _assert_file_exists(corrupted3_filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_non_reliable_is_empty(queue);

  gboolean persistent;
  log_queue_disk_stop(queue, &persistent);
  log_queue_unref(queue);
  disk_queue_options_destroy(&options);
  unlink(filename);
  unlink(corrupted_filename);
  unlink(corrupted2_filename);
  unlink(corrupted3_filename);

  stop_grabbing_messages();
}

static void
_assert_log_queue_disk_non_reliable_has_messages_in_qout(LogQueue *q, guint num_of_messages)
{
  LogQueueDiskNonReliable *queue = (LogQueueDiskNonReliable *) q;
  const guint item_number_per_message = 2;

  LogMessage *msg = log_msg_new_empty();
  const gssize log_msg_size = log_msg_get_size(msg);
  log_msg_unref(msg);

  cr_assert_eq(g_queue_get_length(queue->qout), num_of_messages * item_number_per_message);
  cr_assert_eq(g_queue_get_length(queue->qoverflow), 0);
  cr_assert_eq(g_queue_get_length(queue->qbacklog), 0);
  cr_assert_eq(qdisk_get_length(queue->super.qdisk), 0);

  cr_assert(q->memory_usage);
  cr_assert(q->queued_messages);

  cr_assert_eq(stats_counter_get(q->memory_usage), num_of_messages * log_msg_size);
  cr_assert_eq(stats_counter_get(q->queued_messages), num_of_messages);
  cr_assert_eq(atomic_gssize_get_unsigned(&q->stats_cache.memory_usage), num_of_messages * log_msg_size);
  cr_assert_eq(atomic_gssize_get_unsigned(&q->stats_cache.queued_messages), num_of_messages);
}

Test(logqueue_disk, restart_corrupted_non_reliable_with_qout)
{
  start_grabbing_messages();

  const gchar *filename = "restart_corrupted_non_reliable_with_qout.qf";
  const gchar *corrupted_filename = "restart_corrupted_non_reliable_with_qout.qf.corrupted";

  DiskQueueOptions options = {0};
  disk_queue_options_set_default_options(&options);
  disk_queue_options_reliable_set(&options, FALSE);
  disk_queue_options_disk_buf_size_set(&options, MIN_DISK_BUF_SIZE);
  disk_queue_options_mem_buf_size_set(&options, 4096);
  disk_queue_options_qout_size_set(&options, 1);

  LogQueue *queue = log_queue_disk_non_reliable_new(&options, "restart_corrupted_non_reliable_with_qout");
  LogQueueDiskNonReliable *queue_disk_non_reliable = (LogQueueDiskNonReliable *) queue;
  _register_stats_counters(queue);

  cr_assert(log_queue_disk_start(queue, filename));
  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(filename);
  _assert_log_queue_disk_non_reliable_is_empty(queue);

  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
  LogMessage *msg = log_msg_new_empty();
  log_queue_push_tail(queue, msg, &path_options);

  _assert_log_queue_disk_non_reliable_has_messages_in_qout(queue, 1);

  log_queue_disk_restart_corrupted(&queue_disk_non_reliable->super);

  cr_assert_str_eq(log_queue_disk_get_filename(queue), filename);
  _assert_file_exists(corrupted_filename);
  _assert_file_exists(filename);

  _assert_log_queue_disk_non_reliable_has_messages_in_qout(queue, 1);

  gboolean persistent;
  log_queue_disk_stop(queue, &persistent);
  log_queue_unref(queue);
  disk_queue_options_destroy(&options);
  unlink(filename);
  unlink(corrupted_filename);

  stop_grabbing_messages();
}

static void
setup(void)
{
  app_startup();
  configuration = cfg_new_snippet();
  configuration->stats_options.level = STATS_LEVEL1;
  cfg_init(configuration);
}

static void
teardown(void)
{
  cfg_free(configuration);
  app_shutdown();
}

TestSuite(logqueue_disk, .init = setup, .fini = teardown);
