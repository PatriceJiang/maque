package org.maque.player;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class SceneCatalog {
    private static final String INDEX_ASSET = "maque/scenes.json";

    private SceneCatalog() {}

    static List<String> load(Context context) {
        try (InputStream input = context.getAssets().open(INDEX_ASSET);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                output.write(buffer, 0, read);
            }
            JSONObject root = new JSONObject(output.toString(StandardCharsets.UTF_8.name()));
            JSONArray values = root.getJSONArray("scenes");
            ArrayList<String> scenes = new ArrayList<>(values.length());
            for (int i = 0; i < values.length(); ++i) {
                String path = values.getString(i).replace('\\', '/');
                if (!path.isEmpty() && path.endsWith(".scn") && !path.startsWith("/") && !path.contains("../")) {
                    scenes.add(path);
                }
            }
            return Collections.unmodifiableList(scenes);
        } catch (Exception error) {
            throw new IllegalStateException("Cannot read packaged scene index", error);
        }
    }

    static int wrap(int index, int size) {
        if (size <= 0) return 0;
        int result = index % size;
        return result < 0 ? result + size : result;
    }
}
