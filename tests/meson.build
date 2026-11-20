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

test_xfburn_speed = executable(
  'test-xfburn-x-speed',
  [
    'test-xfburn-speed.c',
    '../xfburn/xfburn-utils.c',
    '../xfburn/xfburn-utils.h',
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
    libxfce4ui,
    libxfce4util,
  ],
  install: false,
)

test('xfburn-x-speed', test_xfburn_speed)
