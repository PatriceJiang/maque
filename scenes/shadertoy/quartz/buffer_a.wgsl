// Buffer A — the camera offset kept from frame to frame. ch0 = Buffer A (itself).
// The mouse and the offset at the press are kept, the live view is stored every held frame and
// the release never reads iMouse.xy; a stamp resets it all.
//
// Port: the buffer is exactly 1 x STATE_ROWS texels (the original kept this column in a corner
// of an iResolution-sized texture). `iResolution` in the drag maths is the **screen** one, so
// `screen_resolution` is used here, not this pass's own `resolution`.

fn stateFresh() -> bool {
    return mq_pass.frame < 0.5 || texelFetchSH(ch0, TX_STAMP).x != STATE_STAMP;
}

fn dragUpdate(px: vec2i) -> vec4f {
    let fresh = stateFresh();
    var o = vec4f(0.0);
    var a = vec4f(0.0);
    var s = vec4f(0.0);
    var h = vec4f(0.0);
    var dn = vec4f(0.0);
    if (!fresh) {
        o = texelFetchSH(ch0, TX_OFFSET);
        a = texelFetchSH(ch0, TX_ANCHOR);
        s = texelFetchSH(ch0, TX_START);
        h = texelFetchSH(ch0, TX_HELD);
        dn = texelFetchSH(ch0, TX_DOWN);
    }
    let down = iMouse().z > 0.0;
    let wasDown = dn.x > 0.5;
    let m = dragN(iMouse().xy); // valid only while held
    if (down && !wasDown) {
        a = vec4f(m, 0.0, 0.0);
        s = vec4f(o.xy, 0.0, 0.0);
        h = vec4f(m, 0.0, 0.0);
        dn.y = 0.0;
    } else if (down) {
        let sr = mq_pass.screen_resolution;
        dn.y = min(dn.y + length(m - h.xy) * min(sr.x, sr.y), 1e4);
        h = vec4f(m, 0.0, 0.0);
    } else if (wasDown) {
        // release: o already holds the last held view
        o = vec4f(wrapOffset(o.xy), 0.0, 0.0);
        if (CLICK_RESET) {
            if (dn.y < CLICK_PX) {
                o = vec4f(0.0);
            }
        }
    } else if (CLICK_RESET) {
        if (iMouse().w > 0.0) {
            o = vec4f(0.0); // a click shorter than a frame: never held, pressed now
        }
    }
    if (down) {
        let oxy = s.xy + (m - a.xy);
        // the elevation stops at its ends; rebasing the start keeps a drag back from winding up
        let y = clamp(oxy.y, PITCH_LO, PITCH_HI);
        s.y += y - oxy.y;
        o = vec4f(oxy.x, y, 0.0, 0.0);
    }
    dn.x = select(0.0, 1.0, down);
    if (px.y == TX_OFFSET.y) {
        return o;
    } else if (px.y == TX_DOWN.y) {
        return dn;
    } else if (px.y == TX_ANCHOR.y) {
        return a;
    } else if (px.y == TX_START.y) {
        return s;
    } else if (px.y == TX_HELD.y) {
        return h;
    }
    return vec4f(STATE_STAMP, 0.0, 0.0, 0.0);
}

// The buffer is exactly 1 x STATE_ROWS, so every texel is a state row; `fragCoordSH` makes the
// row index the same bottom-left one `texelFetchSH` uses everywhere else.
@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    return dragUpdate(vec2i(fragCoordSH(frag_coord)));
}
