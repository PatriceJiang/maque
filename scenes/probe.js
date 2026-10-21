/* 最小 ImGui 面板探针：逐级加复杂度，用来定位"面板内容糊/白条"的渲染缺陷。
 * 用法：MAQUE_IMGUI=1 maque_player --shot out.png --shot-frame 10 scenes/imgui_probe.scn
 * 阶段（改 PROBE_STAGE 看不同层级）：
 *   0 = 只有文本          1 = +按钮/复选框/滑块（无子窗口）
 *   2 = +子窗口（不滚动） 3 = +下拉框
 */
const PROBE_STAGE = 2;

function on_start() {
    log('probe: imgui_enabled=' + imgui_enabled());
}

function update(dt) {
    if (!imgui_enabled()) return;
    imgui_set_pos(20.0, 20.0);
    imgui_set_size(300.0, 240.0);
    if (imgui_begin('probe')) {
        imgui_text('stage ' + PROBE_STAGE + ' hello 0123456789');
        if (PROBE_STAGE >= 1) {
            if (imgui_button('Button')) log('probe: clicked');
            if (imgui_checkbox('check', 1) !== 1) log('probe: unchecked');
            imgui_slider_float('slider', 0.5, 0.0, 1.0);
            imgui_separator();
        }
        if (PROBE_STAGE >= 2) {
            imgui_begin_child('child', 260.0, 120.0, 1);
            imgui_text('line 1');
            imgui_text('line 2');
            imgui_text('line 3');
            imgui_end_child();
        }
        if (PROBE_STAGE >= 3) {
            imgui_combo('combo', 0, 'aa,bb,cc,dd');
            imgui_list_box('list', 0, 'x,y,z');
        }
    }
    imgui_end();
}
