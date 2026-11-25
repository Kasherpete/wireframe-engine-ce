// The title screen: "ti-craft" and a menu (play, settings, quit) over the world, seen from the
// spawn (playerSpawn) while the camera turns slowly right. Up/down select, [2nd]/[enter] choose (on release,
// so the key does not also make the player jump).
#ifndef TITLE_H
#define TITLE_H

#include <stdint.h>

#define TITLE_STAY  0
#define TITLE_PLAY  1
#define TITLE_QUIT  2

// reads the keys and turns the camera for dt clock ticks: TITLE_STAY, TITLE_PLAY or TITLE_QUIT
uint8_t titleUpdate(uint24_t dt);
// after drawSpans: the title and the buttons (the frame needs deinterlaceFrame(GFX_LCD_HEIGHT))
void titleDraw(void);

#endif
