# Third-party components

GoodbyeDPI Auto is an independent application and is not an official interface of the upstream projects.

| Component | Version/source | Usage | License |
|---|---|---|---|
| GoodbyeDPI | 0.2.3rc3-2 binary; 0.2.3rc3 source | Embedded DPI engine, extracted unchanged at installation | Apache-2.0 |
| WinDivert | 2.2 binary shipped by GoodbyeDPI; v2.2.0 source tag | Embedded packet driver/library, extracted unchanged | LGPL-3.0 |
| Dear ImGui | 1.91.9b | C++ immediate-mode user interface; Win32 and DX11 backends | MIT |
| nlohmann/json | 3.11.3 | JSON recovery journal parsing/serialization | MIT |

The original license texts are included under `licenses/` and `vendor/`. Upstream source archives are included under `upstream-source/`. The WinDivert driver keeps its valid upstream Authenticode signature; GoodbyeDPI Auto does not modify it.
