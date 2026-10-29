package org.maque.player;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.util.List;
import java.util.Locale;

public final class MainActivity extends Activity {
    static final String EXTRA_SCENE_INDEX = "org.maque.player.SCENE_INDEX";
    static final String EXTRA_AUTO_LAUNCH = "org.maque.player.AUTO_LAUNCH";
    static final String PREFS = "maque-scenes";
    static final String PREF_INDEX = "selected-index";

    private List<String> scenes;
    private ListView sceneList;
    private TextView status;
    private Button openButton;
    private int selectedIndex;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        scenes = SceneCatalog.load(this);
        selectedIndex = SceneCatalog.wrap(
                getSharedPreferences(PREFS, MODE_PRIVATE).getInt(PREF_INDEX, 0), scenes.size());
        setContentView(createContent());
        refreshSelection();
        if (!scenes.isEmpty() && getIntent().getBooleanExtra(EXTRA_AUTO_LAUNCH, false)) {
            select(getIntent().getIntExtra(EXTRA_SCENE_INDEX, selectedIndex));
            sceneList.post(this::launchSelectedScene);
        }
    }

    private View createContent() {
        int padding = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(padding, padding, padding, padding);
        root.setBackgroundColor(Color.rgb(246, 247, 249));

        TextView title = new TextView(this);
        title.setText(getString(R.string.app_name));
        title.setTextColor(Color.rgb(24, 28, 33));
        title.setTextSize(24);
        title.setGravity(Gravity.CENTER_VERTICAL);
        root.addView(title, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(48)));

        status = new TextView(this);
        status.setTextColor(Color.rgb(86, 94, 104));
        status.setTextSize(14);
        status.setGravity(Gravity.CENTER_VERTICAL);
        root.addView(status, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(36)));

        sceneList = new ListView(this);
        sceneList.setChoiceMode(ListView.CHOICE_MODE_SINGLE);
        sceneList.setDividerHeight(1);
        sceneList.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_list_item_single_choice, scenes));
        sceneList.setOnItemClickListener((parent, view, position, id) -> {
            select(position);
            launchSelectedScene();
        });
        root.addView(sceneList, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

        LinearLayout commands = new LinearLayout(this);
        commands.setOrientation(LinearLayout.HORIZONTAL);
        commands.setGravity(Gravity.CENTER_VERTICAL);

        Button previous = commandButton(R.string.previous_scene);
        previous.setOnClickListener(view -> select(selectedIndex - 1));
        commands.addView(previous, weightedButtonParams());

        openButton = commandButton(R.string.open_scene);
        openButton.setOnClickListener(view -> launchSelectedScene());
        commands.addView(openButton, weightedButtonParams());

        Button next = commandButton(R.string.next_scene);
        next.setOnClickListener(view -> select(selectedIndex + 1));
        commands.addView(next, weightedButtonParams());

        root.addView(commands, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(64)));
        return root;
    }

    private Button commandButton(int label) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(15);
        return button;
    }

    private LinearLayout.LayoutParams weightedButtonParams() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, dp(48), 1.0f);
        params.setMargins(dp(4), dp(8), dp(4), dp(8));
        return params;
    }

    private void select(int index) {
        if (scenes.isEmpty()) return;
        selectedIndex = SceneCatalog.wrap(index, scenes.size());
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_INDEX, selectedIndex).apply();
        refreshSelection();
    }

    private void refreshSelection() {
        boolean available = !scenes.isEmpty();
        openButton.setEnabled(available);
        if (!available) {
            status.setText(R.string.scene_count_empty);
            return;
        }
        sceneList.setItemChecked(selectedIndex, true);
        sceneList.setSelection(selectedIndex);
        status.setText(String.format(Locale.ROOT, "%d scenes  |  %s", scenes.size(), scenes.get(selectedIndex)));
    }

    private void launchSelectedScene() {
        if (scenes.isEmpty()) return;
        Intent intent = new Intent(this, MaqueActivity.class);
        intent.putExtra(EXTRA_SCENE_INDEX, selectedIndex);
        startActivity(intent);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
