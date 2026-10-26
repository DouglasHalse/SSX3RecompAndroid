/* JNI entry points for the launcher's importer (see xiso.c).
 *
 * The Java side passes the descriptor the system file picker handed it, so the
 * image is read where it lives: a redump image is 405 MiB of video partition
 * plus the game, and copying it first would need the space twice on a phone.
 *
 * Progress is polled from Java (nativeDone/nativeTotal) rather than pushed
 * through an upcall: the extractor runs on a plain thread and nothing has to
 * find the right JNIEnv.
 */
#include <android/log.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "xiso.h"

#define LOG_TAG "SSX3IMPORT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static volatile unsigned long long g_done, g_total;
static char g_error[256];
static char g_probe[512];
static int  g_is_pal;

static void on_progress(void *user, const char *path, unsigned long long done,
                        unsigned long long total)
{
    static unsigned long long logged;

    (void)user;
    g_done = done;
    g_total = total;
    /* Every 128 MB, and always the last one: if the process dies mid-extract,
     * logcat says how far it got and on which file. */
    if (done == 0 || total == 0 || done >= logged + (128u << 20) || done == total) {
        LOGI("extract progress %llu/%llu MB: %s", done >> 20, total >> 20, path);
        logged = done;
    }
}

/* Size in bytes, for the launcher's "x / y MB". */
JNIEXPORT jlong JNICALL
Java_com_douglashalse_ssx3_LauncherActivity_nativeDone(JNIEnv *env, jobject thiz)
{
    (void)env; (void)thiz;
    return (jlong)g_done;
}

JNIEXPORT jlong JNICALL
Java_com_douglashalse_ssx3_LauncherActivity_nativeTotal(JNIEnv *env, jobject thiz)
{
    (void)env; (void)thiz;
    return (jlong)g_total;
}

JNIEXPORT jstring JNICALL
Java_com_douglashalse_ssx3_LauncherActivity_nativeError(JNIEnv *env, jobject thiz)
{
    (void)thiz;
    return (*env)->NewStringUTF(env, g_error[0] ? g_error : "unknown error");
}

JNIEXPORT jboolean JNICALL
Java_com_douglashalse_ssx3_LauncherActivity_nativeProbeIsPal(JNIEnv *env, jobject thiz)
{
    (void)env; (void)thiz;
    return g_is_pal ? JNI_TRUE : JNI_FALSE;
}

/* Inspect the image and describe it in one line for the player. */
JNIEXPORT jstring JNICALL
Java_com_douglashalse_ssx3_LauncherActivity_nativeProbe(JNIEnv *env, jobject thiz, jlong fd)
{
    xiso_info info;
    char msg[256] = "";

    (void)thiz;
    g_error[0] = 0;
    g_is_pal = 0;
    LOGI("probe: fd=%d", (int)fd);
    if (xiso_probe((int)fd, &info, msg, sizeof msg) != 0) {
        LOGI("probe failed: %s", msg);
        snprintf(g_probe, sizeof g_probe, "%s", msg[0] ? msg : "not an Xbox disc image");
        return (*env)->NewStringUTF(env, g_probe);
    }
    LOGI("probe ok: image=%llu MB descriptor=0x%X title=0x%08X xbe=%u pal=%d",
         (unsigned long long)(info.image_size >> 20), (unsigned)info.vd_offset,
         (unsigned)info.title_id, (unsigned)info.xbe_size, info.matches_this_port);
    g_is_pal = info.matches_this_port;
    if (info.matches_this_port)
        snprintf(g_probe, sizeof g_probe,
                 "PAL disc of SSX 3 (title %08X), %llu MB.\n"
                 "Ready to extract: default.xbe and the data folder, about 2.1 GB.",
                 (unsigned)info.title_id,
                 (unsigned long long)(info.image_size / (1024 * 1024)));
    else
        snprintf(g_probe, sizeof g_probe,
                 "This image is not the disc this port was built from (title %08X, XBE %u bytes).\n"
                 "The recompiled code is the PAL build: a USA or Japanese disc has different code "
                 "at the same addresses and will not run.",
                 (unsigned)info.title_id, (unsigned)info.xbe_size);
    return (*env)->NewStringUTF(env, g_probe);
}

/* Extract every file into destDir. 0 on success; the reason is in nativeError. */
JNIEXPORT jint JNICALL
Java_com_douglashalse_ssx3_LauncherActivity_nativeExtract(JNIEnv *env, jobject thiz,
                                                          jlong fd, jstring dest)
{
    const char *dest_dir;
    int rc;

    (void)thiz;
    g_done = g_total = 0;
    g_error[0] = 0;
    if (!dest) {
        snprintf(g_error, sizeof g_error, "no destination folder");
        return -1;
    }
    dest_dir = (*env)->GetStringUTFChars(env, dest, NULL);
    if (!dest_dir) {
        snprintf(g_error, sizeof g_error, "out of memory reading the path");
        return -1;
    }
    LOGI("extract: fd=%d dest=%s", (int)fd, dest_dir);
    rc = xiso_extract((int)fd, dest_dir, on_progress, NULL, g_error, sizeof g_error);
    LOGI("extract %s: rc=%d done=%llu MB/%llu MB %s", rc == 0 ? "ok" : "FAILED", rc,
         g_done >> 20, g_total >> 20, rc == 0 ? "" : g_error);
    (*env)->ReleaseStringUTFChars(env, dest, dest_dir);
    if (rc != 0 && !g_error[0])
        snprintf(g_error, sizeof g_error, "extraction failed (off the end of the image?)");
    /* tell the launcher the real total, so the bar lands on 100% */
    if (rc == 0 && g_total == 0)
        g_total = g_done;
    return rc;
}

/* The launcher owns the descriptor only until it is done with it. */
JNIEXPORT void JNICALL
Java_com_douglashalse_ssx3_LauncherActivity_nativeClose(JNIEnv *env, jobject thiz, jlong fd)
{
    (void)env; (void)thiz;
    if (fd >= 0)
        close((int)fd);
}
