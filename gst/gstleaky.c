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
 * SECTION:element-leaky
 * @title: leaky
 * @short_description: leak memory on purpose
 *
 * leaky passes buffers through untouched until #GstLeaky:num-buffers
 * buffers have gone by, then leaks on every buffer after that, the way
 * #GstLeaky:method says.
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 videotestsrc num-buffers=110 ! leaky method=ref ! fakesink
 * ]|
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/base.h>
#include "gstleaky.h"

#define DEFAULT_NUM_BUFFERS     100
#define DEFAULT_METHOD          LEAKY_MALLOC

#define GST_TYPE_LEAKY_METHOD (gst_leaky_method_get_type())
static GType
gst_leaky_method_get_type (void)
{
  static gsize leaky_method_type = 0;
  static const GEnumValue leaky_method[] = {
    {LEAKY_MALLOC, "Leak a malloc() block", "malloc"},
    {LEAKY_REF, "Leak a reference on the buffer", "ref"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&leaky_method_type)) {
    GType type = g_enum_register_static ("LeakyMethod", leaky_method);
    g_once_init_leave (&leaky_method_type, type);
  }
  return leaky_method_type;
}

enum
{
  PROP_0,
  PROP_NUM_BUFFERS,
  PROP_METHOD,
};

/* global, so the optimizer cannot drop the allocation */
int *malloc_ptr;

static void
leaky_malloc_leak (GstBuffer * buf G_GNUC_UNUSED)
{
  malloc_ptr = (int *) malloc (sizeof (int));
  /* no free (malloc_ptr) */
}

static void
leaky_ref_leak (GstBuffer * buf)
{
  gst_buffer_ref (buf);
  /* no gst_buffer_unref (buf) */
}

static void
leaky_call_method (LeakyMethod const method, GstBuffer * buf)
{
  static void (*const pf[]) (GstBuffer * buf) = {
    [LEAKY_MALLOC] = leaky_malloc_leak,
    [LEAKY_REF] = leaky_ref_leak,
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


GST_DEBUG_CATEGORY_STATIC (gst_leaky_debug);
#define GST_CAT_DEFAULT gst_leaky_debug

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_leaky_debug, "leaky", 0, \
    "leaky element");
#define gst_leaky_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstLeaky, gst_leaky, GST_TYPE_BASE_TRANSFORM,
    _do_init);

static void gst_leaky_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_leaky_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstFlowReturn gst_leaky_transform_ip (GstBaseTransform * base,
    GstBuffer * buf);
static gboolean gst_leaky_start (GstBaseTransform * base);


static void
gst_leaky_class_init (GstLeakyClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *trans_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_leaky_set_property;
  gobject_class->get_property = gst_leaky_get_property;


  gst_element_class_set_static_metadata (gstelement_class,
      "Leaky",
      "Generic",
      "Leak memory on purpose after num-buffers buffers",
      "Ashley Camba Garrido <ashwoods@gmail.com>");
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);


  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_leaky_transform_ip);
  trans_class->start = GST_DEBUG_FUNCPTR (gst_leaky_start);

  g_object_class_install_property (gobject_class, PROP_METHOD,
      g_param_spec_enum ("method", "Method",
          "How to leak once num-buffers buffers have passed",
          GST_TYPE_LEAKY_METHOD,
          DEFAULT_METHOD, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_BUFFERS,
      g_param_spec_int ("num-buffers", "Num buffers",
          "Number of buffers to pass through before leaking",
          0, G_MAXINT, DEFAULT_NUM_BUFFERS, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

}

static void
gst_leaky_init (GstLeaky * leaky)
{


  GstBaseTransform *trans = GST_BASE_TRANSFORM (leaky);
  gst_base_transform_set_in_place (trans, TRUE);
  gst_base_transform_set_passthrough (trans, TRUE);

  leaky->num_buffers = DEFAULT_NUM_BUFFERS;
  leaky->method = DEFAULT_METHOD;
  leaky->num_buffers_left = leaky->num_buffers;
}

/* count down from num-buffers again each time the element starts */
static gboolean
gst_leaky_start (GstBaseTransform * base)
{
  GstLeaky *leaky = GST_LEAKY (base);

  leaky->num_buffers_left = leaky->num_buffers;
  return TRUE;
}

static GstFlowReturn
gst_leaky_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  GstLeaky *leaky = GST_LEAKY (base);
  if (leaky->num_buffers_left > 0) {
    GST_LOG_OBJECT (leaky, "Leaking in %d buffers.", leaky->num_buffers_left);
    leaky->num_buffers_left--;
    return GST_FLOW_OK;
  }
  leaky_call_method (leaky->method, buf);
  return GST_FLOW_OK;
}


static void
gst_leaky_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstLeaky *leaky;

  leaky = GST_LEAKY (object);

  switch (prop_id) {
    case PROP_METHOD:
      leaky->method = g_value_get_enum (value);
      break;
    case PROP_NUM_BUFFERS:
      leaky->num_buffers = g_value_get_int (value);
      leaky->num_buffers_left = leaky->num_buffers;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_leaky_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstLeaky *leaky;

  leaky = GST_LEAKY (object);

  switch (prop_id) {
    case PROP_METHOD:
      g_value_set_enum (value, leaky->method);
      break;
    case PROP_NUM_BUFFERS:
      g_value_set_int (value, leaky->num_buffers);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
