/* 程序化图元场景的拾取自检（plans/27 M1-1）：
 * 第 2 帧后用 scene_pick 点几个图元中心/空隙，日志由 geometry_primitives_e2e 断言。
 * 注意 scene_pick 收**设计像素坐标**（与 input_x/input_y 一致，y 向下）；
 * 本场景 1280x480、相机 z=7.2/fov40 → 1 世界单位 ≈ 91.58 px：
 *   世界 (-2.8,0) → (384,240) 球；世界 (0,0) → (640,240) 圆柱；
 *   世界 (1.4,0.3) → (768,213) 圆锥；世界 (-2.8,2.2) → (384,39) 空隙。
 * 只读（pick 不改模拟状态），不参与渲染。 */
let pick_done = false;

function update() {
    if (pick_done || director_frame() < 2) return;
    pick_done = true;
    const points = [
        [384, 240],   // p1_sphere
        [640, 240],   // p3_cylinder
        [768, 213],   // p4_cone（上半段）
        [384, 39],    // 球上方空隙 → 未命中
    ];
    for (let i = 0; i < points.length; ++i) {
        const id = scene_pick(points[i][0], points[i][1]);
        log("primitive pick " + points[i][0] + "," + points[i][1] +
            " id=" + id + " name=" + entity_name(id));
    }
}
