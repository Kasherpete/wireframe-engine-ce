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
// With no world open (titleNoWorld) a message says where to get one, over a black background
// (main draws no scene), with a Continue button to the main menu.
// The pause menu (titlePause, [clear] in game) shares the screens: Back to Game, Settings, Save &
// Quit, Quit Without Saving, over the frozen world. [clear] there resumes.
#ifndef TITLE_H
#define TITLE_H

#include <stdint.h>

#define TITLE_STAY  0
#define TITLE_PLAY  1
#define TITLE_QUIT  2           // quit without saving
#define TITLE_RESUME     3      // pause menu: back to the game
#define TITLE_SAVE_QUIT  4      // pause menu: save the position, then quit
#define TITLE_START      5      // the low memory notice: play the world opened for TITLE_PLAY

// reads the keys and turns the camera for dt clock ticks: TITLE_STAY, TITLE_PLAY or TITLE_QUIT
uint8_t titleUpdate(uint24_t dt);
// samples the keys between titleUpdate calls (main calls it a few times a frame), so the menus
// do not miss a key tapped for less than a frame
void titlePoll(void);
// after TITLE_PLAY: the world_list index chosen
uint8_t titleWorld(void);
// the chosen world could not be opened: the world list stays, with msg (a static string) under it
void titleError(const char *msg);
// at startup with no world open: the message screen first. reason: a worldError message, or NULL
// when there is no world at all
void titleNoWorld(const char *reason);
// after TITLE_PLAY, if the world cannot be saved (worldCanSave): a notice that it can be played
// but not saved (need and avail: bytes), then TITLE_START, or [clear] back to the world list
void titleLowMemory(uint24_t need, uint24_t avail);
// opens the pause menu (until TITLE_RESUME); keys held now count as already pressed. Without
// can_save it says under the buttons that Save & Quit cannot save
void titlePause(uint8_t can_save);
// after drawSpans: the title and the buttons (the frame needs deinterlaceFrame(GFX_LCD_HEIGHT))
void titleDraw(void);

#endif
