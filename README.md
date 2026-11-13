# DXMT Adrenalin Edition

This is the authoritative Direct3D 8–12 source fork used by GooseHUB. Local shader, resource, synchronization and presentation changes are retained. Upstream changes are reviewed individually.

Keep the checkout beside `GooseHUB`, `wine-adrenalin-edition` and `fex-adrenalin-edition`. GooseHUB fetches and builds the pinned sources during first-launch setup. Build output is kept in `~/Library/Caches/GooseHUB/Build/DXMT`, and the installed runtime is placed inside the app. Generated DLLs and native libraries are excluded from this repository.

For a standalone build, set `WINE_BUILD` to the matching native Wine build and run `./build.sh`. The default targets ARM64EC and x86 for native Apple Silicon Wine.

The upstream project and its license are acknowledged below.

# DXMT

A Metal-based translation layer for Direct3D 11 and 10 which allows running 3D applications on macOS using Wine.

For the current status of the project, please refer to the [project wiki](https://github.com/3Shain/dxmt/wiki).

The most recent development builds can be found [here](https://github.com/3Shain/dxmt/actions).


## Build

See [DEVELOPMENT.md](docs/DEVELOPMENT.md)
