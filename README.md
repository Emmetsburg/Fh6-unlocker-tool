# FH6 Car Tool

Standalone Windows GUI for adding and removing cars in Forza Horizon 6.

The tool builds as `GUIInjector.exe`. It does not inject a separate DLL; the ImGui interface, car database, process access, signature scanning, and SQL execution code are compiled into the EXE.

## Features

- Embedded searchable car database with aliases
- Add selected cars to the garage
- View and filter the current garage
- Remove cars from the garage
- Uses the local `game/` and `process/` DBDUMPER source files directly

## Build

Prerequisites:

- Visual Studio 2022 with Desktop development with C++
- Windows 10/11 SDK
- ImGui in `dependencies/imgui`
- nlohmann/json in `dependencies/json`

Build with Visual Studio:

```bat
msbuild FH6CarAdder.sln /p:Configuration=Release /p:Platform=x64
```

Build with CMake:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Visual Studio output is copied to `bin/Release/`.
Build output is ignored by git and should be regenerated locally.

## Usage

1. Start Forza Horizon 6.
2. Run `bin/Release/GUIInjector.exe` as Administrator.
3. Use `Check Game` to confirm the process is visible.
4. Search for a car and click `Add Car`, or use `My Garage` to remove one.

The Release EXE embeds the car database, so `bin/Release/GUIInjector.exe` does not need `cars.json` next to it.

## Project Structure

```text
GUIInjector/      Standalone ImGui Windows app
Injector/         Shared process lookup helper
game/             Local DBDUMPER database helpers
process/          Local DBDUMPER process/signature helpers
dependencies/     ImGui and nlohmann/json
cars.json         Source car database embedded into the EXE at build time
```

## Notes

- Run as Administrator so the tool can open the FH6 process.
- Game updates can break signatures and require updating the DBDUMPER helper code.
- Back up saves before modifying garage data.
