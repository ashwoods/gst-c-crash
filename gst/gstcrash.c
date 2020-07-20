/*
 * GStreamer
 * Copyright (C) 2020 Ashley Camba Garrido <ashwoods@gmail.com>
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

#include "config.h"
#include <gst/gst.h>

#include "gstleaky.h"
#include "gstfaulty.h"

static gboolean
plugin_init (GstPlugin * plugin)
{ 
  if(!gst_element_register (plugin, "leaky", GST_RANK_NONE, GST_TYPE_LEAKY))
    return FALSE;

  if(!gst_element_register (plugin, "faulty", GST_RANK_NONE, GST_TYPE_FAULTY))
    return FALSE;

  return TRUE;
}

/* gstreamer looks for this structure to register realsensesrc */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    crash,
    "Gstcrash plugin",
    plugin_init, 
    VERSION, LICENSE, PACKAGE_DESCRIPTION, PACKAGE_ORIGIN
)
