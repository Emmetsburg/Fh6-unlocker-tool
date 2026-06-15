# Build Instructions

## Visual Studio

1. Install Visual Studio 2022 with Desktop development with C++.
2. Build Release x64:

```bat
msbuild FH6CarAdder.sln /p:Configuration=Release /p:Platform=x64
```

Expected output:

- `bin/Release/GUIInjector.exe`

The `bin/` and `build/` directories are generated locally and ignored by git.

## CMake

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Expected output:

- `build/bin/Release/GUIInjector.exe`

## Troubleshooting

- If ImGui headers are missing, make sure `dependencies/imgui` exists.
- If `json.hpp` is missing, make sure `dependencies/json/include/nlohmann/json.hpp` exists.
- If process access fails at runtime, run the EXE as Administrator and start FH6 first.
