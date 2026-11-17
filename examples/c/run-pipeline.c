/* GStreamer
 * Copyright (C) Ashley Camba Garrido <ashwoods@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/* Run a pipeline the way an application should: check every state change,
 * watch the bus, retry runtime errors, notice a stall and stop with a
 * deadline.
 *
 *     run-pipeline [--retries N] [--stall-timeout S] PIPELINE
 *
 * Exit status: 0 at EOS, 1 on an error, 2 for a pipeline that cannot be
 * built, 3 when the pipeline is stuck and cannot be stopped.
 */

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <gst/gst.h>

#define STOP_TIMEOUT 2          /* seconds */

typedef enum
{
  OUTCOME_EOS,
  OUTCOME_ERROR,
  OUTCOME_STALL
} Outcome;

/* print a line to stdout right away: _exit() and crashes skip stdio flushing */
static void
say (const gchar * format, ...)
{
  va_list args;

  va_start (args, format);
  vprintf (format, args);
  va_end (args);
  putchar ('\n');
  fflush (stdout);
}

/* --8<-- [start:stop] */
typedef struct
{
  GstElement *pipeline;
  GAsyncQueue *result;
} StopRequest;

static gpointer
stop_thread (gpointer data)
{
  StopRequest *request = data;
  GstStateChangeReturn ret;

  ret = gst_element_set_state (request->pipeline, GST_STATE_NULL);
  /* push ret + 1: a NULL pointer cannot go through the queue */
  g_async_queue_push (request->result, GINT_TO_POINTER (ret + 1));
  return NULL;
}

/* Set the pipeline to NULL, giving up after STOP_TIMEOUT seconds.
 *
 * A blocked streaming thread holds its pad's stream lock, and the state
 * change waits for that lock forever, so never wait for it unbounded.
 */
static gboolean
stop (GstElement * pipeline)
{
  StopRequest request = { pipeline, g_async_queue_new () };
  GThread *thread;
  gpointer result;

  thread = g_thread_new ("stop", stop_thread, &request);
  result = g_async_queue_timeout_pop (request.result,
      STOP_TIMEOUT * G_USEC_PER_SEC);
  if (result == NULL) {
    say ("error: the pipeline did not stop within %d s", STOP_TIMEOUT);
    /* the stuck thread cannot be freed: leave it to the supervisor */
    _exit (3);
  }
  g_thread_join (thread);
  g_async_queue_unref (request.result);
  if (GPOINTER_TO_INT (result) - 1 == GST_STATE_CHANGE_FAILURE) {
    say ("error: could not stop the pipeline");
    return FALSE;
  }
  return TRUE;
}

/* --8<-- [end:stop] */

/* --8<-- [start:bus] */
/* Play until EOS, an error or a stall. */
static Outcome
play (GstElement * pipeline, gint stall_timeout)
{
  GstBus *bus = gst_element_get_bus (pipeline);
  gint64 last_position = -1;
  gboolean have_last = FALSE;
  gint idle = 0;
  Outcome outcome;

  while (TRUE) {
    GstMessage *msg = gst_bus_timed_pop_filtered (bus, GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_WARNING);

    if (msg == NULL) {
      /* no message for a second: is data still flowing? */
      gint64 position = -1;

      gst_element_query_position (pipeline, GST_FORMAT_TIME, &position);
      idle = (have_last && position == last_position) ? idle + 1 : 0;
      last_position = position;
      have_last = TRUE;
      if (idle >= stall_timeout) {
        say ("stalled: no data for %d s", stall_timeout);
        outcome = OUTCOME_STALL;
        break;
      }
    } else if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS) {
      say ("eos");
      gst_message_unref (msg);
      outcome = OUTCOME_EOS;
      break;
    } else if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_WARNING) {
      GError *err = NULL;
      gchar *debug = NULL;
      gchar *path = gst_object_get_path_string (GST_MESSAGE_SRC (msg));

      gst_message_parse_warning (msg, &err, &debug);
      say ("warning: from %s: %s", path, err->message);
      g_free (path);
      g_free (debug);
      g_error_free (err);
      gst_message_unref (msg);
    } else {
      /* the first error is the cause; the ones after it are its echo */
      GError *err = NULL;
      gchar *debug = NULL;
      gchar *path = gst_object_get_path_string (GST_MESSAGE_SRC (msg));

      gst_message_parse_error (msg, &err, &debug);
      say ("error: from %s: %s", path, err->message);
      say ("debug: %s", debug ? debug : "None");
      g_free (path);
      g_free (debug);
      g_error_free (err);
      gst_message_unref (msg);
      outcome = OUTCOME_ERROR;
      break;
    }
  }
  gst_object_unref (bus);
  return outcome;
}

/* --8<-- [end:bus] */

/* --8<-- [start:retry] */
static int
run (GstElement * pipeline, gint retries, gint stall_timeout)
{
  gint attempt;

  for (attempt = 0; attempt <= retries; attempt++) {
    Outcome outcome;

    if (attempt)
      say ("retrying (%d/%d)", attempt, retries);
    if (gst_element_set_state (pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
      /* an element refused to start: retrying will not help. It may
       * have posted why on the bus, or not */
      GstBus *bus = gst_element_get_bus (pipeline);
      GstMessage *msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);

      say ("error: could not start the pipeline");
      if (msg) {
        GError *err = NULL;
        gchar *debug = NULL;
        gchar *path = gst_object_get_path_string (GST_MESSAGE_SRC (msg));

        gst_message_parse_error (msg, &err, &debug);
        say ("error: from %s: %s", path, err->message);
        g_free (path);
        g_free (debug);
        g_error_free (err);
        gst_message_unref (msg);
      }
      gst_object_unref (bus);
      stop (pipeline);
      return 1;
    }
    outcome = play (pipeline, stall_timeout);
    if (!stop (pipeline))
      return 1;
    if (outcome == OUTCOME_EOS)
      return 0;
  }
  return 1;
}

/* --8<-- [end:retry] */

int
main (int argc, char *argv[])
{
  gint retries = 0;
  gint stall_timeout = 5;
  GOptionEntry entries[] = {
    {"retries", 0, 0, G_OPTION_ARG_INT, &retries, NULL, "N"},
    {"stall-timeout", 0, 0, G_OPTION_ARG_INT, &stall_timeout, NULL, "S"},
    {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
  };
  GOptionContext *context;
  GError *error = NULL;
  GstElement *pipeline;
  int status;

  context = g_option_context_new ("PIPELINE");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error) || argc != 2) {
    g_printerr ("%s\n", error ? error->message : "expected one PIPELINE");
    g_clear_error (&error);
    g_option_context_free (context);
    return 2;
  }
  g_option_context_free (context);

  gst_init (NULL, NULL);
  /* --8<-- [start:build] */
  pipeline = gst_parse_launch (argv[1], &error);
  if (error != NULL) {
    say ("error: could not build the pipeline: %s", error->message);
    g_error_free (error);
    gst_clear_object (&pipeline);
    return 2;
  }
  /* --8<-- [end:build] */

  status = run (pipeline, retries, stall_timeout);
  gst_object_unref (pipeline);
  return status;
}
