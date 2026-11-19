test_xfburn_log = executable(
  'test-xfburn-x-log',
  [
    'test-xfburn-log.c',
    '../xfburn/xfburn-log.c',
    '../xfburn/xfburn-log.h',
  ],
  sources: xfce_revision_h,
  include_directories: include_directories('..', '../xfburn'),
  dependencies: [
    gio,
    giounix,
    glib,
    gst,
    gthread,
    gtk,
    gudev,
    libburn,
    libisofs,
    libm,
    libxfce4util,
  ],
  install: false,
)

test('xfburn-x-log', test_xfburn_log)
