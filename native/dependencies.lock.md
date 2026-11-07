# Native dependency pins

Every version here is exact. `tools/setup_runtime.ps1` and `CMakeLists.txt` refuse a download whose checksum differs.

| Dependency | Version | Source | SHA-256 | Licence |
| --- | --- | --- | --- | --- |
| Python (private runtime) | 3.13.17 | `https://www.nuget.org/api/v2/package/python/3.13.17` | `DCE409744B088F94C6E37BA41F8100C384C27E029E843237364C602D30D475E5` | PSF |
| NumPy | 2.5.4 | PyPI, `numpy-2.5.4-cp313-cp313-win_amd64.whl` | `8b4d2fd2d34e5f8c9235ee787de5631a37a28402b15cb80814df973d2be54129` | BSD-3-Clause, with bundled libraries under their own terms |
| pybind11 | 3.1.0 | `https://github.com/pybind/pybind11/archive/refs/tags/v3.1.0.tar.gz` | `EF712655692A2E9BF7BB7874C022564A45F91D847DDEE987E720CD9E28849665` | BSD-style |

## Installed separately, at these versions

Not checksummed by this project, because each comes from its vendor's installer.

| Tool | Version | Source |
| --- | --- | --- |
| Qt | 6.12.0, `qt.qt6.6120.win64_msvc2022_64` | Qt Maintenance Tool, open-source licence (LGPLv3) |
| Visual Studio Build Tools | 2022 17.14, MSVC 19.44, Windows SDK 10.0.26100.0 | `winget`, `Microsoft.VisualStudio.2022.BuildTools` |
| CMake | 4.4.4 | `winget`, `Kitware.CMake` |
| Ninja | 1.13.2 | `winget`, `Ninja-build.Ninja` |

## Qt modules

The app links only `Core`, `Gui`, `Widgets` and, for tests, `Test`, all dynamically. Under the free licence, do not add Qt Charts, Qt Graphs, Qt Quick 3D, Qt HTTP Server or Qt gRPC, and do not link statically, without checking the licence first.
