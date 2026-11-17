# The example applications: each one is a single file linking only GStreamer
# and GLib. They are not installed.
foreach example : ['run-pipeline', 'supervise', 'leak-check']
  executable(example, example + '.c',
    dependencies : [gst_dep, glib_dep])
endforeach
