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

/* Play a pipeline twice and report the GStreamer objects the second run
 * left alive, with the leaks tracer.
 *
 *     GST_TRACERS=leaks leak-check PIPELINE
 *
 * Exit status: 0 when nothing leaked, 1 when something did, 2 without the
 * leaks tracer.
 */

#include <stdarg.h>
#include <stdio.h>

#include <gst/gst.h>

/* print a line to stdout right away */
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

static void
play (GstElement * pipeline)
{
  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg;

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_message_unref (msg);
  gst_object_unref (bus);
}

/* --8<-- [start:tracer] */
/* The tracer exists only when GST_TRACERS names it before gst_init(). */
static GstTracer *
find_leaks_tracer (void)
{
  GList *tracers = gst_tracing_get_active_tracers ();
  GstTracer *leaks = NULL;
  GList *l;

  for (l = tracers; l != NULL; l = l->next) {
    if (g_str_equal (G_OBJECT_TYPE_NAME (l->data), "GstLeaksTracer")) {
      leaks = gst_object_ref (l->data);
      break;
    }
  }
  g_list_free_full (tracers, gst_object_unref);
  return leaks;
}

/* --8<-- [end:tracer] */

/* Add one to the count of every type in a checkpoint's list of objects, or
 * subtract one with step -1. */
static void
count_types (GHashTable * alive, const GstStructure * checkpoint,
    const gchar * field, gint step)
{
  const GValue *list = gst_structure_get_value (checkpoint, field);
  guint i;

  for (i = 0; i < gst_value_list_get_size (list); i++) {
    const GstStructure *object =
        gst_value_get_structure (gst_value_list_get_value (list, i));
    const gchar *name = gst_structure_get_string (object, "type-name");
    gint n = GPOINTER_TO_INT (g_hash_table_lookup (alive, name));

    /* the names belong to the checkpoint, which outlives the table */
    g_hash_table_insert (alive, (gpointer) name, GINT_TO_POINTER (n + step));
  }
}

/* --8<-- [start:checkpoint] */
/* Play the pipeline twice and print the objects the second run left alive.
 * Return how many types leaked. */
static guint
check_leaks (GstTracer * leaks, GstElement * pipeline)
{
  GstStructure *checkpoint = NULL;
  GHashTable *alive = g_hash_table_new (g_str_hash, g_str_equal);
  GList *names, *l;
  guint leaked = 0;

  /* the first run creates the objects GStreamer keeps for good (the
   * system clock, caps and pad templates): leave them out */
  play (pipeline);
  g_signal_emit_by_name (leaks, "activity-start-tracking");
  play (pipeline);
  g_signal_emit_by_name (leaks, "activity-get-checkpoint", &checkpoint);
  g_signal_emit_by_name (leaks, "activity-stop-tracking");

  /* alive = created since tracking started, minus removed since */
  count_types (alive, checkpoint, "objects-created-list", 1);
  count_types (alive, checkpoint, "objects-removed-list", -1);
  names = g_list_sort (g_hash_table_get_keys (alive), (GCompareFunc) g_strcmp0);
  for (l = names; l != NULL; l = l->next) {
    gint n = GPOINTER_TO_INT (g_hash_table_lookup (alive, l->data));

    if (n > 0) {
      say ("leaked: %d %s", n, (const gchar *) l->data);
      leaked++;
    }
  }
  g_list_free (names);
  g_hash_table_unref (alive);
  gst_structure_free (checkpoint);
  return leaked;
}

/* --8<-- [end:checkpoint] */

int
main (int argc, char *argv[])
{
  GstTracer *leaks;
  GstElement *pipeline;
  GError *error = NULL;
  guint leaked;

  if (argc != 2) {
    g_printerr ("usage: %s PIPELINE\n", argv[0]);
    return 2;
  }
  gst_init (NULL, NULL);
  leaks = find_leaks_tracer ();
  if (leaks == NULL) {
    say ("error: no leaks tracer: run with GST_TRACERS=leaks");
    return 2;
  }
  pipeline = gst_parse_launch (argv[1], &error);
  if (error != NULL) {
    g_printerr ("error: could not build the pipeline: %s\n", error->message);
    g_error_free (error);
    gst_clear_object (&pipeline);
    gst_object_unref (leaks);
    return 2;
  }

  leaked = check_leaks (leaks, pipeline);
  if (!leaked)
    say ("no leaks");
  gst_object_unref (pipeline);
  gst_object_unref (leaks);
  return leaked ? 1 : 0;
}
