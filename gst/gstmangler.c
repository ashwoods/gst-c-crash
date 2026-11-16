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
 * SECTION:element-mangler
 * @title: mangler
 * @short_description: break timestamps and events on purpose
 *
 * mangler passes buffers and events through untouched until
 * #GstMangler:num-buffers buffers have gone by, then breaks the stream the
 * way #GstMangler:mode says: it rewrites buffer timestamps or durations,
 * swallows events, or sends events it should not.
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 videotestsrc ! mangler num-buffers=5 mode=no-pts ! fakesink
 * ]|
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/base.h>
#include "gstmangler.h"

#define DEFAULT_NUM_BUFFERS     100
#define DEFAULT_MODE            MANGLER_NO_PTS

/* how far gap and before-segment move things */
#define MANGLER_OFFSET          GST_SECOND

#define GST_TYPE_MANGLER_MODE (gst_mangler_mode_get_type())
static GType
gst_mangler_mode_get_type (void)
{
  static gsize mangler_mode_type = 0;
  static const GEnumValue mangler_mode[] = {
    {MANGLER_NO_PTS, "Clear PTS and DTS: qtmux fails with 'Buffer has no PTS'",
        "no-pts"},
    {MANGLER_BACKWARDS, "Start the timestamps over from the first buffer's: "
          "muxers see decreasing DTS (needs num-buffers >= 2)", "backwards"},
    {MANGLER_GAP, "Jump the timestamps 1 s ahead without a DISCONT flag: "
          "synced sinks wait a second", "gap"},
    {MANGLER_BAD_DURATION, "Double each duration: buffers overlap the next",
        "bad-duration"},
    {MANGLER_BEFORE_SEGMENT, "Start a new segment 1 s ahead of the buffers: "
          "sinks drop them", "before-segment"},
    {MANGLER_DROP_EOS, "Swallow EOS: the pipeline never ends, an mp4 gets "
          "no moov", "drop-eos"},
    {MANGLER_DROP_SEGMENT, "Swallow segments: data flows before a segment",
        "drop-segment"},
    {MANGLER_DROP_STREAM_START, "Swallow stream-start: the app gets no "
          "stream-start message", "drop-stream-start"},
    {MANGLER_STICKY_STORM, "Send the caps again before every buffer: "
          "downstream sees duplicate caps", "sticky-storm"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&mangler_mode_type)) {
    GType type = g_enum_register_static ("ManglerMode", mangler_mode);
    g_once_init_leave (&mangler_mode_type, type);
  }
  return mangler_mode_type;
}

enum
{
  PROP_0,
  PROP_NUM_BUFFERS,
  PROP_MODE,
};

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);


GST_DEBUG_CATEGORY_STATIC (gst_mangler_debug);
#define GST_CAT_DEFAULT gst_mangler_debug

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_mangler_debug, "mangler", 0, \
    "mangler element");
#define gst_mangler_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstMangler, gst_mangler, GST_TYPE_BASE_TRANSFORM,
    _do_init);

static void gst_mangler_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_mangler_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_mangler_start (GstBaseTransform * trans);
static gboolean gst_mangler_sink_event (GstBaseTransform * trans,
    GstEvent * event);
static GstFlowReturn gst_mangler_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);


static void
gst_mangler_class_init (GstManglerClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *trans_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_mangler_set_property;
  gobject_class->get_property = gst_mangler_get_property;

  gst_element_class_set_static_metadata (gstelement_class,
      "Mangler",
      "Generic",
      "Break timestamps and events on purpose after num-buffers buffers",
      "Ashley Camba Garrido <ashwoods@gmail.com>");
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_mangler_transform_ip);
  trans_class->sink_event = GST_DEBUG_FUNCPTR (gst_mangler_sink_event);
  trans_class->start = GST_DEBUG_FUNCPTR (gst_mangler_start);

  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_enum ("mode", "Mode",
          "How to break the stream once num-buffers buffers have passed",
          GST_TYPE_MANGLER_MODE,
          DEFAULT_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_BUFFERS,
      g_param_spec_int ("num-buffers", "Num buffers",
          "Number of buffers to pass through before breaking the stream",
          0, G_MAXINT, DEFAULT_NUM_BUFFERS, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));
}

