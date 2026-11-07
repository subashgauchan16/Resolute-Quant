# Native dependency pins

Every version here is exact. `tools/setup_runtime.ps1` and `CMakeLists.txt` refuse a download whose checksum differs.

| Dependency | Version | Source | SHA-256 | Licence |
| --- | --- | --- | --- | --- |
| Python (private runtime) | 3.13.17 | `https://www.nuget.org/api/v2/package/python/3.13.17` | `DCE409744B088F94C6E37BA41F8100C384C27E029E843237364C602D30D475E5` | PSF |
| Python packages, including NumPy 2.5.4, pandas 3.1.1 and yfinance 1.8.0 | The 22 pins in `requirements.txt` at the repository root | PyPI | Every wheel's hash is in [python/requirements.lock](python/requirements.lock) | Each package's own licence |
| pybind11 | 3.1.0 | `https://github.com/pybind/pybind11/archive/refs/tags/v3.1.0.tar.gz` | `EF712655692A2E9BF7BB7874C022564A45F91D847DDEE987E720CD9E28849665` | BSD-style |

## SQLite

`tools/setup_runtime.ps1` replaces the SQLite library inside the private Python. Python 3.13.17 ships 3.50.4, which is in the range of [a documented WAL reset bug](https://www.sqlite.org/wal.html) fixed in 3.51.3 and backported to 3.50.7. SQLite is in the public domain.

| Version | Source | SHA-256 |
| --- | --- | --- |
| 3.54.1 | `https://www.sqlite.org/2026/sqlite-dll-win-x64-3540100.zip` | `F436F53AD8F965AF43F3817B42C99C0689E6787F6B7ACE208FE38DAB7FFC4971` |

## Bundled fonts

Committed in `assets/fonts/`, from `https://github.com/google/fonts` (`ofl/ibmplexmono` and `ofl/ibmplexsanscondensed`). IBM Plex is under the SIL Open Font License 1.1, in `assets/fonts/OFL.txt`.

| File | SHA-256 |
| --- | --- |
| `IBMPlexMono-Regular.ttf` | `6A3412F058C7D8DFD9170C41E85ADE48E5156ECB89356110CA57A0A27734AF46` |
| `IBMPlexMono-Medium.ttf` | `A9B4C49BB299E05B5F6C481E7FB5E78943D2793249A0C8874AB574A2D1EA6755` |
| `IBMPlexMono-SemiBold.ttf` | `D3C38E55C78F5B0F28009FDDBA4834EC503278936A5986032424C9BD2D23AA46` |
| `IBMPlexSansCondensed-Medium.ttf` | `426350C298277F7F9D1A93956572799CA3D16E2D43E7F60EEC8382BCD795EC30` |
| `IBMPlexSansCondensed-SemiBold.ttf` | `A081CA60548AEA14100757A2D6FE71CA60FD90EBCE779C9A8AE5A0C8FB4526D4` |

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
