package com.douglashalse.ssx3;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Rect;
import android.util.DisplayMetrics;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.ArrayList;
import java.util.List;

/**
 * The knobs the game reads from {@code <root>/recomp.cfg} (loaded by
 * src/android/sdl_main.c before anything else runs), plus the choices made on
 * the settings screen.
 *
 * The Windows launcher offers a resolution list and an aspect-ratio list of
 * 16:9/21:9/32:9; a phone has one panel, so the internal render height follows
 * the screen (see {@link #write}) and the only aspect choice left is whether to
 * fill the display or keep the original 4:3 frame.
 */
public final class Settings {

    /** One row on the settings screen. */
    public static final class Opt {
        public final String group;
        public final String title;
        public final String summary;
        public final String key;
        public final String env;          // the variable the game reads, null if app-only
        public final String[] labels;
        public final String[] values;
        public final int def;             // index into labels/values

        Opt(String group, String title, String summary, String key, String env,
            String[] labels, String[] values, int def) {
            this.group = group;
            this.title = title;
            this.summary = summary;
            this.key = key;
            this.env = env;
            this.labels = labels;
            this.values = values;
            this.def = def;
        }

        public boolean isToggle() {
            return labels.length == 2;
        }
    }

    private static final String[] ON_OFF = {"Off", "On"};
    private static final String[] OFF_ON = {"Off", "On"};

    private static Opt toggle(String group, String title, String summary, String key,
                              String env, boolean onByDefault) {
        return new Opt(group, title, summary, key, env, ON_OFF, new String[] {"0", "1"},
                onByDefault ? 1 : 0);
    }

    /** Every row, in the order the screen shows them. */
    public static final List<Opt> ALL = new ArrayList<Opt>();

    static {
        /* Display */
        ALL.add(new Opt("Display", "Aspect ratio", "Fill the screen or keep the original frame",
                "aspect", "RECOMP_WIDESCREEN",
                new String[] {"Fill screen", "4:3 (original)"}, new String[] {"auto", "0"}, 0));
        ALL.add(toggle("Display", "Modern camera", "Smoother chase camera, roll and shake",
                "cam", "RECOMP_CAM", false));
        ALL.add(toggle("Display", "FPS counter", "Show the frame rate in the corner",
                "fps", "RECOMP_FPS_OVERLAY", false));

        /* Graphics */
        ALL.add(new Opt("Graphics", "Anti-aliasing", "MSAA smooths edges, costs fill rate",
                "msaa", "RECOMP_MSAA",
                new String[] {"Off", "2x", "4x", "8x"}, new String[] {"0", "2", "4", "8"}, 0));
        ALL.add(new Opt("Graphics", "Anisotropic filtering", "Keeps the snow sharp at a distance",
                "aniso", "RECOMP_ANISO",
                new String[] {"Trilinear", "4x", "8x", "16x"},
                new String[] {"0", "4", "8", "16"}, 1));
        ALL.add(new Opt("Graphics", "Draw distance", "How far the mountain is drawn",
                "drawdist", "RECOMP_DRAW_DIST",
                new String[] {"Original", "Far", "Maximum"}, new String[] {"1", "1.5", "2"}, 0));
        ALL.add(new Opt("Graphics", "Shadows", "Shadow map resolution",
                "shadows", "RECOMP_SHADOW_RES",
                new String[] {"Original", "High", "Ultra"}, new String[] {"1", "4", "8"}, 0));
        /* RECOMP_HDTEX is a directory name; "0" is the way to turn it off. */
        ALL.add(new Opt("Graphics", "HD textures", "Uses the hd-textures folder when it is there",
                "hdtex", "RECOMP_HDTEX",
                new String[] {"Off", "On"}, new String[] {"0", "hd-textures"}, 1));
        ALL.add(toggle("Graphics", "HUD edges", "Draws the screen-edge markers",
                "hud_edges", "RECOMP_HUD_EDGES", true));
        ALL.add(toggle("Graphics", "CRT filter", "Scanlines over the whole frame",
                "crt", "RECOMP_CRT", false));
        ALL.add(new Opt("Graphics", "Motion blur", "Blur at speed",
                "mblur", "RECOMP_MBLUR",
                new String[] {"Off", "On"}, new String[] {"0", "0.5"}, 0));

        /* Game */
        ALL.add(toggle("Game", "Skip intro", "Goes straight to the title screen",
                "skip_intro", "RECOMP_SKIP_INTRO", true));
        ALL.add(new Opt("Game", "Fast boot", "Shortens the frontend pauses while loading",
                "fast_boot", "RECOMP_FAST_BOOT",
                new String[] {"Off", "On"}, new String[] {"0", "4"}, 1));
        ALL.add(toggle("Game", "Debug menu", "Adds the developer menu",
                "debug_menu", "RECOMP_DEBUG_MENU", false));
        ALL.add(toggle("Game", "Money cheat", "Unlocks the lodges' money cheat",
                "money", "RECOMP_MONEY_CHEAT", false));

        /* Advanced */
        ALL.add(new Opt("Advanced", "Log level", "How much the log records",
                "log_level", "RECOMP_LOG_LEVEL",
                new String[] {"Errors only", "Warnings", "Normal", "Detailed", "Everything"},
                new String[] {"error", "warn", "info", "debug", "trace"}, 2));
    }