static void
gst_mangler_init (GstMangler * mangler)
{
  GstBaseTransform *trans = GST_BASE_TRANSFORM (mangler);

  /* in place, but not passthrough, so buffers are writable */
  gst_base_transform_set_in_place (trans, TRUE);

  mangler->num_buffers = DEFAULT_NUM_BUFFERS;
  mangler->mode = DEFAULT_MODE;
  mangler->num_buffers_left = mangler->num_buffers;
  mangler->first_pts = GST_CLOCK_TIME_NONE;
}

/* a segment that starts MANGLER_OFFSET after pts, at the same running time,
 * so the buffers in between fall before it. It keeps the seqnum of the
 * segment it replaces, as any new segment in the same stream would */
static GstEvent *
mangler_segment_ahead (GstBaseTransform * trans, GstClockTime pts)
{
  GstSegment segment;
  GstClockTime start = pts + MANGLER_OFFSET;
  GstEvent *event, *old;

  gst_segment_copy_into (&trans->segment, &segment);
  /* the offset is folded into the new base */
  segment.offset = 0;
  segment.base = gst_segment_to_running_time (&trans->segment,
      GST_FORMAT_TIME, start);
  segment.time = gst_segment_to_stream_time (&trans->segment,
      GST_FORMAT_TIME, start);
  segment.start = start;
  segment.position = start;
  event = gst_event_new_segment (&segment);
  old = gst_pad_get_sticky_event (GST_BASE_TRANSFORM_SINK_PAD (trans),
      GST_EVENT_SEGMENT, 0);
  if (old) {
    gst_event_set_seqnum (event, gst_event_get_seqnum (old));
    gst_event_unref (old);
  }
  return event;
}

/* move a timestamp by diff, but not below 0 */
static GstClockTime
mangler_shift_time (GstClockTime time, GstClockTimeDiff diff)
{
  if (!GST_CLOCK_TIME_IS_VALID (time))
    return time;
  if (diff < 0 && time < (GstClockTime) - diff)
    return 0;
  return time + diff;
}

static void
mangler_shift (GstBuffer * buf, GstClockTimeDiff diff)
{
  GST_BUFFER_PTS (buf) = mangler_shift_time (GST_BUFFER_PTS (buf), diff);
  GST_BUFFER_DTS (buf) = mangler_shift_time (GST_BUFFER_DTS (buf), diff);
}

/* before-segment works on time segments and needs the new start inside */
static gboolean
mangler_can_move_segment (GstBaseTransform * trans, GstClockTime pts)
{
  GstSegment *segment = &trans->segment;

  return segment->format == GST_FORMAT_TIME && segment->rate > 0 &&
      (!GST_CLOCK_TIME_IS_VALID (segment->stop) ||
      pts + MANGLER_OFFSET <= segment->stop);
}

