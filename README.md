fdmonbench - File Descriptor Monitoring Benchmark
=================================================
Collect performance results for Linux file descriptor monitoring APIs.

The benchmark sends a message to one or more threads that monitor for file
descriptor activity. When a file descriptor becomes readable the message is
read and sent back as a reply. The next message is sent to a random file
descriptor when the reply is received.

This ping-pong test simulates an application that is monitoring one or more
file descriptors and one of them becomes ready at a time.

Supported APIs:
- select(2)
- poll(2)
- epoll(7)
- io\_uring
- threads (for comparison with threaded architectures)

Metrics
-------
- Duration (seconds) - wall clock time, may vary slightly from the desired
  duration
- Total Roundtrips - number of times a message was sent and a reply was
  received
- Roundtrips/second - the main performance metric, indicating the rate at which
  messages were transferred
- CPU usage (seconds) - total user and system CPU usage of the benchmark
  process
- Roundtrips/CPU seconds - efficiency metric, indicating how many messages are
  transferred per unit of CPU time

Usage
-----

    $ fdmonbench
    Usage: fdmonbench [OPTION]...
    Perform file descriptor monitoring benchmarking.

      --duration-secs=<int>  run for number of seconds (default: 30)
      --engine=epoll|io_uring|poll|select|threads
                             set fd monitoring engine (default: select)
      --exclusive=0|1        use EPOLLEXCLUSIVE (default: 0)
      --help                 print this help
      --msg-size             number of bytes per message (default: 1)
      --num-engines          number of engine instances (default: 1)
      --num-fds              number of file descriptors (default: 1)

This software is licensed under the GNU General Public License v3.0 or later.

Please contact Stefan Hajnoczi <stefanha@gmail.com> for questions about this
software.
