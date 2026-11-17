# The example applications: each one is a single file linking only GStreamer
# and GLib. They are not installed. supervise needs backtrace(), which musl
# lacks.
examples = ['run-pipeline', 'leak-check']
if meson.get_compiler('c').has_header('execinfo.h')
  examples += 'supervise'
endif
foreach example : examples
  executable(example, example + '.c',
    dependencies : [gst_dep, glib_dep])
endforeach
