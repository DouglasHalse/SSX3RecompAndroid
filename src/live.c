/*
 * Settings changed while playing, to try them on a problem as it happens:
 *   F4  HD textures on / off
 *   F5  draw distance: Normal (1x) -> Far (1.5x) -> Maximum (2x)
 *   F6  rider shadows on / off
 * Each change is shown on screen for a few seconds and logged as
 * INFO [SETTINGS] "live: ..." (the launcher's choice is what the next start
 * uses again). Polled from mark.c's input thread.
 */
#include "log.h"
#include <stdio.h>

extern int xbox_FramebufferKeyPresses(int vk);
extern void xbox_FramebufferOsd(const char *text);
extern int ssx3_LiveHdTextures(int on);              /* src/vk/vkr_backend.c */
extern int ssx3_LiveHdTexturesOn(void);
extern void ssx3_LiveShadows(int on);
extern float ssx3_LiveDrawDistNext(void);            /* ssx3_overrides.c */

#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75

static void say(const char *what, const char *key)
{
    char osd[64];
    snprintf(osd, sizeof osd, "%s (%s)", what, key);
    xbox_FramebufferOsd(osd);
    log_msg(LOG_INFO, "SETTINGS", "live: %s (%s)", what, key);
}

void ssx3_LiveKeysPoll(void)
{
    static int shadows = 1;
    int k;
    for (k = xbox_FramebufferKeyPresses(VK_F4); k > 0; k--)
        say(ssx3_LiveHdTextures(!ssx3_LiveHdTexturesOn()) ? "HD textures on" : "HD textures off", "F4");
    for (k = xbox_FramebufferKeyPresses(VK_F5); k > 0; k--) {
        float d = ssx3_LiveDrawDistNext();
        say(d < 1.25f ? "draw distance Normal 1x" : d < 1.75f ? "draw distance Far 1.5x"
            : "draw distance Maximum 2x", "F5");
    }
    for (k = xbox_FramebufferKeyPresses(VK_F6); k > 0; k--) {
        shadows = !shadows;
        ssx3_LiveShadows(shadows);
        say(shadows ? "rider shadows on" : "rider shadows off", "F6");
    }
}
