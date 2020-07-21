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
 * @short_description: enforce limitations on the data format
 *
 *
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
  static GType leaky_method_type = 0;
  static const GEnumValue leaky_method[] = {
    {LEAKY_MALLOC, "Malloc", "malloc"},
    {LEAKY_REF, "Reference", "ref"},
    {0, NULL, NULL},
  };

  if (!leaky_method_type) {
    leaky_method_type =
        g_enum_register_static ("LeakyMethod", leaky_method);
  }
  return leaky_method_type;
}

enum
{
  PROP_0,
  PROP_NUM_BUFFERS,
  PROP_METHOD,
};

// define global to avoid optimizer optiizaing it away
int *malloc_ptr;
void static
leaky_malloc_leak(GstBaseTransform * base, GstBuffer * buf)
{
  malloc_ptr = (int *) malloc(sizeof(int));
  // no free(ptr);  
}

void static
leaky_ref_leak(GstBaseTransform * base, GstBuffer * buf)
{
  gst_buffer_ref(buf);
  // no gst_buffer_unref
}

// void static
// leaky_hw(GstBaseTransform * base, GstBuffer * buf)
// {
// }

// void static
// leaky_map(GstBaseTransform * base, GstBuffer * buf)
// {
// }

void static 
leaky_call_method(u_int8_t const index, GstBaseTransform * base, GstBuffer * buf)
{
  static void (*pf[])(GstBaseTransform * base, GstBuffer * buf) = {
    leaky_malloc_leak,
    leaky_ref_leak};
  if (index < sizeof(pf) / sizeof(*pf))
    {
      pf[index](base, buf);
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
      "Leak some memory",
      "Ashley Camba Garrido <ashwoods@gmail.com>");
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_leaky_transform_ip);

  g_object_class_install_property (gobject_class, PROP_METHOD,
      g_param_spec_enum ("method", "Error Method",
          "Select the error method",
          GST_TYPE_LEAKY_METHOD,
          LEAKY_MALLOC, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_BUFFERS,
      g_param_spec_int ("num-buffers", "Numm buffers",
          "Number of buffers to output before raising an error",
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

static GstFlowReturn
gst_leaky_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  GstLeaky *leaky = GST_LEAKY (base);
  if (leaky->num_buffers_left > 0) {
    GST_INFO ("Calling something %d", leaky->num_buffers_left);
    leaky->num_buffers_left--;
    return GST_FLOW_OK;
  }
  leaky_call_method(leaky->method, base,  buf);
  return GST_FLOW_OK; // this shouldn't happen 
}


static void
gst_leaky_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstLeaky *leaky;

  leaky = GST_LEAKY(object);

  switch (prop_id) {
    case PROP_METHOD:
      leaky->method = g_value_get_enum (value);
      break;
    case PROP_NUM_BUFFERS:
      leaky->num_buffers = g_value_get_int (value);
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

