#ifndef SSX3_SETTINGS_PAGE_H
#define SSX3_SETTINGS_PAGE_H

/* Installs the settings page as a page of the game's own Options screen:
 * draw hook, pad filter, key hook. */
void ssx3_SettingsPageInstall(void);

/* Which row of the game's Options list is selected, or -1 when that list is
 * not the screen on show (implemented in ssx3_overrides.c, where the screen
 * state is visible). */
int ssx3_OptionsRowSelected(void);

/* Rewrite a row label the game is drawing, in place. Returns how many copies
 * it changed; used to turn Credits into the entry to this page. */
int ssx3_RelabelRow(const char *from, const char *to);

#endif /* SSX3_SETTINGS_PAGE_H */
