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
 

#ifndef __GST_FAULTY_H__
#define __GST_FAULTY_H__

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

// /* Standard boilerplate stuff */
// typedef struct _GstFaulty GstFaulty;
// typedef struct _GstFaultyClass GstFaultyClass;

/* #defines don't like whitespacey bits */
#define GST_TYPE_FAULTY (gst_faulty_get_type())
G_DECLARE_FINAL_TYPE (GstFaulty, gst_faulty, GST, FAULTY,
    GstBaseTransform)

typedef enum
{
  FAULTY_SIGSEGV,
  FAULTY_SIGBUS,
  FAULTY_SIGABRT,
  FAULTY_START_ERROR,
  FAULTY_STOP_ERROR,
  FAULTY_CUSTOM_METHOD,
  FAULTY_GST_ERROR
} FaultyMethod;

/**
 * GstFaulty:
 *
 * Opaque data structure.
 */
struct _GstFaulty
{
  GstBaseTransform parent;

  /* properties */
  gboolean silent;
  gint num_buffers;
  gint num_buffers_left;
  FaultyMethod method;
};

struct _GstFaultyClass {
  GstBaseTransformClass trans_class;
};



GType gst_faulty_get_type (void);


G_END_DECLS
#endif /* __GST_FAULTY_H__ */
