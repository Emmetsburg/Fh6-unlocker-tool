# FH6 Car Tool

## Features

- Embedded searchable car database with aliases
- Add selected cars to your garage
- View, search, and filter your current garage
- Remove cars from your garage
- Add cars from traffic like buses, trucks and vans.
- Temporarily borrow or replace selected garage rows
- Misc database actions such as free cars, free upgrades, and clearing new car tags

## Build

Prerequisites:

- Visual Studio 2022 with Desktop development with C++
- Windows 10/11 SDK
- ImGui in `dependencies/imgui`
- nlohmann/json in `dependencies/json`

Build with Visual Studio:

`msbuild FH6CarAdder.sln /p:Configuration=Release /p:Platform=x64`

Build with CMake:

`cmake -S . -B build -G "Visual Studio 17 2022" -A x64`

`cmake --build build --config Release`

Visual Studio output is copied to `bin/Release/`.
CMake output is copied to `build/bin/Release/`.

## Usage

1. Start Forza Horizon 6.
2. Run `GUIInjector.exe` as Administrator.
3. Click the Lighting bolt button to confirm FH6 is running.
4. Search for a car and click `Add Car`, or open `My Garage` to view and remove cars.
5. Use the `Traffic Vehicles` tab for supported traffic car options.
6. Use the `Misc` tab for extra database actions.

The EXE embeds the car database, so `cars.json` does not need to be next to the Release build.

## Notes

- Run as Administrator so the tool can open the FH6 process.

Disclaimer
I am not responsible for anything that happens when you use this software.
