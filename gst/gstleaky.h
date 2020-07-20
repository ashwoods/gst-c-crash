 /* GStreamer
 * Copyright (C) <2020> <ashwoods@gmail.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
 

#ifndef __GST_LEAKY_H__
#define __GST_LEAKY_H__

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

// /* Standard boilerplate stuff */
// typedef struct _GstLeaky GstLeaky;
// typedef struct _GstLeakyClass GstLeakyClass;

/* #defines don't like whitespacey bits */
#define GST_TYPE_LEAKY (gst_leaky_get_type())
G_DECLARE_FINAL_TYPE (GstLeaky, gst_leaky, GST, LEAKY,
    GstBaseTransform)

typedef enum
{
  LEAKY_MALLOC,
  LEAKY_REF,
} LeakyMethod;

/**
 * GstLeaky:
 *
 * Opaque data structure.
 */
struct _GstLeaky
{
  GstBaseTransform parent;

  /* properties */
  gboolean silent;
  gint num_buffers;
  gint num_buffers_left;
  LeakyMethod method;
};

struct _GstLeakyClass {
  GstBaseTransformClass trans_class;
};



GType gst_leaky_get_type (void);


G_END_DECLS
#endif /* __GST_LEAKY_H__ */

