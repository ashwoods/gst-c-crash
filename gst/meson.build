crash_sources = [
  'gstleaky.c',
  'gstfaulty.c',
  'gstcrash.c'
]


gstcrash = library('gstcrash',
  crash_sources,
  c_args : gstreamer_crash_args + ['-DGST_USE_UNSTABLE_API'],
  include_directories : [configinc],
  dependencies : [gst_dep, gst_base_dep],
  install : true,
  install_dir : plugins_install_dir,
  # override_options : ['cpp_std=c++98', 'c_std=c99']
)

plugins = [gstcrash]
# install_headers(gst_s3_public_headers, subdir : 'gstreamer-1.0/gst/aws')