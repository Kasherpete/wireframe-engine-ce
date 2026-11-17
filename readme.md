# Overview

This is just a simple algorithm for the TI-84+ CE to render simple voxels quickly and dynamically. For now it is just an engine/algorithm, but once it is in a ready stage, it will be used to make a couple games.

![gif](https://cdn.discordapp.com/attachments/1168344250908418078/1177019478593241098/tunneldemo.gif?ex=6ad0b10f&is=6acf5f8f&hm=825f474176c56a3a043ea621bddc756477e9f0d6733d96df017c3b756bf06e6d&)

![gif](https://cdn.discordapp.com/attachments/1168344250908418078/1177019478370959410/mcdemo.gif?ex=6ad0b10f&is=6acf5f8f&hm=4c2a8e811679fa6be8335baec4dd97533533daafc148385596095d7847c41b40&) 

### Setup

In `main.c`, you will find some `#define`'s at the top. Just configure them to your needs. If you change any of the Z render distances or FOV, make sure to run `powAZx50` found in /utils. This script pre-calculates any needed values. If you change anything regarding the Z render distance, FOV, or `powAZx50` variables within `main.c`, make sure to double check the Python script since it automatically handles constants in `main.c`.

**NOTE:** You will need at least version v22 of the ez80-clang LLVM compiler. As of this writing, only version v19 is bundled with CEdev, meaning you will need to install either the nightly toolchain or nightly compiler. If you already have the CEdev toolchain installed, you can download the [nightly compiler release package](https://github.com/CE-Programming/llvm-project/releases/nightly) and extract both ez80-clang and ez80-link to the `CEdev/bin` folder.

If you run into issues with VScode's C/C++ extension pack, it's probably because of `c_cpp_properties.json` in the `.vscode` directory. You'll need to edit this file and include the path of your personal CEdev location. From the CE Toolchain docs:

```
## Open IntelliSense Configurations Screen

Open the Command Palette (Ctrl+Shift+P) and select C/C++: Edit configurations (UI).

## Set Compiler Options

Set the value of the “Compiler path” option to be the path to the ez80-clang executable in the bin directory of your toolchain installation, e.g. /home/john/CEdev/bin/ez80-clang. Also set the “Include path” option to the path to the include directory (e.g. /home/john/CEdev/include). Make sure to remove/replace the original value of ${default}, rather than just adding the path at the end.
```

### TODO


 - [x] Add functionality to render planes and polygons
 - [ ] Darken colors for the further away from the camera it is (volumetric fog)
 - [ ] Optimize algorithm (sacrifice of accuracy is acceptable, and may make the scene loop less calculated and and definitive - more natural)
 - [ ] Clipping alorithm for panels
 - [x] Add `drawPlane()`
 - [ ] Look into smoother movements (I'll see what I can do. Right now this is the most important thing.)
 - [x] Render polygon instead of lines for panel, plane
 - [x] Make variables into constants
 - [ ] Add `drawPillar()`
 - [ ] Direction control
 - [x] Write custom drawing routine for trapezoids
 - [ ] Time delta for player movement
 - [x] Implement clipping detection for planes
 - [x] Add render distance detection for planes
 - [x] Add rotation control for planes
 - [x] Set palette to remove compiler warnings
 - [x] Find a way to render everything unclipped (will need help!!)
 - [x] Check if cache really helps performance
 - [x] Add common voxel coordinate lookup table to optimize rendering
 - [ ] ~~Render screen coordinates using 8 bit values instead of 16 - will allow the right side of the screen to be used for stats~~

<br>

 - [x] *Unplanned*: Implement a lookup table for both powAZx50 variables - which are calculated every voxel
### Changelog

***v0.2.7*** - Added DrawBasePlane, multiple bug fixes, small optimizations, and code changes

***v0.2.6*** - Fully implement planes, optimize existing algorithms, updated trapezoid routine, other small changes

***v0.2.5*** - Fix clipping algorithm, improve `powAZx50.py` script, fix render algorithm, add coordinate view, add basic `drawPlane()` function - only able to lay flat.

 - Note: v0.2.4 was skipped...I did a funny and accidentally labelled it as v0.2.5.

***v0.2.3*** - Add fully functioning `sortCoordinateList()` method, optimize filling algorithm, add functionality for drawing only visible parts of an object.

***v0.2.2*** - Code cleanup, remove excess code, optimizations, removed vertex cache, currently the fastest version.

***v0.2.1*** - Added cache for common vertices. May be slower? We'll see, for now the cache is set to 1.

***v0.2.0*** - **Massive** speed boost, over 6x speed

***v0.1.0*** - First demo
