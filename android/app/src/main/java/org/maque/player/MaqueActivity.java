package org.maque.player;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.util.List;

public final class MaqueActivity extends SDLActivity {
    private List<String> scenes;
    private int sceneIndex;
    private int pendingSceneIndex = -1;
    private SurfaceView renderSurface;
    private static native void nativeTouch(int id, int action, float nx, float ny);


    /**
     * SDL's default {@code setOrientationBis} promotes a resizable desktop window to
     * {@code FULL_USER}, which lets the phone stay portrait and stretches the scene.
     * This template is a landscape game host: override the SDL hint so both the scene
     * surface and the engine's design canvas stay landscape.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected SDLSurface createSDLSurface(android.content.Context context) {
        SDLSurface surface = super.createSDLSurface(context);
        surface.getHolder().setFormat(PixelFormat.RGBA_8888);
        return surface;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        scenes = SceneCatalog.load(this);
        sceneIndex = SceneCatalog.wrap(
                getIntent().getIntExtra(MainActivity.EXTRA_SCENE_INDEX, 0), scenes.size());
        super.onCreate(savedInstanceState);
        addRenderSurface();
        addSceneNavigation();
        addDebugKeys();
        hideSystemBars();
    }

    @Override
    protected String[] getArguments() {
        if (scenes == null || scenes.isEmpty()) return new String[] { "asset://ball.scn" };
        return new String[] { "asset://" + scenes.get(sceneIndex) };
    }

    public Surface getMaqueRenderSurface() {
        if (renderSurface == null) return null;
        Surface surface = renderSurface.getHolder().getSurface();
        return surface != null && surface.isValid() ? surface : null;
    }

    private void addRenderSurface() {
        renderSurface = new SurfaceView(this);
        renderSurface.getHolder().setFormat(PixelFormat.RGBA_8888);
        renderSurface.setZOrderMediaOverlay(true);
        renderSurface.setFocusable(false);
        renderSurface.setFocusableInTouchMode(false);
        renderSurface.setOnTouchListener(this::handleRenderTouch);
        renderSurface.setOnGenericMotionListener(getMotionListener());
        addContentView(renderSurface, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** 触摸只走 native 归一化队列：不依赖 SDL 在当前窗口/旋转下的本地坐标。 */
    private boolean handleRenderTouch(android.view.View view, MotionEvent event) {
        final float w = Math.max(1, view.getWidth());
        final float h = Math.max(1, view.getHeight());
        final int action = event.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                final int i = event.getActionIndex();
                nativeTouch(event.getPointerId(i), 0, event.getX(i) / w, event.getY(i) / h);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); ++i)
                    nativeTouch(event.getPointerId(i), 2, event.getX(i) / w, event.getY(i) / h);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                final int i = event.getActionIndex();
                nativeTouch(event.getPointerId(i), 1, event.getX(i) / w, event.getY(i) / h);
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < event.getPointerCount(); ++i)
                    nativeTouch(event.getPointerId(i), 3, event.getX(i) / w, event.getY(i) / h);
                break;
            default:
                return false;
        }
        return true;
    }

    /** 没有物理键盘时的调试热键：F2 = stats，F4 = 引擎选项（与桌面同一条热键路径）。 */
    private void addDebugKeys() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(4), dp(4), dp(4), dp(4));

        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.argb(165, 22, 25, 29));
        background.setCornerRadius(dp(6));
        bar.setBackground(background);

        bar.addView(debugKeyButton("F2", KeyEvent.KEYCODE_F2));
        bar.addView(debugKeyButton("F4", KeyEvent.KEYCODE_F4));

        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.END);
        params.setMargins(dp(12), dp(40), dp(12), 0);
        addContentView(bar, params);
    }

    private Button debugKeyButton(String label, int keyCode) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(12);
        button.setTextColor(Color.WHITE);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setPadding(dp(8), 0, dp(8), 0);
        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.argb(180, 48, 54, 62));
        background.setCornerRadius(dp(5));
        button.setBackground(background);
        button.setOnClickListener(view -> {
            SDLActivity.onNativeKeyDown(keyCode);
            SDLActivity.onNativeKeyUp(keyCode);
        });
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(48), dp(34));
        params.setMargins(dp(2), 0, dp(2), 0);
        button.setLayoutParams(params);
        return button;
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

        Button home = navigationButton(R.string.home);
        home.setOnClickListener(view -> goHome());
        bar.addView(home, new LinearLayout.LayoutParams(dp(88), dp(44)));

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

    /** 回到场景列表（主页）；保留当前场景索引，但不自动重开。 */
    private void goHome() {
        getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE).edit()
                .putInt(MainActivity.PREF_INDEX, sceneIndex).apply();
        Intent intent = new Intent(this, MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.putExtra(MainActivity.EXTRA_SCENE_INDEX, sceneIndex);
        startActivity(intent);
        finish();
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

    /** 全屏沉浸：状态栏会盖住 ImGui 面板顶部并拦截触摸（表现为面板点不中）。 */
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
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
