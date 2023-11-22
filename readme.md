# Overview

This is just a simple algorithm for the TI-84+ CE to render simple voxels quickly and dynamically. For now it is just an engine/algorithm, but once it is in a ready stage, it will be used to make a couple games.

![gif](https://cdn.discordapp.com/attachments/1168344250908418078/1168599163961868358/screen.gif) ![image](https://cdn.discordapp.com/attachments/772599413247442948/1168328775025561660/wireframe1.png)

### Setup

In `main.c`, you will find some `#define`'s at the top. Just configure them to your needs. If you change any of the Z render distances or FOV, make sure to run `powAZx50` found in /utils. This script pre-calculates any needed values. If you change anything regarding the Z render distance, FOV, or `powAZx50` variables within `main.c`, make sure to double check the Python script since it automatically handles constants in `main.c`.

### TODO

 - [ ] Add algorithm for blocks
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

***v0.2.7*** - Add `drawBasePlane()` function, add a demo file, update coordinate list handling

***v0.2.6*** - Fully implement planes, optimize existing algorithms, updated trapezoid routine, other small changes

***v0.2.5*** - Fix clipping algorithm, improve `powAZx50.py` script, fix render algorithm, add coordinate view, add basic `drawPlane()` function - only able to lay flat.

 - Note: v0.2.4 was skipped...I did a funny and accidentally labelled it as v0.2.5.

***v0.2.3*** - Add fully functioning `sortCoordinateList()` method, optimize filling algorithm, add functionality for drawing only visible parts of an object.

***v0.2.2*** - Code cleanup, remove excess code, optimizations, removed vertex cache, currently the fastest version.

***v0.2.1*** - Added cache for common vertices. May be slower? We'll see, for now the cache is set to 1.

***v0.2.0*** - **Massive** speed boost, over 6x speed

***v0.1.0*** - First demo
