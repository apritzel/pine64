Binaries
========

---

**NOTE**

These binaries here are no secret blobs, but are just here provided for
convenience. You can always rebuild them, as described below.

---

This directory here provides built versions of some firmware components,
because:
- You might run into issues when some binaries require certain toolchains.
ATF for instance is known for some peculiarities.
- You just can't be bothered with cloning and building just another piece
of software.
- You might want to troubleshoot a firmware build, and want to isolate the
culprit by replacing some components with known good builds.
- Some binaries are not fully mainlined (FEL capable SPL builds).

ARM Trusted Firmware builds
---------------------------
- `bl31.bin`: default debug build of the ARM Trusted Firmware binary. This is
the branch based on the original Allwinner fork.
- `bl31-nodebug.bin`: built as a release version, which does not output anything
on the serial console, but is functionally identical to the above build. This
version will leave you clueless about anything going wrong within the ATF
component, but it is smaller than the debug build (20KB vs. 32KB) and might
be required for situations where image size is of importance.

To rebuild those binaries, fetch the allwinner branch from the
[ATF](https://github.com/apritzel/arm-trusted-firmware) repository:

    $ export CROSS_COMPILE=aarch64-linux-
    $ make PLAT=sun50iw1p1 DEBUG=1 bl31

The resulting binary is `build/sun50iw1p1/debug/bl31.bin`. For the non-debug
build, use `DEBUG=0` in the command line above and use the binary at
`build/sun50iw1p1/release/bl31.bin`.
