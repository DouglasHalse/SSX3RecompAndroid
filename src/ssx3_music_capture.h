#ifndef SSX3_MUSIC_CAPTURE_H
#define SSX3_MUSIC_CAPTURE_H

#include <stdint.h>

/*
 * Music director capture.
 *
 * One JSONL line per signal the soundtrack reacts to:
 *
 *   {"t":1234,"k":"event","va":"0x0018A6E0","s":"Race","a":[7,0,0,0]}
 *
 * t  milliseconds since the capture started
 * k  which signal: event, value, reset, trigger, marker
 * va the guest function it came through
 * s  the screen the port thinks it is on, when known
 * a  the argument candidates that were readable at the call (ecx, edx, [esp+4],
 *    [esp+8]) -- the calling convention of these functions is not documented by
 *    the game, so the reader picks the argument that makes sense; tools/
 *    analyse_capture.py reports which slot behaves like what.
 *
 * Off unless RECOMP_MUSIC_CAPTURE=<path> is set. See docs/capture.md.
 */
int  mx_on(void);
void mx_sig(const char *kind, uint32_t va, const uint32_t *args, int n);
void mx_note(const char *kind, const char *text);
void mx_set_screen_provider(const char *(*fn)(void));
void mx_close(void);

#endif
