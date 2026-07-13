scanbadblocks
==============

Empirically check blocks of check USB drives, SSDs and other disks by reading and optionally writing all blocks.

This is similar to testing a device using dd but unlike dd this tool writes a unique pattern into blocks and verifies the pattern when read back.
This will detect USB sticks that report a false (too big) size, that simply mirror the same blocks multiple times.

Usage
-----

See scanbadblocks --help:

```
Usage: scanbadblocks [OPTIONS] BLOCK_DEVICE

Options:
  -b --block-size=BLOCKSIZE Granularity of reads/writes in bytes. (default=4M)
  -s --stride=STRIDE        Distance between read/write offsets. The default tracks --block-size. Use
                            values larger than --block-size to sample the full disk range, e.g.
                            --block-size=1M --stride=1G.
     --offset=OFFSET        Initial offset before applying --stride. Use this to scan another interleaved
                            part of the disk, e.g. --block-size=1M --stride=1G --offset=512M. (default=0)
  -w --overwrite            Overwrite device with known pattern and then read it back. This immediately
                            destroys the contents of the disk, erases the disk and deletes all files on the
                            disk. Specify twice to override interactive safety prompt. The default is just
                            to read the disk.
  -p --pattern=PATTERN      Comma separated list of one or more hexadecimal byte values for --overwrite.
                            Each byte will result in one write pass and one read pass on the disk. Useful
                            patterns to clear the disk 4 times are 55,aa,00,ff. The default is 00 resulting
                            in one write pass and one read pass. (default=00)
  -o --outfile=PREFIX       Write timing data to CSV files of the format PREFIX_PASS_DISKSIZE.txt.  (default=scanbadblocks)
  -d --drop-caches          Linux only: sync and drop page cache, dentries and inodes before starting.
                            May be used without BLOCK_DEVICE to only drop caches.
  -v --verbose              Increase verbosity. Specify multiple times to be more verbose.
  -h --help                 Print this help message and exit. (set)
     --version              Print version and exit.
scanbadblocks version 1.0.3 *** Copyright (c) 2025 Johannes Overmann *** https://github.com/jovermann/scanbadblocks
```
