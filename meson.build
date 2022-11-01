project(
  'nadeko', 'c',
  version : '1.0',
  default_options : [
    # Development settings
    'c_std=c99',
    'warning_level=3',
    'debug=true',
    'optimization=3',
    'b_lto=true',
  ],
)

r = run_command('python', '-c', 'import os; print(os.environ["PKG_CONFIG_PATH"])', check: false)
message(r.stdout().strip())

sqlite3_dep = dependency(
  'sqlite3',
  required: true,
)
libarchive_dep = dependency(
  'libarchive',
  required: true,
)

nadeko_lib = both_libraries(
  'nadeko', [ 'nadeko.c' ],
  dependencies : [ libarchive_dep, sqlite3_dep ],
  pic : true,
  install : true,
)
lines_lib = both_libraries(
  'lines', [ 'lines.c' ],
  dependencies : [ sqlite3_dep ],
  pic : true,
  install : true,
)

nadeko_dep = declare_dependency(
  link_with : [ nadeko_lib ],
)
lines_dep = declare_dependency(
  link_with : [ lines_lib ],
)

nadeko_exe = executable(
  'nadeko-sqlite3', [ 'main.c' ],
  override_options : [ ],
  c_args : [ '-DSQLITE_CORE' ],
  dependencies : [ libarchive_dep, sqlite3_dep ],
)
