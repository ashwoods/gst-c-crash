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
 * @short_description: enforce limitations on the data format
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/base.h>
#include "gstfaulty.h"

#define DEFAULT_NUM_BUFFERS     100
#define DEFAULT_METHOD          FAULTY_SIGSEGV

#define GST_TYPE_FAULTY_METHOD (gst_faulty_method_get_type())
static GType
gst_faulty_method_get_type (void)
{
  static GType faulty_method_type = 0;
  static const GEnumValue faulty_method[] = {
    {FAULTY_SIGSEGV, "SIGSEGV", "sigsegv"},
    {FAULTY_SIGBUS, "SIGBUS", "sigbus"},
    {FAULTY_SIGABRT, "SIGABRT", "sigabrt"},
    {FAULTY_START_ERROR, "START ERROR", "start"},
    {FAULTY_STOP_ERROR, "STOP ERROR", "stop"},
    {FAULTY_CUSTOM_METHOD, "CUSTOM METHOD", "custom"},
    {FAULTY_GST_ERROR, "GST ERROR", "gst"},
    {FAULTY_GOBJECT_ERROR, "GOBJECT ERROR", "gobject"},
    {0, NULL, NULL},
  };

  if (!faulty_method_type) {
    faulty_method_type =
        g_enum_register_static ("FaultyMethod", faulty_method);
  }
  return faulty_method_type;
}

enum
{
  PROP_0,
  PROP_NUM_BUFFERS,
  PROP_METHOD,
};

static void
faulty_signal_sigsegv()
{
  raise(SIGSEGV);
}

static void
faulty_signal_sigbus()
{
  raise(SIGBUS);
}

static void
faulty_signal_sigabrt()
{
  raise(SIGABRT);
}

static void
faulty_custom_method()
{
  // this isn't gauranteed to fail depending on compiler/platform
  // mainly meant as a placeholder for adaptation
  *(int*)0 = 0;
}

static void
faulty_call_method(u_int8_t const index)
{
  static void (*pf[])(void) = {
    faulty_signal_sigsegv,
    faulty_signal_sigbus,
    faulty_signal_sigabrt,
    faulty_custom_method};
  if (index < sizeof(pf) / sizeof(*pf))
    {
      pf[index]();
    }
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
static gboolean gst_faulty_start(GstBaseTransform * trans);
static gboolean gst_faulty_stop(GstBaseTransform * trans);
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
      "Leak some memory",
      "Ashley Camba Garrido <ashwoods@gmail.com>");
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_faulty_transform_ip);
  trans_class->start = GST_DEBUG_FUNCPTR (gst_faulty_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_faulty_stop);

  g_object_class_install_property (gobject_class, PROP_METHOD,
      g_param_spec_enum ("method", "Error Method",
          "Select the error method",
          GST_TYPE_FAULTY_METHOD,
          FAULTY_SIGSEGV, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_BUFFERS,
      g_param_spec_int ("num-buffers", "Numm buffers",
          "Number of buffers to output before raising an error",
          0, G_MAXINT, DEFAULT_NUM_BUFFERS, G_PARAM_READWRITE |
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
  faulty->num_buffers_left = faulty->num_buffers;
}

static GstFlowReturn
gst_faulty_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  GstFaulty *faulty = GST_FAULTY (base);
  if (faulty->num_buffers_left > 0) {
    GST_INFO ("Calling faulty in %d frames.", faulty->num_buffers_left);
    faulty->num_buffers_left--;
    return GST_FLOW_OK;
  }
  if (faulty->method == FAULTY_GST_ERROR) {
    GST_ELEMENT_ERROR(GST_ELEMENT(faulty),
                      RESOURCE,
                      READ,
                      ("Cannot do this anymore."),
                      (NULL));
    return GST_FLOW_ERROR;
  }
  else if (faulty->method == FAULTY_GOBJECT_ERROR) {
    GST_ERROR_OBJECT(faulty, "Boom!");
    return GST_FLOW_ERROR;
  }
  else {
    faulty_call_method(faulty->method);
    return GST_FLOW_OK; // code should never reach here
  }

}

static gboolean
gst_faulty_start (GstBaseTransform * trans)
{
  GstFaulty *faulty = GST_FAULTY(trans);
  if (faulty->method == FAULTY_START_ERROR) {
    return FALSE;
  } else {
    return TRUE;
  }
}

static gboolean
gst_faulty_stop (GstBaseTransform * trans)
{
  GstFaulty *faulty = GST_FAULTY(trans);
  if (faulty->method == FAULTY_STOP_ERROR) {
    return FALSE;
  } else {
    return TRUE;
  }
}

static void
gst_faulty_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstFaulty *faulty;

  faulty = GST_FAULTY(object);

  switch (prop_id) {
    case PROP_METHOD:
      faulty->method = g_value_get_enum (value);
      break;
    case PROP_NUM_BUFFERS:
      faulty->num_buffers = g_value_get_int (value);
      faulty->num_buffers_left = faulty->num_buffers; 
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_faulty_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstFaulty *faulty;

  faulty = GST_FAULTY (object);

  switch (prop_id) {
    case PROP_METHOD:
      g_value_set_enum (value, faulty->method);
      break;
    case PROP_NUM_BUFFERS:
      g_value_set_int (value, faulty->num_buffers);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

