## libvgm
fork of [ValleyBell / libvgm](https://github.com/ValleyBell/libvgm), here the focus is building from source with Microsoft's Visual Studio MSVC + vcpkg + CMake in Windows 10/Windows 11.

# How to build
Use Microsoft's Visual Studio. [Install Visual Studio](https://learn.microsoft.com/en-us/visualstudio/install/install-visual-studio?view=vs-2022) .
Use vcpkg cross-platform C/C++ package manager. [Install and use packages with vcpkg](https://learn.microsoft.com/en-us/vcpkg/commands/install) .

As it can be seen from CMakeLists.txt, the following two packages are required:
1. libiconv
2. zlib

Install packages with vcpkg with [vcpkg install <package>...](https://learn.microsoft.com/en-us/vcpkg/commands/install) command
```
PS C:\<PATH>\vcpkg>vcpkg install libiconv:x64-windows zlib:x64-windows
```
Clone repository and compile sources
```
PS C:\<PATH>>git clone https://github.com/gzaffin/libvgm.git
PS C:\<PATH>>cd libvgm
PS C:\<PATH>\libvgm>mkdir build
PS C:\<PATH>\libvgm>cd build
PS C:\<PATH>\libvgm\build>cmake -G "Visual Studio 17 2022" -A x64 -T host=x64 -D CMAKE_TOOLCHAIN_FILE=C:/<PATH>/vcpkg/scripts/buildsystems/vcpkg.cmake ..
PS C:\<PATH>\libvgm\build>cmake --build . --config Release
```

