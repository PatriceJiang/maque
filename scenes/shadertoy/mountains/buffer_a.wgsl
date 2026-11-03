// Buffer A — raw height map painted with the mouse (Shadertoy "Buffer A" tab).
// Reads its own previous frame (`ch0`, ping-pong in the engine) and edits a brush-sized
// region when the mouse is held: Backspace resets, Shift inverts the brush.

fn GetBrushDelta(mapPos: vec2f, cursorPos: vec2f, brushSize: f32) -> vec3f {
    // Find the distance and direction to the mouse cursor.
    let dist = distance(cursorPos, mapPos);
    let dir = normalize(cursorPos - mapPos);

    // Calculate the brush value.
    let freq = 1.0 / brushSize;
    let x = clamp(1.0 - freq * dist, 0.0, 1.0);
    return vec3f(
        x * x * (3.0 - 2.0 * x),
        dir * 6.0 * x * (1.0 - x) * freq);
}

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let fragCoord = fragCoordSH(frag_coord);
    let mapPos = fragCoord / mq_pass.resolution;

    // Set the output to the same value as in the last frame.
    var fragColor = textureSH(ch0, fragCoord / mq_pass.resolution);

    // Reset in first frame, or if pressing Backspace.
    if (mq_pass.frame < 2.0 || mq_pass.keys.x > 0.5) {
        // Initialize with a bump in the middle.
        fragColor = vec4f(DEFAULT_HEIGHT, 0.0, 0.0, 0.0);
        fragColor += vec4f(GetBrushDelta(mapPos, vec2f(0.5), 0.35) * 0.1, 0.0);
    }

    // If the mouse is pressed...
    // Mouse rays are built in **screen** pixel space (that is where iMouse lives), not in this
    // buffer's resolution — the buffer is a square sub-image of the screen map space.
    let mouse = iMouse();
    if (mouse.z > 0.0) {
        let rayPixel = GetRay(mq_pass.time, mouse, mq_pass.screen_resolution, fragCoord, 0.0);
        let rayMouse = GetRay(mq_pass.time, mouse, mq_pass.screen_resolution, mouse.xy, 0.0);
        let cursor = GetMouseWorldPos(rayPixel.ro, rayMouse.rd, DEFAULT_HEIGHT);

        let cursorPos = cursor.xz + vec2f(0.5);
        var v = GetBrushDelta(mapPos, cursorPos, BRUSH_SIZE);

        // Set the sign based on the Shift key state.
        if (mq_pass.keys.y > 0.5) {
            v = -v;
        }

        // Lerp towards the brush value (iFrameRate is the fixed 60 Hz simulation rate).
        fragColor += vec4f(v * (0.05 / 60.0), 0.0);
        fragColor.x = clamp(fragColor.x, 0.0, 1.0);
    }

    return fragColor;
}
