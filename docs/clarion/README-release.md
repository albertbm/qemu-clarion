# Clarion head-unit emulator (binary package)

Layout:

- `qemu-clarion`: launcher; reads the unit model from the PROD block of the
  NOR image and starts the matching engine.
- `libexec/qemu-system-arm`: engine for QY8 units.
- `libexec/qemu-system-sh4`: engine for the QY7221NL.
- `lib/`: bundled shared libraries.

Usage:

    ./qemu-clarion NOR [SD]
    ./qemu-clarion --help

On macOS, remove the quarantine attribute first (the binaries are only
ad-hoc signed):

    xattr -dr com.apple.quarantine .

Source and documentation: https://github.com/alexpalyan/qemu
