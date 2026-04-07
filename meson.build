project('sysrepo-gnmi', 'c',
  version : '0.1.0',
  license : 'BSD-4-Clause',
  default_options : [
    'c_std=c11',
    'warning_level=2',
  ],
)

cc = meson.get_compiler('c')

# -- System dependencies ----------------------------------------------
protobufc_dep  = dependency('libprotobuf-c', version : '>=1.3')
cjson_dep      = dependency('libcjson', version : '>=1.7')
grpc_dep       = dependency('grpc', version : '>=1.18')
libevent_dep   = dependency('libevent', version : '>=2.1')
libevent_pt_dep = dependency('libevent_pthreads', version : '>=2.1')
threads_dep    = dependency('threads')
libsystemd_dep = dependency('libsystemd', version : '>=250', required : false)

# -- CESNET dependencies via cmake (custom_target) --------------------
# Prerequisite: run `meson subprojects download` before `meson setup`
deps_prefix = meson.current_build_dir() / 'deps'

cesnet_version = get_option('cesnet_version')

cesnet_deps = custom_target('cesnet_deps',
  output : 'cesnet_deps.stamp',
  command : [find_program('sh'),
    meson.current_source_dir() / 'build-deps.sh',
    meson.current_source_dir(),
    meson.current_build_dir(),
    '@OUTPUT@',
    cesnet_version],
  console : true,
)

cesnet_dep = declare_dependency(
  sources : cesnet_deps,
  compile_args : ['-I' + deps_prefix / 'include'],
  link_args : [
    '-L' + deps_prefix / 'lib',
    '-Wl,-rpath,' + deps_prefix / 'lib',
    '-lyang',
    '-lsysrepo',
  ],
)

# -- protobuf-c code generation (at configure time) -------------------
protoc = find_program('protoc')
proto_dir = meson.current_source_dir() / 'proto'
proto_inc = '/usr/include'
proto_outdir = meson.current_source_dir() / 'proto' / 'generated'

# Generate all protobuf-c stubs at configure time (protoc is fast).
# Output goes into proto/generated/ in the source tree so meson can
# reference it with include_directories() and files().
run_command('sh', meson.current_source_dir() / 'proto' / 'gen-proto-c.sh',
            proto_dir, proto_inc, proto_outdir, check : true)

gnmi_proto_lib = static_library('gnmi_proto_c',
  'proto/generated/gnmi.pb-c.c',
  'proto/generated/gnmi_ext.pb-c.c',
  'proto/generated/google/protobuf/any.pb-c.c',
  'proto/generated/google/protobuf/descriptor.pb-c.c',
  'proto/generated/google/protobuf/duration.pb-c.c',
  dependencies : [protobufc_dep],
  include_directories : include_directories('proto/generated'),
  c_args : ['-Wno-unused-parameter', '-Wno-missing-field-initializers'],
)

gnmi_proto_dep = declare_dependency(
  link_with : gnmi_proto_lib,
  include_directories : include_directories('proto/generated'),
  dependencies : [protobufc_dep],
)

# -- Python gRPC stub generation (for tests) --------------------------
# Only if grpc_tools is available (pip install grpcio-tools)
grpc_tools_check = run_command('python3', '-c', 'import grpc_tools', check : false)
if grpc_tools_check.returncode() == 0
  proto_py_gen = custom_target('proto_py',
    input : ['proto/gnmi.proto', 'proto/gnmi_ext.proto'],
    output : ['gnmi_pb2.py', 'gnmi_pb2_grpc.py',
              'gnmi_ext_pb2.py', 'gnmi_ext_pb2_grpc.py'],
    command : [find_program('python3'), '-m', 'grpc_tools.protoc',
      '-I' + proto_dir,
      '--python_out=@OUTDIR@',
      '--grpc_python_out=@OUTDIR@',
      proto_dir / 'gnmi.proto',
      proto_dir / 'gnmi_ext.proto'],
    build_by_default : true,
  )
endif

# -- Sanitizer support ------------------------------------------------
sanitize = get_option('sanitize')
sanitize_args = []
sanitize_link_args = []
if sanitize != 'none'
  sanitize_args += ['-fsanitize=' + sanitize, '-fno-omit-frame-pointer']
  sanitize_link_args += ['-fsanitize=' + sanitize]
endif

# -- Static analysis --------------------------------------------------
if get_option('analyzer')
  sanitize_args += ['-fanalyzer']
endif

# -- sysrepo-gnmi server ---------------------------------------------
gnmi_src = files(
  'src/main.c',
  'src/log.c',
  'src/server.c',
  'src/session.c',
  'src/gnmi_service.c',
  'src/capabilities.c',
  'src/xpath.c',
  'src/encode.c',
  'src/get.c',
  'src/set.c',
  'src/subscribe.c',
  'src/rpc.c',
  'src/confirm.c',
  'src/monitoring.c',
)

gnmi_server_deps = [
  gnmi_proto_dep,
  grpc_dep,
  libevent_dep,
  libevent_pt_dep,
  cjson_dep,
  threads_dep,
  cesnet_dep,
]
gnmi_server_cargs = sanitize_args

if libsystemd_dep.found()
  gnmi_server_deps += libsystemd_dep
  gnmi_server_cargs += '-DHAVE_SYSTEMD'
endif

gnmi_server = executable('gnmi_server',
  gnmi_src,
  dependencies : gnmi_server_deps,
  c_args : gnmi_server_cargs,
  link_args : sanitize_link_args,
  install : true,
  install_dir : get_option('sbindir'),
)

# -- Test helpers -----------------------------------------------------
seed_oper = executable('seed_oper',
  'tests/seed_oper.c',
  dependencies : [cesnet_dep, threads_dep],
  include_directories : include_directories('src'),
  c_args : sanitize_args,
  link_args : sanitize_link_args,
  install : false,
)

# -- Tests ------------------------------------------------------------
pytest = find_program('pytest', 'py.test', required : false)
if pytest.found()
  test_env = environment()
  test_env.set('GNMI_SERVER_BIN', gnmi_server.full_path())
  test_env.set('GNMI_PROTO_DIR', meson.current_build_dir())
  test_env.set('GNMI_YANG_DIR', meson.current_source_dir() / 'tests' / 'yang')
  test_env.set('SYSREPO_SHM_PREFIX', 'gnmitest_')
  test_env.set('SYSREPO_REPOSITORY_PATH', meson.current_build_dir() / 'test_repo')
  test_env.set('PKG_CONFIG_PATH', deps_prefix / 'lib' / 'pkgconfig')
  test_env.set('LD_LIBRARY_PATH', deps_prefix / 'lib')

  test('conformance', pytest,
    args : [meson.current_source_dir() / 'tests', '-v', '--tb=short'],
    env : test_env,
    depends : [gnmi_server, cesnet_deps],
    timeout : get_option('test_timeout'),
  )
endif
