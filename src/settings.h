/*
 * settings.h -- the port's own options, the file they live in, and the screen
 * that edits them.
 *
 * The launcher used to be the only place these could be changed; every one of
 * them is an environment variable the port already reads (RECOMP_*), so the
 * in-game screen edits that variable, writes the file, and the rest of the
 * port needs no change. Values the screen can apply without tearing down the
 * renderer (draw distance, HUD placement, the frame counter) take effect on
 * the next frame; the rest are marked in the screen as needing a restart.
 */
#ifndef SSX3_SETTINGS_H
#define SSX3_SETTINGS_H

/* The screen can change it while the game runs. */
#define SSX3_LIVE       1u
/* Only offered where it means something. */
#define SSX3_WIN_ONLY   2u
#define SSX3_AND_ONLY   4u

typedef struct {
    const char *key;            /* RECOMP_*: the variable and the file key */
    const char *label;          /* on screen */
    const char *help;           /* one line under the list */
    const char *const *values;  /* env strings, one per choice */
    const char *const *names;   /* the same choices, as shown */
    int         count;
    int         def;            /* choice when nothing is set */
    int         def_android;    /* Android's own default */
    unsigned    flags;
} Ssx3Setting;

extern const Ssx3Setting ssx3_settings[];
extern const int ssx3_settings_count;

/* Read the file and put it in the environment, before the port reads any of
 * it. An existing variable the file does not name is left alone, so a one-off
 * RECOMP_* on the command line still works. */
void ssx3_SettingsLoad(void);

/* The port's settings file: ssx3.cfg beside the executable (Windows) or in
 * the game folder (everywhere else). */
const char *ssx3_SettingsFile(void);

/* The choice in use for row `i`, and which rows the file set. */
int  ssx3_SettingsGet(int i);
int  ssx3_SettingsFromFile(int i);

/* Choose a value: applied now, written out now. */
void ssx3_SettingsSet(int i, int choice);

/* Bumped on every change, so a consumer that caches a value can tell. */
unsigned ssx3_SettingsGeneration(void);

/* Rows offered on this platform, in order. */
int ssx3_SettingsRowCount(void);
int ssx3_SettingsRowAt(int index);

#endif /* SSX3_SETTINGS_H */
