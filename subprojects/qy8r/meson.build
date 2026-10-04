project('qy8r', 'c', version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'])

egl_dep = dependency('egl', required: false)
gles_dep = dependency('glesv2', required: false)
cc = meson.get_compiler('c')

qy8r_usp = static_library('qy8r_usp', 'qy8r_usp.c', install: true)
qy8r_usp_cpu = static_library('qy8r_usp_cpu', 'qy8r_usp_cpu.c',
  link_with: qy8r_usp, install: true)
qy8r_usp_glsl = static_library('qy8r_usp_glsl', 'qy8r_usp_glsl.c',
  link_with: qy8r_usp, install: true)

qy8r_sources = ['qy8r.c', 'qy8r_sw_api.c', 'qy8r_sw_raster.c']
qy8r_links = [qy8r_usp, qy8r_usp_cpu]
qy8r_dependencies = []
qy8r_arguments = []

if egl_dep.found() and gles_dep.found()
  qy8r_sources += ['qy8r_gl.c']
  qy8r_links += [qy8r_usp_glsl]
  qy8r_dependencies += [egl_dep, gles_dep]
  qy8r_arguments += ['-DQY8R_HAVE_GPU=1']
  if cc.has_header('EGL/eglext_angle.h', dependencies: egl_dep)
    qy8r_arguments += ['-DQY8R_HAVE_ANGLE_EGL=1']
  endif
else
  message('EGL/GLES unavailable; building the software backend only')
endif

shared_library('qy8r', qy8r_sources,
  include_directories: include_directories('.'),
  dependencies: qy8r_dependencies,
  link_with: qy8r_links,
  c_args: qy8r_arguments,
  install: true,
  version: '0.1.0')

install_headers('qy8r.h')
install_headers('qy8r_usp.h')
install_headers('qy8r_usp_cpu.h')
install_headers('qy8r_usp_glsl.h')
