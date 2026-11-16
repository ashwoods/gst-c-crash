project('gst-c-crash', 'c',
  version : '0.1.0',
  default_options : [ 'warning_level=2',
                      'buildtype=debug' ])

gst_req = '>= 1.0.0'
gst_crash_version = meson.project_version()
apiversion = '1.0'


cc = meson.get_compiler('c')

if cc.has_link_argument('-Wl,-Bsymbolic-functions')
  add_project_link_arguments('-Wl,-Bsymbolic-functions', language : 'c')
endif

# Symbol visibility
if cc.has_argument('-fvisibility=hidden')
  add_project_arguments('-fvisibility=hidden', language: 'c')
endif

# Disable strict aliasing
if cc.has_argument('-fno-strict-aliasing')
  add_project_arguments('-fno-strict-aliasing', language: 'c')
endif


glib_dep = dependency('glib-2.0')
gst_dep = dependency('gstreamer-1.0', version : gst_req,
  fallback : ['gstreamer', 'gst_dep'])
gst_base_dep = dependency('gstreamer-base-1.0', version : gst_req,
  fallback : ['gstreamer', 'gst_base_dep'])

gstreamer_crash_args = ['-DHAVE_CONFIG_H']
configinc = include_directories('.')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

conf = configuration_data()
conf.set_quoted('VERSION', gst_crash_version)
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('LICENSE', 'LGPL')
conf.set_quoted('GST_PACKAGE_NAME', 'gst-c-crash')
conf.set_quoted('PACKAGE_ORIGIN', get_option('package-origin'))
configure_file(output : 'config.h', configuration : conf)

subdir('gst')
subdir('pkgconfig')

# install the style-check hook in a git checkout; tarballs and worktrees
# have no .git folder
if import('fs').is_dir('.git/hooks')
  python3 = import('python').find_installation()
  run_command(python3, '-c', 'import shutil, sys; shutil.copy(*sys.argv[1:])',
    meson.project_source_root() / 'hooks/pre-commit.hook',
    meson.project_source_root() / '.git/hooks/pre-commit',
    check : true)
endif