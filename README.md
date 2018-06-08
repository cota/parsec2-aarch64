# PARSEC v2.1 for aarch64

This repo has been tested to build all benchmarks on an aarch64 host running
Ubuntu 16.04. The benchmarks are compiled into static binaries.

## Dependencies
At least the following packages are needed to complete a build:

	build-essential gettext pkg-config libx11-dev libxt-dev libxmu-dev libxi-dev libjpeg-dev

## How to build

	$ parsecmgmt -a build -c gcc-aarch64 -p blackscholes bodytrack canneal dedup facesim ferret fluidanimate freqmine raytrace streamcluster swaptions vips x264

You can also use the `gcc-aarch64-hooks` config which includes the hooks'
library.

## Adding input files
Only the _test_ inputs are included here. All other input files can be
downloaded from the
[PARSEC website.](http://parsec.cs.princeton.edu/download.htm)

## Known issues

* `libs/ssl` does not support parallel builds. Therefore if you are using the
  `MAKEFLAGS` environment variable to enable parallel builds, build libssl with
  `-p ssl` before building the benchmarks as listed above.
* Do not pass `-p all` to `parsecmgmt`. This tries to build libtbb, which is
  x86-only.
* Cross-compilation is not supported.
