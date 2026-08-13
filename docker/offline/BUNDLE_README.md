# 3D Tiles Clip Worker Offline Dependency Bundle

This directory contains the architecture-specific build and runtime base images used to compile
the Worker without network access. It does not contain Worker source code or deployment secrets.

From the matching architecture Worker project directory, verify and load the bundle:

```sh
./docker/offline/load-bundle.sh /path/to/this-bundle
```

Build a fresh final image from the current source with network access disabled and build cache
disabled:

```sh
./docker/offline/build.sh 3d-tiles-clip-worker:offline
```

The bundle architecture must match `uname -m`. Generate a new bundle in a connected environment
whenever `vcpkg.json`, the dependency version, Ubuntu, vcpkg, or the compiler baseline changes.
The `network_profile`, `base_image` and `apt_mirror` entries in `manifest.properties` record which
download profile, base image reference and Ubuntu package mirror were used to prepare this bundle.
They do not contain credentials.
