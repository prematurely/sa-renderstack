renderstack_d3d9_batch_audit_test = executable(
  'renderstack-d3d9-batch-audit-test',
  'd3d9_batch_audit_test.cpp',
  include_directories: dxvk_include_path,
  install: false,
)

renderstack_d3d9_deferred_shader_binding_test = executable(
  'renderstack-d3d9-deferred-shader-binding-test',
  'd3d9_deferred_shader_binding_test.cpp',
  include_directories: dxvk_include_path,
  install: false,
)

renderstack_dxvk_state_dedup_test = executable(
  'renderstack-dxvk-state-dedup-test',
  'dxvk_state_dedup_test.cpp',
  include_directories: dxvk_include_path,
  dependencies: dxbc_spirv_dep,
  install: false,
)

renderstack_user32 = cpp.find_library('user32')

executable(
  'renderstack-stateblock-prefilter-probe',
  'stateblock_prefilter_probe.cpp',
  include_directories: dxvk_include_path,
  dependencies: renderstack_user32,
  install: false,
)

executable(
  'renderstack-gta-sa-compat-probe',
  'gta_sa_compat_probe.cpp',
  include_directories: dxvk_include_path,
  dependencies: renderstack_user32,
  install: false,
)

test('renderstack-d3d9-batch-audit-test', renderstack_d3d9_batch_audit_test)
test('renderstack-d3d9-deferred-shader-binding-test', renderstack_d3d9_deferred_shader_binding_test)
test('renderstack-dxvk-state-dedup-test', renderstack_dxvk_state_dedup_test)
