"""
extra_script.py — PlatformIO per-library build-flag tuning for HubFX.

Currently used for one job: append -Os to libhelix-mp3's compile flags
so its fixed-point MP3 hot loops fit the LX7 I-cache better.  GCC's
"last -Ox wins" rule means appending -Os overrides the default -O2 for
this library only.  atomic14 reported ~5-10 % helix throughput
improvement on S3 with the same trick.

Append (not Replace) is critical — Replace clears the entire CCFLAGS
list including every framework-provided include path / arch flag,
which broke the build the first time around.
"""

Import("env")


def _add_os(lb):
    lb.env.Append(CCFLAGS=["-Os"], CXXFLAGS=["-Os"])


for lb in env.GetLibBuilders():
    name = lb.name.lower()
    if "libhelix" in name or "arduino-libhelix" in name:
        print(f"[extra_script] appending -Os to library: {lb.name}")
        _add_os(lb)
