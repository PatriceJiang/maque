package org.maque.player;

import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.view.Gravity;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.util.List;

public final class MaqueActivity extends SDLActivity {
    private List<String> scenes;
    private int sceneIndex;
    private int pendingSceneIndex = -1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        scenes = SceneCatalog.load(this);
        sceneIndex = SceneCatalog.wrap(
                getIntent().getIntExtra(MainActivity.EXTRA_SCENE_INDEX, 0), scenes.size());
        super.onCreate(savedInstanceState);
        addSceneNavigation();
    }

    @Override
    protected String[] getArguments() {
        if (scenes == null || scenes.isEmpty()) return new String[] { "asset://ball.scn" };
        return new String[] { "asset://" + scenes.get(sceneIndex) };
    }

    private void addSceneNavigation() {
        if (scenes.isEmpty()) return;

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(8), dp(6), dp(8), dp(6));

        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.argb(210, 22, 25, 29));
        background.setCornerRadius(dp(6));
        bar.setBackground(background);

        Button previous = navigationButton(R.string.previous_scene);
        previous.setOnClickListener(view -> switchScene(sceneIndex - 1));
        bar.addView(previous, new LinearLayout.LayoutParams(dp(88), dp(44)));

        TextView sceneName = new TextView(this);
        sceneName.setText(scenes.get(sceneIndex));
        sceneName.setTextColor(Color.WHITE);
        sceneName.setTextSize(14);
        sceneName.setGravity(Gravity.CENTER);
        sceneName.setSingleLine(true);
        sceneName.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        LinearLayout.LayoutParams nameParams = new LinearLayout.LayoutParams(0, dp(44), 1.0f);
        nameParams.setMargins(dp(8), 0, dp(8), 0);
        bar.addView(sceneName, nameParams);

        Button next = navigationButton(R.string.next_scene);
        next.setOnClickListener(view -> switchScene(sceneIndex + 1));
        bar.addView(next, new LinearLayout.LayoutParams(dp(88), dp(44)));

        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        params.setMargins(dp(12), dp(12), dp(12), dp(48));
        addContentView(bar, params);
    }

    private Button navigationButton(int label) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(14);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        return button;
    }

    private void switchScene(int requestedIndex) {
        if (pendingSceneIndex >= 0) return;
        pendingSceneIndex = SceneCatalog.wrap(requestedIndex, scenes.size());
        getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE).edit()
                .putInt(MainActivity.PREF_INDEX, pendingSceneIndex).apply();
        finish();
    }

    @Override
    protected void onDestroy() {
        final int nextIndex = pendingSceneIndex;
        super.onDestroy(); // SDLActivity joins the native thread and clears process-wide SDL state.
        if (nextIndex < 0 || isChangingConfigurations()) return;

        Intent next = new Intent(getApplicationContext(), MaqueActivity.class);
        next.putExtra(MainActivity.EXTRA_SCENE_INDEX, nextIndex);
        next.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION);
        new Handler(Looper.getMainLooper()).post(() -> getApplicationContext().startActivity(next));
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
