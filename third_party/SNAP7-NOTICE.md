# Snap7 native runtime

FluentHMI Studio and exported runtimes dynamically link Snap7 1.4.3, under
LGPL-3.0-or-later. The unmodified official source is pinned by the submodule at:

https://github.com/davenardella/snap7/tree/30f37da3114024a71ba93f7fd855c680b97a406f

The build applies the following public patches to a separate build copy:

- `patches/snap7-macos.patch`: native monotonic clocks and per-socket SIGPIPE suppression on modern macOS.
- `patches/snap7-connect-timeout.patch`: bounded native TCP connection attempts using the configured timeout, including macOS and Windows.

The original submodule is unchanged. The patches and both GPL-3.0 and LGPL-3.0
license texts are included in `licenses/snap7` in the runtime. The corresponding
source can be reconstructed from the pinned source and these patches. Build the
shared library with `cmake --build <build-directory> --target snap7` using this
repository's `cmake/Snap7.cmake`. It can be replaced independently of the HMI
application, using a compatible library built for the same OS and architecture.

Snap7 author: Davide Nardella. Official project: https://snap7.sourceforge.net/
