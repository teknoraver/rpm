# RPM copy-on-write
RPM CoW is a technique to speedup the package extraction by using filesystem reflinks.

The base idea is to avoid copying the file content from the RPM archive to the destination,
but instead to create a reflink from the file content in the RPM archive to the destination file.

This is supported by the **XFS** and **Btrfs** filesystems Linux since kernel 4.5,
via the [FICLONERANGE](https://man7.org/linux/man-pages/man2/ioctl_ficlonerange.2.html) ioctl,
which allows to share file segments between files.

There are only two blockers to this technique: files data inside RPM packages is compressed,
and the ioctl requires the source offset and the destination offset to be aligned to the filesystem block size.

We solved this by transcoding on the fly the RPM archives into a new on-disk format, which we call extents file.

## The extents file
Created by a new tool named `rpm2extents`, this a file format which is basically an RPM archive with
uncompressed file data, and with extra padding used to align the file content to the filesystem block boundary.
At the end of the file, there is a table of file extents, which are the file content segments, and their offset in the file.

Additionally, rpm2extents detects duplicate files by keeping a hashtable of the file content and making identical files
in the same package point to the same data. In the extraction phase, this will deduplicate the file, saving disk space.

All other fields are in place (metadata, checksums, hooks, etc.).

## The RPM plugin
The new code lies mostly in an RPM plugin. It detects the presence of the extents file, and if found,
starts the installation of the package by using reflink.

The typical steps of a CoW file extraction are:
1. create the destination file as an empty file
2. call `ioctl(FICLONERANGE)` to reflink the file data from the archive to the destination file
3. as the file size is multiple of the filesystem block size, truncate the file to the original size
4. set the file permissions, ownership, SELinux context, etc.

Steps 1 and 4 are the same as in the traditional extraction, but step 2 is the key to the speedup,
which replaces the usual data copy.

In the unlikely case that the reflink fails (because the host kernel or filesystem doesn’t support reflinks,
or simply because the destination lies on another filesystem), the data is copied as fallback.

## The DNF plugin
The transcoding is done on the fly by a DNF plugin. This plugin hooks after the package download and
signature verification, and transcodes the RPM archive into an extents file.

As DNF downloads the packages in parallel, often rpm2extents will transcode while the network is being used,
so the two operations can overlap.

The DNF plugin is being rewritten in C++ to be ported in DNF5

## Benchmarks
The speed advantage of RPM CoW varies on the package type and hardware type. As less IO is done for file content,
it has a bigger advantage with packages containing big files, or machines with spinning drives.

As a reference I choose a package which is big enough to provide valid data, the core data package for LibreOffice 7.1.8.1.
The package is roughly 100 MB compressed, and expands to 280:
```
$ ll libreoffice-core-7.1.8.1-14.el9.x86_64.rpm*
-rw-r--r-- 1 matteo matteo 106M Oct 28 09:07 libreoffice-core-7.1.8.1-14.el9.x86_64.rpm
-rw-r--r-- 1 matteo matteo 284M Oct 28 09:07 libreoffice-core-7.1.8.1-14.el9.x86_64.rpme
```
The transcoding of this package takes ~2.7 seconds on a modern machine:
```
$ time rpm2extents SHA256 \
        <libreoffice-core-7.1.8.1-14.el9.x86_64.rpm \
        >libreoffice-core-7.1.8.1-14.el9.x86_64.rpme

real    0m2.769s
user    0m3.361s
sys     0m0.549s
```
Which is enough to keep up with a 320 Mbit line when downloading, or a Gbit line if DNF downloads three packages in parallel.
This is the installation of the aforementioned package in both formats (.rpme stands for RPM Extents):
```
# time rpm -i libreoffice-core-7.1.8.1-14.el9.x86_64.rpm

real    0m4.243s
user    0m1.724s
sys     0m1.168s

# time rpm -i libreoffice-core-7.1.8.1-14.el9.x86_64.rpme

real    0m0.711s
user    0m0.134s
sys     0m0.449s
```
The installation of the rpm2cow package is 6x faster and completed in 3.532 seconds faster than the stock one.
This is enough to compensate for the transcoding step even in the unlikely case that the download was instantaneous.

A bit of more detailed statistics with perf:

```
# perf stat rpm -i libreoffice-core-7.1.8.1-14.el9.x86_64.rpm

 Performance counter stats for 'rpm -i libreoffice-core-7.1.8.1-14.el9.x86_64.rpm':

          2,648.16 msec task-clock                #    0.639 CPUs utilized
             4,767      context-switches          #    1.800 K/sec
                30      cpu-migrations            #   11.329 /sec
             4,859      page-faults               #    1.835 K/sec
     5,939,045,942      cycles                    #    2.243 GHz
    13,945,714,688      instructions              #    2.35  insn per cycle
       944,586,613      branches                  #  356.696 M/sec
        15,546,713      branch-misses             #    1.65% of all branches

       4.141149038 seconds time elapsed

       1.612887000 seconds user
       1.027403000 seconds sys

# perf stat rpm -i libreoffice-core-7.1.8.1-14.el9.x86_64.rpme

 Performance counter stats for 'libreoffice-core-7.1.8.1-14.el9.x86_64.rpme':

            658.19 msec task-clock                #    0.898 CPUs utilized
               635      context-switches          #  964.763 /sec
                21      cpu-migrations            #   31.906 /sec
             3,372      page-faults               #    5.123 K/sec
     1,432,333,414      cycles                    #    2.176 GHz
     2,218,052,665      instructions              #    1.55  insn per cycle
       398,472,670      branches                  #  605.404 M/sec
         3,318,196      branch-misses             #    0.83% of all branches

       0.732587245 seconds time elapsed

       0.119177000 seconds user
       0.481303000 seconds sys
```
Other than the much lower installation time, we note that the context switches count dropped considerably,
so there is less pressure on the system.

## Caveats
The RPM CoW plugin is not a silver bullet, and there are some caveats to consider.

First, filesystem reflink is not supported by every OS or filesystem. At the moment, this is only supported
by Linux and only on Btrfs and XFS. Also, as the reflink can't cross a mount point, the destination files
must be on the same filesystem of the DNF cache, usually */var/cache/dnf*.

This is not a big problem, as separate */usr* partitions are not common anymore and the DNF cache is usually
on the root filesystem, but it's worth mentioning.

Second, the package transcoding is done after the signature validation for security reasons. This is suboptimal,
the optimal solution would be to pipe the package being downloaded into rpm2extents and transcode it on the fly.

This poses a security risk as the package is uncompressed before the signature validation, but it could be mitigated by
sandboxing rpm2extents with seccomp filters, a restrictive pid and mount namespace, and a dedicated user.
