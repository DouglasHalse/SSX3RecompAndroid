package com.douglashalse.ssx3;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * Settings, one row per knob: tap a row, pick from a list, and the choice is
 * written to {@code <root>/recomp.cfg} before the game starts.
 *
 * Rows rather than a switchboard because every option here is an enumeration
 * (the game reads strings like {@code RECOMP_MSAA=4}); a dialog keeps the
 * current value visible and is what Android does for the same job elsewhere.
 */
public class SettingsActivity extends Activity {

    private static final int BG = 0xFF101418;
    private static final int FG = 0xFFEAF2FF;
    private static final int MUTED = 0xFF8FA3B8;
    private static final int ACCENT = 0xFF6FB3FF;

    private LinearLayout list;
    private int pad;

    private File root;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        root = new File(getExternalFilesDir(null), "ssx3");
        setContentView(buildUi());
    }

    private ScrollView buildUi() {
        pad = (int) (20 * getResources().getDisplayMetrics().density);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(pad, pad, pad, pad);
        scroll.addView(list, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        rebuild();
        return scroll;
    }

    private void rebuild() {
        list.removeAllViews();

        TextView title = new TextView(this);
        title.setText("Settings");
        title.setTextSize(24);
        title.setTypeface(title.getTypeface(), Typeface.BOLD);
        title.setTextColor(FG);
        list.addView(title);

        TextView note = new TextView(this);
        note.setText("Applies the next time you press Play. Nothing here changes the "
                + "game's own options.");
        note.setTextSize(12);
        note.setTextColor(MUTED);
        note.setPadding(0, pad / 4, 0, pad / 2);
        list.addView(note);

        for (String group : Settings.groups()) {
            TextView header = new TextView(this);
            header.setText(group.toUpperCase());
            header.setTextSize(11);
            header.setTypeface(header.getTypeface(), Typeface.BOLD);
            header.setTextColor(ACCENT);
            header.setPadding(0, pad, 0, pad / 3);
            list.addView(header);

            for (final Settings.Opt o : Settings.in(group))
                list.addView(row(o));
        }

        Button reset = new Button(this);
        reset.setText("Reset to defaults");
        reset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Settings.reset(SettingsActivity.this);
                write();
                rebuild();
            }
        });
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rlp.topMargin = pad;
        list.addView(reset, rlp);

        TextView where = new TextView(this);
        where.setText("Written to " + new File(root, "recomp.cfg"));
        where.setTextSize(11);
        where.setTextColor(0xFF5E7183);
        where.setPadding(0, pad / 2, 0, pad);
        list.addView(where);
    }

    private LinearLayout row(final Settings.Opt o) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(pad / 3, pad / 3, pad / 3, pad / 3);
        row.setBackgroundColor(0xFF171C22);
        row.setClickable(true);
        row.setFocusable(true);

        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        text.setLayoutParams(new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView name = new TextView(this);
        name.setText(o.title);
        name.setTextSize(16);
        name.setTextColor(FG);
        text.addView(name);

        if (o.summary != null) {
            TextView sub = new TextView(this);
            sub.setText(o.summary);
            sub.setTextSize(11);
            sub.setTextColor(MUTED);
            text.addView(sub);
        }
        row.addView(text);

        final TextView value = new TextView(this);
        value.setText(o.labels[Settings.index(this, o)]);
        value.setTextSize(14);
        value.setTextColor(ACCENT);
        value.setPadding(pad / 2, 0, 0, 0);
        row.addView(value);

        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                /* The dark device dialog, so the choice list does not flash
                 * white over the dark screen. */
                new AlertDialog.Builder(SettingsActivity.this,
                        android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle(o.title)
                        .setSingleChoiceItems(o.labels, Settings.index(SettingsActivity.this, o),
                                new DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(DialogInterface dialog, int which) {
                                        Settings.set(SettingsActivity.this, o, which);
                                        value.setText(o.labels[which]);
                                        write();
                                        dialog.dismiss();
                                    }
                                })
                        .setNegativeButton("Cancel", null)
                        .show();
            }
        });

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = Math.max(2, pad / 12);
        row.setLayoutParams(lp);
        return row;
    }

    private void write() {
        try {
            Settings.write(this, root);
        } catch (Exception e) {
            TextView where = new TextView(this);
            where.setText("Cannot write recomp.cfg: " + e);
            where.setTextColor(Color.RED);
            where.setTextSize(12);
            list.addView(where);
        }
    }
}
