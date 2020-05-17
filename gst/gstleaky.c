/* GStreamer
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *                    2000 Wim Taymans <wtay@chello.be>
 *                    2005 Wim Taymans <wim@fluendo.com>
 *                    2005 David Schleef <ds@schleef.org>
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
// #include <gst/controller/controller.h>

#include "gstleaky.h"

enum
{
  PROP_0,
  FIXME_PROPERTY_EXAMPLE,
};


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

static GstFlowReturn gst_leaky_transform_ip (GstBaseTransform * base,
    GstBuffer * buf);


static void
gst_leaky_class_init (GstLeakyClass * klass)
{
  //GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *trans_class;

  //gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  trans_class = GST_BASE_TRANSFORM_CLASS (klass);
  
  
 // gobject_class->set_property = gst_leaky_set_property;
 // gobject_class->get_property = gst_leaky_get_property;


  gst_element_class_set_static_metadata (gstelement_class,
      "Leaky",
      "Generic",
      "Leak some memory",
      "Ashley Camba Garrido <ashwoods@gmail.com>");
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_leaky_transform_ip);
}

static void
gst_leaky_init (GstLeaky * filter)
{
  //GstBaseTransform *trans = GST_BASE_TRANSFORM (filter);
}

static GstFlowReturn
gst_leaky_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  /*
  TODO: types of leaks: gst_buffer_map, gst_object_ref, a few calls that you should g_free. 
  */
  
  return GST_FLOW_OK;
}

