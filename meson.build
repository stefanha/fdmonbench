project('fdmonbench', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'c_std=gnu11', 'c_args=-D_GNU_SOURCE'],
  license : 'GPL-3.0-or-later')

executable('fdmonbench',
           'epoll.c',
           'io_uring.c',
           'iogen.c',
           'main.c',
           'poll.c',
           'select.c',
           dependencies : [
               dependency('threads'),
               dependency('liburing'),
           ],
           install : true)
