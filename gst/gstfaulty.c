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
/**
 * SECTION:element-faulty
 * @title: faulty
 * @short_description: crash or fail a pipeline on purpose
 *
 * faulty passes buffers through untouched until #GstFaulty:num-buffers
 * buffers have gone by, then fails the way #GstFaulty:method says: it raises
 * a signal, fails a state change, returns a flow error, corrupts memory or
 * blocks the streaming thread: for #GstFaulty:stall-time, endlessly, or in a
 * deadlock.
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 videotestsrc ! faulty num-buffers=5 method=sigabrt ! fakesink
 * ]|
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/base.h>
#include "gstfaulty.h"

#define DEFAULT_NUM_BUFFERS     100
#define DEFAULT_METHOD          FAULTY_SIGSEGV
#define DEFAULT_STALL_TIME      2000

#define GST_TYPE_FAULTY_METHOD (gst_faulty_method_get_type())
static GType
gst_faulty_method_get_type (void)
{
  static gsize faulty_method_type = 0;
  static const GEnumValue faulty_method[] = {
    {FAULTY_SIGSEGV, "Raise SIGSEGV", "sigsegv"},
    {FAULTY_SIGBUS, "Raise SIGBUS", "sigbus"},
    {FAULTY_SIGABRT, "Raise SIGABRT", "sigabrt"},
    {FAULTY_START_ERROR, "Fail start()", "start"},
    {FAULTY_STOP_ERROR, "Fail the READY to NULL state change", "stop"},
    {FAULTY_CUSTOM_METHOD, "Write to a null pointer", "custom"},
    {FAULTY_GST_ERROR, "Post an element error", "gst"},
    {FAULTY_FLOW_ERROR,
        "Return GST_FLOW_ERROR without posting an error", "flow-error"},
    {FAULTY_HANG, "Block the streaming thread forever", "hang"},
    {FAULTY_UNREF, "Unref the buffer once too often", "unref"},
    {FAULTY_DOUBLE_FREE, "Free a malloc() block twice", "double-free"},
    {FAULTY_USE_AFTER_FREE, "Write to a freed malloc() block",
        "use-after-free"},
    {FAULTY_STALL, "Sleep for stall-time once, then pass buffers again",
        "stall"},
    {FAULTY_SPIN, "Busy-loop for stall-time once, then pass buffers again",
        "spin"},
    {FAULTY_DEADLOCK,
        "Deadlock the streaming thread with a helper thread", "deadlock"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&faulty_method_type)) {
    GType type = g_enum_register_static ("FaultyMethod", faulty_method);
    g_once_init_leave (&faulty_method_type, type);
  }
  return faulty_method_type;
}

enum
{
  PROP_0,
  PROP_NUM_BUFFERS,
  PROP_METHOD,
  PROP_STALL_TIME,
};

static void
faulty_signal_sigsegv (GstBuffer * buf G_GNUC_UNUSED)
{
  raise (SIGSEGV);
}

static void
faulty_signal_sigbus (GstBuffer * buf G_GNUC_UNUSED)
{
  raise (SIGBUS);
}

static void
faulty_signal_sigabrt (GstBuffer * buf G_GNUC_UNUSED)
{
  raise (SIGABRT);
}

static void
faulty_custom_method (GstBuffer * buf G_GNUC_UNUSED)
{
  /* a write to a null pointer; volatile so the compiler cannot delete it.
   * Mainly meant as a placeholder for adaptation */
  *(volatile int *) 0 = 0;
}

/* a volatile pointer, so the compiler can neither drop the allocations
 * below nor see that the block is already freed */
static volatile int *volatile faulty_block;

static void
faulty_unref (GstBuffer * buf)
{
  /* one unref too many: faulty still pushes the buffer downstream after
   * dropping the reference it was given */
  gst_buffer_unref (buf);
}

static void
faulty_double_free (GstBuffer * buf G_GNUC_UNUSED)
{
  faulty_block = malloc (sizeof (int));
  free ((void *) faulty_block);
  free ((void *) faulty_block);
}

static void
faulty_use_after_free (GstBuffer * buf G_GNUC_UNUSED)
{
  faulty_block = malloc (sizeof (int));
  free ((void *) faulty_block);
  *faulty_block = 0;
}

static void
faulty_spin (guint ms)
{
  gint64 end = g_get_monotonic_time () + ms * G_TIME_SPAN_MILLISECOND;

  /* the clock is read in user space, so this burns user CPU time */
  while (g_get_monotonic_time () < end);
}

