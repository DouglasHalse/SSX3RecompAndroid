package com.douglashalse.ssx3;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.ParcelFileDescriptor;
import android.text.method.ScrollingMovementMethod;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;

/**
 * The first screen: import the game from the player's own disc image, then play.
 *
 * SSX 3's data cannot ship with the port, and the only copy a player has is
 * their own disc. So this asks for the image through the system file picker,
 * checks it against the PAL disc the port was recompiled from, and extracts it
 * into the app's own storage (no permission needed there) with a progress bar.
 *
 * The extraction itself is C (src/android/xiso.c) behind libssx3import.so: it
 * reads the image in place through the picked descriptor, so a 1.4 GB image
 * does not have to be copied first.
 */
public class LauncherActivity extends Activity {

    static {
        System.loadLibrary("ssx3import");
    }

    private static final int PICK_IMAGE = 1;

    private TextView status;     // one line: ready or not
    private TextView detail;     // what was found / what went wrong
    private ProgressBar bar;
    private Button pick;
    private Button play;

    private final Handler handler = new Handler();
    private Thread worker;
    private File gameDir;

    /* ---- native side (xiso.c) ---- */
    private native String nativeProbe(long fd);       /* "PAL disc: ..." or the reason it is not */
    private native boolean nativeProbeIsPal();        /* did the XBE match this port's build? */
    private native int  nativeExtract(long fd, String destDir);  /* 0 on success */
    private native long nativeDone();
    private native long nativeTotal();
    private native String nativeError();
    private native void nativeClose(long fd);

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(buildUi());
        gameDir = new File(new File(getExternalFilesDir(null), "ssx3"), "game");
        refresh();
        /* A path to import straight away, for testing the extractor without
         * the picker:
         *   adb shell am start -n com.douglashalse.ssx3/.LauncherActivity          *       -e import_path /sdcard/Download/game.iso
         * Same code path as a picked descriptor; the only difference is who
         * opened the file. */
        String straight = getIntent().getStringExtra("import_path");
        if (straight != null)
            importPath(new File(straight));
        /* The same for a content:// document, which is what the picker hands
         * back -- lets the descriptor path be tested without driving the
         * picker UI:
         *   adb shell am start ... -e import_uri content://... */
        String uriString = getIntent().getStringExtra("import_uri");
        if (uriString != null)
            importUri(Uri.parse(uriString));
    }

    private void importUri(Uri uri) {
        long fd;
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) {
                status.setText("The picker gave no file for that image");
                return;
            }
            fd = pfd.detachFd();
        } catch (Exception e) {
            status.setText("Cannot open that image: " + e.getMessage());
            return;
        }
        onDescriptor(fd);
    }

    private void importPath(File file) {
        long fd;
        try {
            ParcelFileDescriptor pfd = ParcelFileDescriptor.open(
                    file, ParcelFileDescriptor.MODE_READ_ONLY);
            fd = pfd.detachFd();
        } catch (Exception e) {
            status.setText("Cannot open " + file + ": " + e.getMessage());
            return;
        }
        onDescriptor(fd);
    }

    /** Probe a descriptor, then import it: the one path every entry shares. */
    private void onDescriptor(long fd) {
        String probe = nativeProbe(fd);
        detail.setText(probe);
        if (!nativeProbeIsPal()) {
            nativeClose(fd);
            status.setText("Not the disc this port needs");
            return;
        }
        if (getIntent().getBooleanExtra("probe_only", false)) {
            status.setText("Probe only: this disc would be imported");
            nativeClose(fd);
            return;
        }
        startExtract(fd);
    }

    private LinearLayout buildUi() {
        int pad = (int) (24 * getResources().getDisplayMetrics().density);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        root.setBackgroundColor(0xFF101418);
        root.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        TextView title = new TextView(this);
        title.setText("SSX 3");
        title.setTextSize(34);
        title.setTextColor(0xFFEAF2FF);
        root.addView(title);

        TextView sub = new TextView(this);
        sub.setText("Xbox recompiled to native ARM, running on this phone");
        sub.setTextSize(14);
        sub.setTextColor(0xFF8FA3B8);
        root.addView(sub);

        /* What this APK is: a release says which one, anything else says the
         * commit it came from -- the only useful identifier for a build that
         * is not a release. Stamped in by android/build_apk.sh. */
        TextView build = new TextView(this);
        build.setText(BuildInfo.RELEASE
                ? "Version " + BuildInfo.VERSION
                : "Build " + BuildInfo.COMMIT + " \u00b7 " + BuildInfo.DATE);
        build.setTextSize(12);
        build.setTextColor(0xFF5E7183);
        build.setPadding(0, pad / 6, 0, 0);
        root.addView(build);

        status = new TextView(this);
        status.setTextSize(18);
        status.setTextColor(0xFFEAF2FF);
        status.setPadding(0, pad, 0, 0);
        root.addView(status);

        detail = new TextView(this);
        detail.setTextSize(13);
        detail.setTextColor(0xFF8FA3B8);
        detail.setPadding(0, pad / 3, 0, 0);
        detail.setMovementMethod(new ScrollingMovementMethod());
        root.addView(detail);

        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(ProgressBar.GONE);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.topMargin = pad / 2;
        root.addView(bar, blp);

        pick = new Button(this);
        pick.setText("Select game image (.iso)");
        pick.setOnClickListener(v -> chooseImage());
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        plp.topMargin = pad;
        root.addView(pick, plp);

        play = new Button(this);
        play.setText("Play");
        play.setEnabled(false);
        play.setOnClickListener(v -> startActivity(
                new Intent(this, org.libsdl.app.SDLActivity.class)));
        root.addView(play, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView foot = new TextView(this);
        foot.setText("SSX 3 is a trademark of its owners. No game data ships with this app: "
                + "you need your own PAL disc image.");
        foot.setTextSize(11);
        foot.setTextColor(0xFF5E7183);
        foot.setGravity(Gravity.START);
        LinearLayout.LayoutParams flp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        flp.topMargin = pad;
        root.addView(foot, flp);

        return root;
    }

    /** What state the game data is in. */
    private void refresh() {
        File xbe = new File(gameDir, "default.xbe");
        File data = new File(gameDir, "data");
        if (xbe.isFile() && data.isDirectory()) {
            status.setText("Game data ready");
            detail.setText("Installed in " + gameDir + "\n" + sizeOf(gameDir) + " on device.");
            play.setEnabled(true);
            pick.setText("Replace game image (.iso)");
        } else {
            status.setText("No game data yet");
            detail.setText("Select your own SSX 3 PAL disc image (.iso). The game's files are "
                    + "extracted to " + gameDir + ", which needs about 2.5 GB and belongs to this "
                    + "app alone.");
            play.setEnabled(false);
            pick.setText("Select game image (.iso)");
        }
    }

    private static String sizeOf(File f) {
        long total = 0;
        File[] kids = f.listFiles();
        if (kids == null)
            return "0 MB";
        for (File k : kids) {
            if (k.isDirectory())
                total += k.getTotalSpace() == 0 ? 0 : dirSize(k);
            else
                total += k.length();
        }
        return (total / (1024 * 1024)) + " MB";
    }

    private static long dirSize(File dir) {
        long total = 0;
        File[] kids = dir.listFiles();
        if (kids == null)
            return 0;
        for (File k : kids)
            total += k.isDirectory() ? dirSize(k) : k.length();
        return total;
    }

    private void chooseImage() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        /* Everything, deliberately: disc images are reported as
         * application/octet-stream by some providers and
         * application/x-iso9660-image by others, and a MIME filter that misses
         * one hides the exact file the player is looking for. What the file is
         * gets decided after it is picked, by reading it. */
        i.setType("*/*");
        /* Open where disc images usually are. A provider that does not know
         * this URI ignores it and shows its own start page. */
        String start = getIntent().getStringExtra("initial_uri");
        if (start == null)
            start = "content://com.android.providers.downloads.documents/root/downloads";
        try {
            i.putExtra("android.provider.extra.INITIAL_URI", Uri.parse(start));
        } catch (Exception ignored) {
        }
        startActivityForResult(i, PICK_IMAGE);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        Uri uri = (request == PICK_IMAGE && result == RESULT_OK && data != null)
                ? data.getData() : null;
        if (uri == null)
            return;

        long fd;
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) {
                status.setText("Cannot open that file");
                return;
            }
            fd = pfd.detachFd();
        } catch (Exception e) {
            status.setText("Cannot open that file: " + e.getMessage());
            return;
        }

        String probe = nativeProbe(fd);
        boolean pal = nativeProbeIsPal();
        detail.setText(probe);
        if (!pal) {
            nativeClose(fd);
            status.setText("Not the disc this port needs");
            return;
        }
        if (getIntent().getBooleanExtra("probe_only", false)) {
            status.setText("Probe only: this disc would be imported");
            nativeClose(fd);
            return;
        }
        if (new File(gameDir, "default.xbe").isFile()) {
            final long ffd = fd;
            new AlertDialog.Builder(this)
                    .setTitle("Replace the game data?")
                    .setMessage("Your saves are kept. The installed game files are replaced.")
                    .setPositiveButton("Replace", (d, w) -> startExtract(ffd))
                    .setNegativeButton("Cancel", (d, w) -> nativeClose(ffd))
                    .show();
            return;
        }
        startExtract(fd);
    }

    private void startExtract(final long fd) {
        status.setText("Extracting...");
        pick.setEnabled(false);
        play.setEnabled(false);
        bar.setVisibility(ProgressBar.VISIBLE);
        bar.setProgress(0);

        /* 8 MB of stack, not the ART default: what the reader recurses to
         * depends on the image, and an 8 MB stack makes a pathological table
         * a slow failure instead of a crash. */
        worker = new Thread(null, () -> {
            final int rc = nativeExtract(fd, gameDir.getAbsolutePath());
            runOnUiThread(() -> {
                bar.setVisibility(ProgressBar.GONE);
                pick.setEnabled(true);
                if (rc == 0) {
                    refresh();
                } else {
                    status.setText("Extraction failed");
                    detail.setText(nativeError());
                    refresh();
                }
            });
        }, "xiso-extract", 8L * 1024 * 1024);
        worker.start();
        pollProgress();
    }

    private void pollProgress() {
        long total = nativeTotal(), done = nativeDone();
        if (total > 0) {
            bar.setProgress((int) Math.min(1000, done * 1000 / total));
            detail.setText((done / (1024 * 1024)) + " / " + (total / (1024 * 1024)) + " MB");
        }
        if (worker != null && worker.isAlive())
            handler.postDelayed(this::pollProgress, 250);
    }
}
