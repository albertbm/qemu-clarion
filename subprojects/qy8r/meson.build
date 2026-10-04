project('qy8r', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])
egl_dep = dependency('egl', required: false)
gles_dep = dependency('glesv2', required: false)
if egl_dep.found() and gles_dep.found()
  shared_library('qy8r', 'qy8r.c',
    include_directories: include_directories('.'),
    dependencies: [egl_dep, gles_dep],
    install: true,
    version: '0.1.0')
else
  message('EGL/GLES unavailable; building the standalone qy8r_usp core only')
endif
# Deliberately independent of EGL/GLES so IR consumers can link it in a
# software-only process.
static_library('qy8r_usp', 'qy8r_usp.c', install: true)
static_library('qy8r_usp_cpu', 'qy8r_usp_cpu.c', install: true)
static_library('qy8r_usp_glsl', 'qy8r_usp_glsl.c', install: true)
install_headers('qy8r.h')
install_headers('qy8r_usp.h')
install_headers('qy8r_usp_cpu.h')
install_headers('qy8r_usp_glsl.h')
