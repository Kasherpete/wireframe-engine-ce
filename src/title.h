// The title screen: "CALCRAFT" and a menu (play, settings, quit) over the world, seen from the
// spawn (playerSpawn) while the camera turns slowly right. Up/down select, [2nd]/[enter] choose (on
// release, so the key does not also make the player jump), [clear] goes back (quits from the main
// menu).
// - Play: the worlds worldScan found (world.h), most recently played first; choosing one returns
//   TITLE_PLAY with it in titleWorld().
//   Below the list: New (an empty world, named first) and Rename (the world last selected), both
//   on a name screen (keys type letters as with alpha lock; [alpha] switches capitals, small
//   letters, digits). The AppVars they leave in RAM are archived at exit (worldArchivePending).
// - Settings: render distance, fog, the FPS / frame time HUD (applied at once: camera.h, hud.h),
//   and where to star the project.
#ifndef TITLE_H
#define TITLE_H

#include <stdint.h>

#define TITLE_STAY  0
#define TITLE_PLAY  1
#define TITLE_QUIT  2

// reads the keys and turns the camera for dt clock ticks: TITLE_STAY, TITLE_PLAY or TITLE_QUIT
uint8_t titleUpdate(uint24_t dt);
// samples the keys between titleUpdate calls (main calls it a few times a frame), so the menus
// do not miss a key tapped for less than a frame
void titlePoll(void);
// after TITLE_PLAY: the world_list index chosen
uint8_t titleWorld(void);
// the chosen world could not be opened: the world list stays, with msg (a static string) under it
void titleError(const char *msg);
// after drawSpans: the title and the buttons (the frame needs deinterlaceFrame(GFX_LCD_HEIGHT))
void titleDraw(void);

#endif
