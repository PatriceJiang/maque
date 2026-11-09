/* 手柄模拟量自检（plans/36）：MAQUE_GAMEPAD 覆盖 pad0。
 * 期望值由 tests/gamepad_e2e.sh 设置：LX=0.75 LY=-0.5 LT=0.25 buttons=3（South|East）。 */
let done = false;

function near(a, b, eps) {
    if (eps === undefined) eps = 1e-3;
    return Math.abs(a - b) <= eps;
}

function check(ok, msg) {
    if (!ok) log("gamepad: FAIL " + msg);
    return ok;
}

function update() {
    if (done || director_frame() < 2) return;
    done = true;
    let ok = true;
    ok = check(near(input_gamepad_connected(), 1.0), "connected count") && ok;
    ok = check(near(input_axis(0), 0.75), "axis LX") && ok;
    ok = check(near(input_axis(1), -0.5), "axis LY") && ok;
    ok = check(near(input_axis(2), 0.0), "axis RX") && ok;
    ok = check(near(input_axis(4), 0.25), "trigger LT") && ok;
    ok = check(near(input_axis_pad(0, 3), 0.0), "axis RY via pad") && ok;
    ok = check(near(input_axis_pad(1, 0), 0.0), "no pad 1") && ok;
    ok = check(near(input_gamepad_button(1), 1.0), "button South") && ok;
    ok = check(near(input_gamepad_button(2), 1.0), "button East") && ok;
    ok = check(near(input_gamepad_button(4), 0.0), "button West released") && ok;
    ok = check(near(input_gamepad_buttons(0), 3.0), "button mask") && ok;
    log(ok ? "gamepad: PASS" : "gamepad: FAIL");
}
