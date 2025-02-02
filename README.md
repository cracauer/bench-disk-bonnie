# bench-disk-bonnie
A modern derivative of the bonnie disk benchmark

This is my take on the classic bonnie filesystem benchmark.

Properties of this version:
- MIT licensed (forked before bonnie went GPL)
- does fsync(2) after writing files
- calls a shell program to drop filesystem caches (*)
- doesn't do single byte I/O anymore
- does a readonly seek pass
- defaults to 64 bit files on 32 bit Linux


(*) to use this define a program `dropthedamncaches` that does  this:
- on Linux: echo 1 > /proc/sys/vm/drop_caches
- on FreeBSD: FreeBSD will drop caches for one filesystem if you try
to unmount that filesystem, even if you don't succeed.
- ZFS: you should be able to drop ZFS's ARC cache by setting it to
zero and then back to the original value.

If you can't drop the caches it is recommended to use the -s parameter
to specify the test file size in gigabyte, using at least 1.5 times
your RAM.
