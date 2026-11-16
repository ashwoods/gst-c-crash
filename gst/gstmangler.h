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

#ifndef __GST_MANGLER_H__
#define __GST_MANGLER_H__

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_MANGLER (gst_mangler_get_type())
G_DECLARE_FINAL_TYPE (GstMangler, gst_mangler, GST, MANGLER,
    GstBaseTransform)

typedef enum
{
  MANGLER_NO_PTS,
  MANGLER_BACKWARDS,
  MANGLER_GAP,
  MANGLER_BAD_DURATION,
  MANGLER_BEFORE_SEGMENT,
  MANGLER_DROP_EOS,
  MANGLER_DROP_SEGMENT,
  MANGLER_DROP_STREAM_START,
  MANGLER_STICKY_STORM,
} ManglerMode;

/**
 * GstMangler:
 *
 * Opaque data structure.
 */
struct _GstMangler
{
  GstBaseTransform parent;

  /* properties */
  gint num_buffers;
  gint num_buffers_left;
  ManglerMode mode;

  /* reset in start() */
  GstClockTime first_pts;
  GstClockTime shift;
  gboolean triggered;
};

G_END_DECLS
#endif /* __GST_MANGLER_H__ */
