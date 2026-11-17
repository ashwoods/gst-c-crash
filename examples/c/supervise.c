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

/* Run a pipeline in a child process, so a crash kills the child and not the
 * application, and restart it when it dies.
 *
 *     supervise [--restarts N] PIPELINE
 *
 * Exit status: 0 when the child reaches EOS, 1 when it fails or crashes once
 * more than --restarts allows.
 */

#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gst/gst.h>

static const struct
{
  int number;
  const char *name;
} fatal_signals[] = {
  {SIGSEGV, "SIGSEGV"},
  {SIGBUS, "SIGBUS"},
  {SIGABRT, "SIGABRT"},
  {SIGFPE, "SIGFPE"},
  {SIGILL, "SIGILL"},
};

static const struct
{
  int number;
  const char *name;
} other_signals[] = {
  {SIGHUP, "SIGHUP"},
  {SIGINT, "SIGINT"},
  {SIGQUIT, "SIGQUIT"},
  {SIGTRAP, "SIGTRAP"},
  {SIGKILL, "SIGKILL"},
  {SIGPIPE, "SIGPIPE"},
  {SIGALRM, "SIGALRM"},
  {SIGTERM, "SIGTERM"},
  {SIGUSR1, "SIGUSR1"},
  {SIGUSR2, "SIGUSR2"},
};

/* print a line to stdout right away: crashes skip stdio flushing */
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

/* Print a line naming the signal and the stack to stderr, then die of the
 * signal as before.
 *
 * A signal can arrive while the program is anywhere, for example inside
 * malloc() with its lock held, so the handler may only call functions that
 * POSIX lists as async-signal-safe: no printf(), no malloc(), no GLib, no
 * GStreamer. It builds nothing: write() on string literals and on names
 * from a static table.
 *
 * backtrace() is not formally async-signal-safe either: the first call may
 * load libgcc and allocate. child() calls it once before the handler is
 * installed, so the handler only walks the stack, which works in practice
 * and is the best a handler this small can do. Crash reporters (Breakpad,
 * Crashpad, systemd-coredump) do not unwind in the crashed process at all:
 * a handler only copies the registers, or the kernel writes a core file,
 * and a healthy process reads the memory and symbolizes it.
 */
static void
fatal_signal_handler (int sig)
{
  void *frames[64];
  const char *name = NULL;
  size_t i;
  int depth;

  for (i = 0; i < sizeof (fatal_signals) / sizeof (fatal_signals[0]); i++) {
    if (fatal_signals[i].number == sig)
      name = fatal_signals[i].name;
  }
  if (name != NULL) {
    if (write (STDERR_FILENO, "fatal signal ", 13) < 0 ||
        write (STDERR_FILENO, name, strlen (name)) < 0 ||
        write (STDERR_FILENO, "\n", 1) < 0) {
      /* nothing to do: stderr is gone */
    }
  }
  depth = backtrace (frames, 64);
  backtrace_symbols_fd (frames, depth, STDERR_FILENO);
  /* restore the default action (SA_RESETHAND should have, but on macOS the
   * handler ran again and again without this): raise() delivers the signal
   * once this handler returns, so the parent sees it */
  signal (sig, SIG_DFL);
  raise (sig);
}

/* --8<-- [start:child] */
/* Play the pipeline until EOS (exit 0) or an error (exit 1). */
static int
child (const gchar * description)
{
  struct sigaction action;
  void *frames[1];
  GError *error = NULL;
  GstElement *pipeline;
  GstBus *bus;
  GstMessage *msg;
  int status;
  size_t i;

  /* on SIGSEGV, SIGBUS, SIGABRT, SIGFPE or SIGILL, print the stack to
   * stderr, then die of the signal as before */
  backtrace (frames, 1);        /* load what backtrace() needs, now */
  memset (&action, 0, sizeof (action));
  action.sa_handler = fatal_signal_handler;
  action.sa_flags = SA_RESETHAND;
  sigemptyset (&action.sa_mask);
  for (i = 0; i < sizeof (fatal_signals) / sizeof (fatal_signals[0]); i++)
    sigaction (fatal_signals[i].number, &action, NULL);

  gst_init (NULL, NULL);
  pipeline = gst_parse_launch (description, &error);
  if (error != NULL) {
    g_printerr ("error: could not build the pipeline: %s\n", error->message);
    g_error_free (error);
    gst_clear_object (&pipeline);
    return 1;
  }
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  bus = gst_element_get_bus (pipeline);
  msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  status = GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS ? 0 : 1;
  gst_message_unref (msg);
  gst_object_unref (bus);
  gst_object_unref (pipeline);
  return status;
}

/* --8<-- [end:child] */

/* The name of a signal, as Python's signal.Signals gives it. */
static gchar *
signal_name (int sig)
{
  size_t i;

  for (i = 0; i < sizeof (fatal_signals) / sizeof (fatal_signals[0]); i++) {
    if (fatal_signals[i].number == sig)
      return g_strdup (fatal_signals[i].name);
  }
  for (i = 0; i < sizeof (other_signals) / sizeof (other_signals[0]); i++) {
    if (other_signals[i].number == sig)
      return g_strdup (other_signals[i].name);
  }
  return g_strdup_printf ("signal %d", sig);
}

/* --8<-- [start:supervise] */
static int
supervise (const gchar * self, const gchar * description, gint restarts)
{
  gchar *argv[] = { (gchar *) self, "--child", (gchar *) description, NULL };
  gint attempt;

  for (attempt = 0; attempt <= restarts; attempt++) {
    GError *error = NULL;
    gint wait_status;

    if (attempt)
      say ("restarting (%d/%d)", attempt, restarts);
    /* the child shares this process's stdout and stderr */
    if (!g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
            NULL, NULL, &wait_status, &error)) {
      g_printerr ("error: could not run %s: %s\n", self, error->message);
      g_error_free (error);
      return 1;
    }
    if (WIFEXITED (wait_status) && WEXITSTATUS (wait_status) == 0) {
      say ("eos");
      return 0;
    }
    if (WIFSIGNALED (wait_status)) {
      /* killed by a signal: here is where a crash reporter would pick
       * up the core dump or minidump */
      gchar *name = signal_name (WTERMSIG (wait_status));

      say ("crashed: %s", name);
      g_free (name);
    } else {
      say ("failed: exit %d", WEXITSTATUS (wait_status));
    }
  }
  return 1;
}

/* --8<-- [end:supervise] */

int
main (int argc, char *argv[])
{
  gint restarts = 0;
  gboolean is_child = FALSE;
  GOptionEntry entries[] = {
    {"restarts", 0, 0, G_OPTION_ARG_INT, &restarts, NULL, "N"},
    {"child", 0, 0, G_OPTION_ARG_NONE, &is_child, NULL, NULL},
    {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
  };
  GOptionContext *context;
  GError *error = NULL;
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

  if (is_child)
    status = child (argv[1]);
  else
    status = supervise (argv[0], argv[1], restarts);
  return status;
}
