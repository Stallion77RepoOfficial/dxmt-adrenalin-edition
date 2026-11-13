d3d8_res = wrc_generator.process('version.rc')

d3d8_src = [
  'd3d8.cpp',
]

d3d8_ld_args      = ['-L' + windows_native_install_dir]
d3d8_link_depends = []

d3d8_dll = shared_library('d3d8', d3d8_src, d3d8_res,
  name_prefix         : '',
  dependencies        : [ util_dep ],
  include_directories : [ dxmt_include_path ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'd3d8.def',
  link_args           : d3d8_ld_args,
  link_depends        : [ d3d8_link_depends ],
)

d3d8_dep = declare_dependency(
  link_with           : [ d3d8_dll ],
  include_directories : [ dxmt_include_path ],
)

if wine_builtin_dll
custom_target('postprocess_libd3d8',
  input : d3d8_dll,
  output: 'd3d8.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : d3d8_dll,
  build_by_default : true
)
endif