/* the other half of the deadlock: takes the locks in the opposite order to
 * the streaming thread */
static gpointer
faulty_deadlock_thread (gpointer data)
{
  GstFaulty *faulty = data;

  GST_OBJECT_LOCK (faulty);
  g_atomic_int_set (&faulty->deadlock_locked, TRUE);
  /* held by the streaming thread, which now waits for the object lock */
  GST_PAD_STREAM_LOCK (GST_BASE_TRANSFORM_SINK_PAD (faulty));
  return NULL;
}

/* an AB-BA deadlock: the streaming thread holds the sink pad's stream lock
 * (A) while it handles the buffer, and waits for the object lock (B), which
 * a helper thread holds while it waits for A. Everything else that needs
 * either lock blocks too: property access, state changes, events. */
static void
faulty_deadlock (GstFaulty * faulty)
{
  g_thread_unref (g_thread_new ("faulty-deadlock", faulty_deadlock_thread,
          faulty));
  /* wait until the helper holds B, or there is no inversion */
  while (!g_atomic_int_get (&faulty->deadlock_locked))
    g_usleep (1000);
  GST_OBJECT_LOCK (faulty);
}

static void
faulty_call_method (FaultyMethod const method, GstBuffer * buf)
{
  /* called through a table so the compiler cannot inline the crash away;
   * methods without an entry here fail elsewhere and do nothing */
  static void (*const pf[]) (GstBuffer * buf) = {
    [FAULTY_SIGSEGV] = faulty_signal_sigsegv,
    [FAULTY_SIGBUS] = faulty_signal_sigbus,
    [FAULTY_SIGABRT] = faulty_signal_sigabrt,
    [FAULTY_CUSTOM_METHOD] = faulty_custom_method,
    [FAULTY_UNREF] = faulty_unref,
    [FAULTY_DOUBLE_FREE] = faulty_double_free,
    [FAULTY_USE_AFTER_FREE] = faulty_use_after_free,
  };

  if ((guint) method < G_N_ELEMENTS (pf) && pf[method])
    pf[method] (buf);
}


static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);


GST_DEBUG_CATEGORY_STATIC (gst_faulty_debug);
#define GST_CAT_DEFAULT gst_faulty_debug

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_faulty_debug, "faulty", 0, \
    "faulty element");
#define gst_faulty_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstFaulty, gst_faulty, GST_TYPE_BASE_TRANSFORM,
    _do_init);

static void gst_faulty_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_faulty_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_faulty_start (GstBaseTransform * trans);
static GstStateChangeReturn gst_faulty_change_state (GstElement * element,
    GstStateChange transition);
static GstFlowReturn gst_faulty_transform_ip (GstBaseTransform * base,
    GstBuffer * buf);


