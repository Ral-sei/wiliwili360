# Xbox 360 Debian Build

This is the native Linux build path for the Xenon/OXDK target. It does not require
WSL or a Windows drive mount.

## Prerequisites

Install the host tools:

```sh
sudo apt install cmake ninja-build python3 make
```

The Xbox 360 dependencies must be available locally:

```text
../OXDK/
../Microsoft Xbox 360 SDK/
~/oxdk-llvm/build/
../bili360-main/             # optional when third_party is present
```

The XDK directory contains spaces in its usual name. The build script creates a
space-free symlink inside the build directory because the OXDK makefiles require it.

## Build

From the repository root:

```sh
scripts/build_xbox360_linux.sh
```

The script configures `build-xbox360-linux`, builds libc++ and the application,
uses `build-xbox360-linux/oxdk-cache` for translated XDK archives, and writes the
full log to `build-xbox360-linux/build.log`. The XEX is:

```text
build-xbox360-linux/wiliwili
```

Override paths or parallelism when needed:

```sh
OXDK_DIR=/opt/OXDK \
XDK_DIR=/opt/xdk360 \
OXDK_LLVM=$HOME/toolchains/oxdk-llvm/build \
BILI360_DIR=$HOME/src/bili360-main \
JOBS=8 scripts/build_xbox360_linux.sh
```

`BUILD_DIR`, `BUILD_TYPE`, `LOG_FILE`, `REPO`, and `OXDK360_CACHE` are also
supported environment overrides.

## Verification

The verified Debian build produced a Xenon XEX with base address `0x82000000`.
Compilation and linking do not replace Xenia or real-devkit UI validation; deploy
the XEX together with `resources/` before testing startup and rendering.

If the linker reports that it cannot write `~/.cache/oxdk360`, set
`OXDK360_CACHE` to a writable directory, for example the build directory shown
above. If a compiler object is reported as `i386`, reconfigure the build directory
after updating the checkout; the Xbox toolchain must pass
`--target=powerpc-unknown-none-elf` to every compile command.
