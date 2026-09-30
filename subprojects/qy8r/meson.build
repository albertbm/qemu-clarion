project('qy8r', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])
egl_dep = dependency('egl', required: true)
gles_dep = dependency('glesv2', required: true)
shared_library('qy8r', 'qy8r.c',
  include_directories: include_directories('.'),
  dependencies: [egl_dep, gles_dep],
  install: true,
  version: '0.1.0')
install_headers('qy8r.h')
