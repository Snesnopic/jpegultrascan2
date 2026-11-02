# jpegultrascancpp

A C++20 port of [jpegultrascan](https://github.com/MegaByte/jpegultrascan) 1.4.0 by Aaron
Kaluszka: it finds the progressive scan script that makes a JPEG smallest, trying every way
to split the coefficients into bands and bits of successive approximation. The coefficients
never change, so the image stays exactly the same.

Working name; meant to become a submodule of [chisel](https://github.com/Snesnopic/chisel).

## Status

Work in progress. The search is a line-by-line port of the Perl script's, but instead of
encoding thousands of candidate scripts it works out each scan's size the way libjpeg-turbo's
Huffman encoder would, without encoding. The result is the Perl script's output byte for byte,
every scan size included, on all the images tested so far.

## Building

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

Standalone, the build downloads and compiles libjpeg-turbo 3.2.0 as a static library. When a
parent project already defines `JPEG::JPEG` (chisel does, with mozjpeg), that one is used
instead: a binary can link only one libjpeg. mozjpeg is detected from its `jpeglib.h`, and its
own scan search is switched off (`JCP_FASTEST`) before any encoding. mozjpeg 4.1.5 can't
encode more than 64 scans (it overruns an array past that): a longer script is not used, and
the input is kept.

Options: `JPEGULTRASCAN_BUILD_CLI`, `JPEGULTRASCAN_MOZJPEG` (`AUTO`, `ON`, `OFF`).

Binaries for Linux, macOS, Windows and FreeBSD are built by the GitHub workflow; pushing a
`v*` tag publishes them as a release.

## Command line

    jpegultrascan [options] input.jpg output.jpg

`-b N` sets the most successive-approximation bits per band (default 3, as upstream), `-t N`
the threads (the result never depends on it), `--dc interleaved` allows only one DC scan for
all components (upstream's `-i -1`), `--markers jpegtran` writes what `jpegtran -copy all`
writes instead of keeping every segment as it is. The output is checked by decoding it and
comparing every coefficient with the input's; if it isn't smaller, the input is copied.

## Library

```cpp
#include <jpegultrascan/jpegultrascan.hpp>

jpegultrascan::Options options;
std::optional<jpegultrascan::Script> script =
    jpegultrascan::find_best_script(srcinfo, coef_arrays, options);
// set cinfo.scan_info / num_scans from script->scans, then jpeg_write_coefficients
```

Link `jpegultrascan::core`. libjpeg errors never end the process.

## License

Apache-2.0, like jpegultrascan: see `LICENSE` and `NOTICE`. Files derived from the Perl
script say so at the top.
