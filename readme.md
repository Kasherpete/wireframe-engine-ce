# Overview

This is just a simple algorithm for the TI-84+ CE to render simple voxels quickly and dynamically. For now it is just an engine/algorithm, but once it is in a ready stage, it will be used to make a couple games.

![gif](https://cdn.discordapp.com/attachments/1168344250908418078/1168599163961868358/screen.gif) ![image](https://cdn.discordapp.com/attachments/772599413247442948/1168328775025561660/wireframe1.png)

### Setup

In `main.c`, you will find some `#define`'s at the top. Just configure them to your needs. If you change any of the Z render distances or FOV, make sure to run `powAZx50` found in /utils. This script pre-calculates any needed values. If you change anything regarding the Z render distance, FOV, or `powAZx50` variables within `main.c`, make sure to double check the Python script since it automatically handles constants in `main.c`.

### TODO

 - [x] ***Check if cache really helps performance***

 - [x] Add common voxel coordinate lookup table to optimize rendering
 - [ ] Add functionality to render planes and polygons
 - [ ] Darken colors for the further away from the camera it is (volumetric fog)
 - [ ] Optimize algorithm (sacrifice of accuracy is acceptable, and may make the scene loop less calculated and and definitive - more natural)
 - [x] Find a way to render everything unclipped (will need help!!)
 - [ ] ~~Render screen coordinates using 8 bit values instead of 16 - will allow the right side of the screen to be used for stats~~

<br>

 - [x] *Unplanned*: Implement a lookup table for both powAZx50 variables - which are calculated every voxel
### Changelog

***v0.2.2*** - Code cleanup, remove excess code, optimizations, removed vertex cache, currently the fastest version.

***v0.2.1*** - Added cache for common vertices. May be slower? We'll see, for now the cache is set to 1.

***v0.2.0*** - **Massive** speed boost, over 6x speed

***v0.1.0*** - First demo
