d3d9_res = wrc_generator.process('version.rc')

d3d9_shaders = files([
  'shaders/d3d9_convert_yuy2_uyvy.comp',
  'shaders/d3d9_convert_l6v5u5.comp',
  'shaders/d3d9_convert_x8l8v8u8.comp',
  'shaders/d3d9_convert_a2w10v10u10.comp',
  'shaders/d3d9_convert_w11v11u10.comp',
  'shaders/d3d9_convert_nv12.comp',
  'shaders/d3d9_convert_yv12.comp',
  'shaders/d3d9_fixed_function_vert.vert',
  'shaders/d3d9_fixed_function_frag.frag',
  'shaders/d3d9_fixed_function_frag_sample.frag',
])

d3d9_src = [
  'd3d9_main.cpp',
  'd3d9_interface.cpp',
  'd3d9_adapter.cpp',
  'd3d9_monitor.cpp',
  'd3d9_device.cpp',
  'd3d9_state.cpp',
  'd3d9_cursor.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_format.cpp',
  'd3d9_common_texture.cpp',
  'd3d9_constant_buffer.cpp',
  'd3d9_constant_copy.cpp',
  'd3d9_texture.cpp',
  'd3d9_surface.cpp',
  'd3d9_volume.cpp',
  'd3d9_common_buffer.cpp',
  'd3d9_buffer.cpp',
  'd3d9_shader.cpp',
  'd3d9_vertex_declaration.cpp',
  'd3d9_query.cpp',
  'd3d9_shader_validator.cpp',
  'd3d9_multithread.cpp',
  'd3d9_options.cpp',
  'd3d9_stateblock.cpp',
  'd3d9_util.cpp',
  'd3d9_initializer.cpp',
  'd3d9_fixed_function.cpp',
  'd3d9_names.cpp',
  'd3d9_swvp_emu.cpp',
  'd3d9_format_helpers.cpp',
  'd3d9_hud.cpp',
  'd3d9_annotation.cpp',
  'd3d9_window.cpp',
  'd3d9_interop.cpp',
  'd3d9_gta_sa_compat.cpp',
  'd3d9_on_12.cpp',
  'd3d9_bridge.cpp',
  'd3d9_shader_analysis.cpp',
]

d3d9_ld_args      = []
d3d9_link_depends = []
d3d9_merged_dxgi_src = []
d3d9_cpp_args = []
d3d9_module_defs = 'd3d9'+def_spec_ext
d3d9_dependencies = [ dxbc_spirv_dep, dxvk_dep ]

if get_option('merge_dxgi_into_d3d9')
  d3d9_merged_dxgi_src = [
    '../dxgi/dxgi_adapter.cpp',
    '../dxgi/dxgi_enums.cpp',
    '../dxgi/dxgi_factory.cpp',
    '../dxgi/dxgi_format.cpp',
    '../dxgi/dxgi_main.cpp',
    '../dxgi/dxgi_monitor.cpp',
    '../dxgi/dxgi_options.cpp',
    '../dxgi/dxgi_output.cpp',
    '../dxgi/dxgi_surface.cpp',
    '../dxgi/dxgi_swapchain.cpp',
  ]
  d3d9_cpp_args += [ '-DDXVK_MERGED_D3D9_DXGI' ]
  d3d9_module_defs = 'd3d9_merged'+def_spec_ext

  if platform == 'windows'
    d3d9_dependencies += [ cpp.find_library('version') ]
  endif
endif

if platform != 'windows'
  d3d9_ld_args      += [ '-Wl,--version-script', join_paths(meson.current_source_dir(), 'd3d9.sym') ]
  d3d9_link_depends += files('d3d9.sym')
endif

d3d9_dll = shared_library(dxvk_name_prefix+'d3d9', d3d9_src, d3d9_merged_dxgi_src, glsl_generator.process(d3d9_shaders), d3d9_res,
  dependencies        : d3d9_dependencies,
  include_directories : dxvk_include_path,
  install             : true,
  vs_module_defs      : d3d9_module_defs,
  cpp_args            : d3d9_cpp_args,
  link_args           : d3d9_ld_args,
  link_depends        : [ d3d9_link_depends ],
  kwargs              : dxvk_so_version,
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ dxvk_include_path ],
)

if platform != 'windows'
  pkg.generate(d3d9_dll,
    filebase: dxvk_pkg_prefix + 'd3d9',
    subdirs:  'dxvk',
  )
endif
