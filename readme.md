# Overview

This is just a simple algorithm for the TI-84+ CE to render simple voxels quickly and dynamically. For now it is just an engine/algorithm, but once it is in a ready stage, it will be used to make a couple games.

![gif](https://cdn.discordapp.com/attachments/1168344250908418078/1168599163961868358/screen.gif) ![image](https://cdn.discordapp.com/attachments/772599413247442948/1168328775025561660/wireframe1.png)

### Setup

In `main.c`, you will find some `#define`'s at the top. Just configure them to your needs. If you change any of the Z render distances, make sure to regenerate the `powAZx50` array by using the Python script found in /utils.

### TODO

 - [ ] Add common voxel coordinate lookup table to optimize rendering
 - [ ] Add functionality to render planes and polygons
 - [ ] Darken colors for the further away from the camera it is (volumetric fog)
 - [ ] Optimize algorithm (sacrifice of accuracy is acceptable, and may make the scene loop less calculated and and definitive - more natural)
 - [ ] Find a way to render everything unclipped (will need help!!)
 - [ ] Render screen coordinates using 8 bit values instead of 16 - will allow the right side of the screen to be used for stats

<br>

 - [x] *Unplanned*: Implement a lookup table for both powAZx50 variables - which are calculated every voxel
### Changelog

***v0.1.0*** - First demo
