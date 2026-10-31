/*
 * in_game_menu.h -- the options screen, which the presenter draws for us.
 */
#ifndef SSX3_IN_GAME_MENU_H
#define SSX3_IN_GAME_MENU_H

/* Paint the screen into the presenter's hooks. Call once, before the window
 * loop starts (src/main.c, src/android/sdl_main.c). */
void ssx3_InGameMenuInstall(void);

int ssx3_InGameMenuOpen(void);

#endif /* SSX3_IN_GAME_MENU_H */
