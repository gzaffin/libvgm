## libvgm
fork of [ValleyBell / libvgm](https://github.com/ValleyBell/libvgm), here the focus is building from source in Windows 10/Windows 11 with Microsoft's Visual Studio or MSYS2.

# How to build
## Microsoft's Visual Studio
Use Microsoft's Visual Studio, Microsoft vcpkg, git, CMake. [Install Visual Studio](https://learn.microsoft.com/en-us/visualstudio/install/install-visual-studio?view=vs-2022) .  
Use vcpkg cross-platform C/C++ package manager. [Install and use packages with vcpkg](https://learn.microsoft.com/en-us/vcpkg/commands/install) .  

As it can be seen from CMakeLists.txt, the following two packages are required:
1. libiconv
2. zlib

A third dependency for Windows audio drivers is Microsoft C++/WinRT, which comes with Visual Studio since Visual Studio installs and manages Microsoft Windows SDK .

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

## MSYS2 minGW-w64 UCRT64
Use MSYS2, pacman, minGW-w64 UCRT64 environment, GNU gcc toolchain, git, ninja, CMake. [Install MSYS2 minGW-w64 UCRT64](https://www.msys2.org/#installation) .  
Use MSYS2 pacman package manager .  

Add dependency packages intto minGW-w64 UCRT64 environment:
1. libiconv (mingw-w64-ucrt-x86_64-libiconv)
2. zlib (mingw-w64-ucrt-x86_64-zlib)
3. cppwinrt (mingw-w64-ucrt-x86_64-cppwinrt)

Install packages with pacman utility
```
$ pacman -S mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-libiconv mingw-w64-ucrt-x86_64-zlib mingw-w64-ucrt-x86_64-cppwinrt
```
Clone repository and compile sources
```
$ git clone https://github.com/gzaffin/libvgm.git
$ cd libvgm
$ mkdir build
$ cd build
$ cmake -G Ninja ..
$ cmake --build . --config Release
```