static void
gst_faulty_class_init (GstFaultyClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *trans_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_faulty_set_property;
  gobject_class->get_property = gst_faulty_get_property;


  gst_element_class_set_static_metadata (gstelement_class,
      "Faulty",
      "Generic",
      "Crash or fail a pipeline on purpose after num-buffers buffers",
      "Ashley Camba Garrido <ashwoods@gmail.com>");
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  gstelement_class->change_state = GST_DEBUG_FUNCPTR (gst_faulty_change_state);
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_faulty_transform_ip);
  trans_class->start = GST_DEBUG_FUNCPTR (gst_faulty_start);

  g_object_class_install_property (gobject_class, PROP_METHOD,
      g_param_spec_enum ("method", "Method",
          "How to fail once num-buffers buffers have passed",
          GST_TYPE_FAULTY_METHOD,
          DEFAULT_METHOD, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_BUFFERS,
      g_param_spec_int ("num-buffers", "Num buffers",
          "Number of buffers to pass through before failing",
          0, G_MAXINT, DEFAULT_NUM_BUFFERS, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_STALL_TIME,
      g_param_spec_uint ("stall-time", "Stall time",
          "How long the stall and spin methods block, in milliseconds",
          0, G_MAXUINT, DEFAULT_STALL_TIME, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

}

static void
gst_faulty_init (GstFaulty * faulty)
{

  GstBaseTransform *trans = GST_BASE_TRANSFORM (faulty);
  gst_base_transform_set_in_place (trans, TRUE);
  gst_base_transform_set_passthrough (trans, TRUE);

  faulty->num_buffers = DEFAULT_NUM_BUFFERS;
  faulty->method = DEFAULT_METHOD;
  faulty->stall_time = DEFAULT_STALL_TIME;
  faulty->num_buffers_left = faulty->num_buffers;
}

static GstFlowReturn
gst_faulty_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  GstFaulty *faulty = GST_FAULTY (base);
  if (faulty->num_buffers_left > 0) {
    GST_LOG_OBJECT (faulty, "Failing in %d buffers.", faulty->num_buffers_left);
    faulty->num_buffers_left--;
    return GST_FLOW_OK;
  }
  if (faulty->method == FAULTY_GST_ERROR) {
    GST_ELEMENT_ERROR (GST_ELEMENT (faulty),
        RESOURCE, READ, ("Cannot do this anymore."), (NULL));
    return GST_FLOW_ERROR;
  } else if (faulty->method == FAULTY_FLOW_ERROR) {
    GST_ERROR_OBJECT (faulty, "Boom!");
    return GST_FLOW_ERROR;
  } else if (faulty->method == FAULTY_HANG) {
    /* block the streaming thread for good while it holds the stream lock,
     * so data stops flowing and the pipeline cannot be shut down either */
    GST_ERROR_OBJECT (faulty, "Hanging the streaming thread.");
    for (;;)
      g_usleep (G_USEC_PER_SEC);
  } else if (faulty->method == FAULTY_STALL || faulty->method == FAULTY_SPIN) {
    /* block once, then let the stream recover */
    if (!faulty->blocked) {
      faulty->blocked = TRUE;
      if (faulty->method == FAULTY_STALL) {
        GST_ERROR_OBJECT (faulty, "Stalling the streaming thread for %u ms.",
            faulty->stall_time);
        g_usleep ((gulong) faulty->stall_time * 1000);
      } else {
        GST_ERROR_OBJECT (faulty, "Spinning the streaming thread for %u ms.",
            faulty->stall_time);
        faulty_spin (faulty->stall_time);
      }
    }
    return GST_FLOW_OK;
  } else if (faulty->method == FAULTY_DEADLOCK) {
    GST_ERROR_OBJECT (faulty, "Deadlocking the streaming thread.");
    faulty_deadlock (faulty);
    return GST_FLOW_OK;
  } else {
    /* the signals end here; the memory methods corrupt silently and carry
     * on, unless a tool such as ASan or valgrind catches them */
    faulty_call_method (faulty->method, buf);
    return GST_FLOW_OK;
  }

}

/* count down from num-buffers again each time the element starts */
static gboolean
gst_faulty_start (GstBaseTransform * trans)
{
  GstFaulty *faulty = GST_FAULTY (trans);

  faulty->num_buffers_left = faulty->num_buffers;
  faulty->blocked = FALSE;
  if (faulty->method == FAULTY_START_ERROR) {
    return FALSE;
  } else {
    return TRUE;
  }
}

static GstStateChangeReturn
gst_faulty_change_state (GstElement * element, GstStateChange transition)
{
  GstFaulty *faulty = GST_FAULTY (element);
  GstStateChangeReturn ret;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  /* fail READY->NULL, not PAUSED->READY: a failure there stops the bin from
   * shutting down the elements upstream, whose streaming threads keep
   * running and hang gst_deinit() */
  if (faulty->method == FAULTY_STOP_ERROR &&
      transition == GST_STATE_CHANGE_READY_TO_NULL) {
    GST_ELEMENT_ERROR (faulty, CORE, STATE_CHANGE, ("Failed to stop."), (NULL));
    return GST_STATE_CHANGE_FAILURE;
  }
  return ret;
}

static void
gst_faulty_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstFaulty *faulty;

  faulty = GST_FAULTY (object);

  GST_OBJECT_LOCK (faulty);
  switch (prop_id) {
    case PROP_METHOD:
      faulty->method = g_value_get_enum (value);
      break;
    case PROP_NUM_BUFFERS:
      faulty->num_buffers = g_value_get_int (value);
      faulty->num_buffers_left = faulty->num_buffers;
      break;
    case PROP_STALL_TIME:
      faulty->stall_time = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (faulty);
}

static void
gst_faulty_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstFaulty *faulty;

  faulty = GST_FAULTY (object);

  /* the deadlock method's helper thread keeps this lock, so reads freeze */
  GST_OBJECT_LOCK (faulty);
  switch (prop_id) {
    case PROP_METHOD:
      g_value_set_enum (value, faulty->method);
      break;
    case PROP_NUM_BUFFERS:
      g_value_set_int (value, faulty->num_buffers);
      break;
    case PROP_STALL_TIME:
      g_value_set_uint (value, faulty->stall_time);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (faulty);
}
