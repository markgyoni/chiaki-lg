# Building

Releases are built by GitHub Actions (`.github/workflows/build.yml`). To build locally you need Linux or WSL2.

1. Get the webOS toolchain: download `arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz` from [buildroot-nc4 webos-a38c582](https://github.com/openlgtv/buildroot-nc4/releases/tag/webos-a38c582), unpack it and run its `relocate-sdk.sh`.
2. Install host tools: `autoconf automake libtool pkg-config file protobuf-compiler`, and `pip install "cmake<4" nanopb protobuf`.
3. Check out [chiaki-ng](https://github.com/streetpea/chiaki-ng) next to this repository at the commit in `CHIAKI_NG_REF` in the workflow, with `git submodule update --init third-party/nanopb`.
4. Build:

   ```bash
   export TOOLCHAIN_DIR=~/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
   ./build-webos.sh ../chiaki-ng
   ```

The package lands in `build-webos/`. The first run also builds the libraries the app needs (cached in `/tmp/webos-staging`).

## Releasing

1. Set `version` in `packaging/appinfo.json` and add a section for it to `CHANGELOG.md`.
2. Merge to `main`.
3. Tag it: `git tag -a v0.9.0 -m v0.9.0 && git push origin v0.9.0`.

The tag builds the app and publishes a GitHub release with the `.ipk` and the webOS Homebrew manifest. The Homebrew Channel picks up new releases on its own.