    private static final String PREFS = "settings";

    private Settings() {
    }

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public static String value(Context ctx, Opt o) {
        return prefs(ctx).getString(o.key, o.values[o.def]);
    }

    public static int index(Context ctx, Opt o) {
        String v = value(ctx, o);
        for (int i = 0; i < o.values.length; i++)
            if (o.values[i].equals(v))
                return i;
        return o.def;
    }

    public static void set(Context ctx, Opt o, int index) {
        prefs(ctx).edit().putString(o.key, o.values[index]).apply();
    }

    public static void reset(Context ctx) {
        prefs(ctx).edit().clear().apply();
    }

    /** Groups in display order. */
    public static List<String> groups() {
        List<String> out = new ArrayList<String>();
        for (Opt o : ALL)
            if (!out.contains(o.group))
                out.add(o.group);
        return out;
    }

    public static List<Opt> in(String group) {
        List<Opt> out = new ArrayList<Opt>();
        for (Opt o : ALL)
            if (o.group.equals(group))
                out.add(o);
        return out;
    }

    /**
     * The game renders at the display's short side and SDL scales that to the
     * panel, so "the phone's resolution" is one number, not a list. Asked of the
     * window manager rather than the activity's metrics, which exclude the
     * system bars and would shrink the render for no reason.
     */
    public static int renderHeight(Context ctx) {
        try {
            WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
            Rect b = wm.getMaximumWindowMetrics().getBounds();
            return Math.min(b.width(), b.height());
        } catch (Throwable t) {
            DisplayMetrics m = ctx.getResources().getDisplayMetrics();
            return Math.min(m.widthPixels, m.heightPixels);
        }
    }

    /**
     * Write {@code <root>/recomp.cfg}. The launcher calls this before starting
     * the game, so the file always matches the screen; nothing else writes it.
     */
    public static File write(Context ctx, File root) throws IOException {
        if (root != null && !root.isDirectory() && !root.mkdirs())
            throw new IOException("cannot create " + root);
        File target = new File(root, "recomp.cfg");
        File tmp = new File(root, "recomp.cfg.tmp");
        Writer out = new OutputStreamWriter(new FileOutputStream(tmp), "UTF-8");
        try {
            out.write("# Written by the launcher's Settings screen. The game reads\n"
                    + "# this before it starts; edits made here survive until the\n"
                    + "# next time a setting changes.\n");
            out.write("RECOMP_RES=" + renderHeight(ctx) + "\n");
            for (Opt o : ALL)
                if (o.env != null)
                    out.write(o.env + "=" + value(ctx, o) + "\n");
        } finally {
            out.close();
        }
        if (target.exists() && !target.delete())
            throw new IOException("cannot replace " + target);
        if (!tmp.renameTo(target))
            throw new IOException("cannot write " + target);
        return target;
    }
}
