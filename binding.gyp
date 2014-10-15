{
  'targets': [{
    'target_name': 'simplejack',
    'sources': [ 'src/binding.cc' ],

    # Flags and defines
    'cflags': ['-Wall','-Wextra','-Wno-unused-parameter','-O3'],

    # Enable exceptions
    'cflags!': [ '-fno-exceptions' ],
    'cflags_cc!': [ '-fno-exceptions' ],
    'conditions': [
      ['OS=="mac"', {
        'xcode_settings': {
          'GCC_ENABLE_CPP_EXCEPTIONS': 'YES'
        }
      }]
    ]
  }]
}
