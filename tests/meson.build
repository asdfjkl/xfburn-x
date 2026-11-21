test_c_args = ['-UG_DISABLE_ASSERT', '-UNDEBUG']

test_xfburn_log = executable(
  'test-xfburn-x-log',
  [
    'test-xfburn-log.c',
    '../xfburn/xfburn-log.c',
    '../xfburn/xfburn-log.h',
  ],
  c_args: test_c_args,
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
  c_args: test_c_args,
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

test_xfburn_async_device = executable(
  'test-xfburn-x-async-device',
  [
    'test-xfburn-async-device.c',
    '../xfburn/xfburn-device.c',
    '../xfburn/xfburn-device.h',
    '../xfburn/xfburn-udev-manager.c',
    '../xfburn/xfburn-udev-manager.h',
    '../xfburn/xfburn-log.c',
    '../xfburn/xfburn-log.h',
  ],
  c_args: test_c_args,
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

test('xfburn-x-async-device', test_xfburn_async_device)

test_xfburn_disc_info = executable(
  'test-xfburn-x-disc-info',
  [
    'test-xfburn-disc-info.c',
    '../xfburn/xfburn-disc-info.c',
    '../xfburn/xfburn-disc-info.h',
    '../xfburn/xfburn-device.c',
    '../xfburn/xfburn-device.h',
    '../xfburn/xfburn-udev-manager.c',
    '../xfburn/xfburn-udev-manager.h',
    '../xfburn/xfburn-utils.c',
    '../xfburn/xfburn-utils.h',
    '../xfburn/xfburn-log.c',
    '../xfburn/xfburn-log.h',
  ],
  c_args: test_c_args,
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

test('xfburn-x-disc-info', test_xfburn_disc_info)

test_xfburn_disc_usage = executable(
  'test-xfburn-x-disc-usage',
  [
    'test-xfburn-disc-usage.c',
    '../xfburn/xfburn-disc-usage.c',
    '../xfburn/xfburn-disc-usage.h',
    '../xfburn/xfburn-data-disc-usage.c',
    '../xfburn/xfburn-data-disc-usage.h',
    '../xfburn/xfburn-audio-disc-usage.c',
    '../xfburn/xfburn-audio-disc-usage.h',
    '../xfburn/xfburn-utils.c',
    '../xfburn/xfburn-utils.h',
    '../xfburn/xfburn-log.c',
    '../xfburn/xfburn-log.h',
  ],
  c_args: test_c_args,
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

test('xfburn-x-disc-usage', test_xfburn_disc_usage)
