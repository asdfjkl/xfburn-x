sizes = [16, 22, 24, 32, 48]
names = [
  'stock_xfburn-x-audio-cd',
  'stock_xfburn-x-audio-copy',
  'stock_xfburn-x-blank-cdrw',
  'stock_xfburn-x-burn-cd',
  'stock_xfburn-x-data-copy',
  'stock_xfburn-x-format-dvdrw',
  'stock_xfburn-x-import-session',
  'stock_xfburn-x-new-data-composition',
  'stock_xfburn-x',
]

foreach name : names
  foreach size : sizes
    filename = '@0@x@0@'.format(size) / '@0@.png'.format(name)
    if fs.is_file(filename)
      install_data(
        filename,
        install_dir: get_option('prefix') / get_option('datadir') / 'icons' / 'hicolor' / '@0@x@0@'.format(size) / 'stock' / 'media',
      )
      if name == 'stock_xfburn-x'
        install_data(
          filename,
          install_dir: get_option('prefix') / get_option('datadir') / 'icons' / 'hicolor' / '@0@x@0@'.format(size) / 'apps',
        )
      endif
    endif
  endforeach

  filename = 'scalable' / '@0@.svg'.format(name)
  if fs.is_file(filename)
    install_data(
      filename,
      install_dir: get_option('prefix') / get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'stock' / 'media',
    )
    if name == 'stock_xfburn-x'
      install_data(
        filename,
        install_dir: get_option('prefix') / get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
      )
    endif
  endif
endforeach
