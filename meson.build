project(
  'nadeko', 'c',
  version : '1.0',
  default_options : [
    # Development settings
    'c_std=c2x',
    'warning_level=3',
    'debug=true',
    'optimization=3',
    'b_lto=true',
  ],
)

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

nadeko_dep = declare_dependency(
  link_with : [ nadeko_lib ],
)

snake_exe = executable(
  'snake', [ 'snake.c' ],
  override_options : [ 'b_sanitize=address,undefined' ],
  c_args : [ '-DSQLITE_CORE' ],
  dependencies : [ libarchive_dep, sqlite3_dep ],
)