static GstFlowReturn
gst_mangler_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstMangler *mangler = GST_MANGLER (trans);
  GstClockTime pts = GST_BUFFER_PTS (buf);
  gboolean first;

  if (!GST_CLOCK_TIME_IS_VALID (mangler->first_pts))
    mangler->first_pts = pts;
  if (mangler->num_buffers_left > 0) {
    GST_LOG_OBJECT (mangler, "Mangling in %d buffers.",
        mangler->num_buffers_left);
    mangler->num_buffers_left--;
    return GST_FLOW_OK;
  }
  /* backwards and before-segment start from the first timestamped buffer */
  if (!mangler->triggered && !GST_CLOCK_TIME_IS_VALID (pts) &&
      (mangler->mode == MANGLER_BACKWARDS ||
          mangler->mode == MANGLER_BEFORE_SEGMENT))
    return GST_FLOW_OK;
  first = !mangler->triggered;
  mangler->triggered = TRUE;

  switch (mangler->mode) {
    case MANGLER_NO_PTS:
      if (first)
        GST_ERROR_OBJECT (mangler, "Clearing timestamps.");
      GST_BUFFER_PTS (buf) = GST_CLOCK_TIME_NONE;
      GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;
      break;
    case MANGLER_BACKWARDS:
      if (first) {
        /* to the first buffer's: below num-buffers=2 that is not earlier
         * than the buffer before */
        mangler->shift = pts - mangler->first_pts;
        GST_ERROR_OBJECT (mangler, "Moving timestamps back by %"
            GST_TIME_FORMAT ".", GST_TIME_ARGS (mangler->shift));
      }
      mangler_shift (buf, -(GstClockTimeDiff) mangler->shift);
      break;
    case MANGLER_GAP:
      if (first)
        GST_ERROR_OBJECT (mangler, "Moving timestamps ahead by %"
            GST_TIME_FORMAT ".", GST_TIME_ARGS (MANGLER_OFFSET));
      mangler_shift (buf, MANGLER_OFFSET);
      break;
    case MANGLER_BAD_DURATION:
      if (first)
        GST_ERROR_OBJECT (mangler, "Doubling durations.");
      if (GST_BUFFER_DURATION_IS_VALID (buf))
        GST_BUFFER_DURATION (buf) *= 2;
      break;
    case MANGLER_BEFORE_SEGMENT:
      if (first && !mangler_can_move_segment (trans, pts)) {
        GST_WARNING_OBJECT (mangler, "Cannot start a segment ahead of %"
            GST_SEGMENT_FORMAT ".", &trans->segment);
      } else if (first) {
        GST_ERROR_OBJECT (mangler, "Starting a segment %" GST_TIME_FORMAT
            " ahead of the buffers.", GST_TIME_ARGS (MANGLER_OFFSET));
        gst_pad_push_event (GST_BASE_TRANSFORM_SRC_PAD (trans),
            mangler_segment_ahead (trans, pts));
        /* the first buffer after a new segment is a discontinuity */
        GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
      }
      break;
    case MANGLER_STICKY_STORM:{
      GstCaps *caps = gst_pad_get_current_caps (GST_BASE_TRANSFORM_SINK_PAD
          (trans));

      if (first)
        GST_ERROR_OBJECT (mangler, "Sending the caps before every buffer.");
      /* a new event with equal caps: the same event would not be resent */
      if (caps) {
        gst_pad_push_event (GST_BASE_TRANSFORM_SRC_PAD (trans),
            gst_event_new_caps (caps));
        gst_caps_unref (caps);
      }
      break;
    }
    default:
      /* the event modes act in sink_event */
      break;
  }
  return GST_FLOW_OK;
}

static gboolean
gst_mangler_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstMangler *mangler = GST_MANGLER (trans);
  gboolean drop;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      drop = mangler->mode == MANGLER_DROP_EOS;
      break;
    case GST_EVENT_SEGMENT:
      drop = mangler->mode == MANGLER_DROP_SEGMENT;
      break;
    case GST_EVENT_STREAM_START:
      drop = mangler->mode == MANGLER_DROP_STREAM_START;
      break;
    default:
      drop = FALSE;
      break;
  }
  /* events after num-buffers buffers; stream-start and the first segment
   * come before any buffer, so only num-buffers=0 drops those */
  if (drop && mangler->num_buffers_left == 0) {
    GST_ERROR_OBJECT (mangler, "Dropping the %s event.",
        GST_EVENT_TYPE_NAME (event));
    gst_event_unref (event);
    return TRUE;
  }
  return GST_BASE_TRANSFORM_CLASS (parent_class)->sink_event (trans, event);
}

/* count down from num-buffers again each time the element starts */
static gboolean
gst_mangler_start (GstBaseTransform * trans)
{
  GstMangler *mangler = GST_MANGLER (trans);

  mangler->num_buffers_left = mangler->num_buffers;
  mangler->first_pts = GST_CLOCK_TIME_NONE;
  mangler->shift = 0;
  mangler->triggered = FALSE;
  return TRUE;
}

static void
gst_mangler_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMangler *mangler = GST_MANGLER (object);

  GST_OBJECT_LOCK (mangler);
  switch (prop_id) {
    case PROP_MODE:
      mangler->mode = g_value_get_enum (value);
      break;
    case PROP_NUM_BUFFERS:
      mangler->num_buffers = g_value_get_int (value);
      mangler->num_buffers_left = mangler->num_buffers;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (mangler);
}

static void
gst_mangler_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMangler *mangler = GST_MANGLER (object);

  GST_OBJECT_LOCK (mangler);
  switch (prop_id) {
    case PROP_MODE:
      g_value_set_enum (value, mangler->mode);
      break;
    case PROP_NUM_BUFFERS:
      g_value_set_int (value, mangler->num_buffers);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (mangler);
}
