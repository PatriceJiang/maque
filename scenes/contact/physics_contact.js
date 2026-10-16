/* 碰撞查询验证脚本（docs/15 §3、docs/19）。
 *
 * 场景：ball（动态圆）从 y=210 落下，先落到 shelf（静态盒，y=120）上弹一下，
 * 再滚落后碰到 floor（静态盒，y=20）。
 * 脚本只做一件事：每帧查询本帧接触对，把"球碰到了谁"打出来。
 *
 * 这份脚本同时是"引擎只提供查询、玩法在脚本层"的示范：
 * 引擎不知道什么是"撞到墙要死"，它只给接触对；怎么解释由游戏决定。 */
var ball = entity_find('ball');

var firstHit = null;

function update(dt) {
    var n = physics_contact_count();
    for (var i = 0; i < n; ++i) {
        // 配对的接触才有两个实体；撞世界边界（平面）的接触 pair=false
        if (!physics_contact_pair(i)) {
            var nx0 = physics_contact_normal_x(i);
            var ny0 = physics_contact_normal_y(i);
            log('contact #' + i + ' vs WORLD-BOUNDS normal=(' + nx0.toFixed(1) + ',' + ny0.toFixed(1) + ')');
            continue;
        }
        var ea = physics_contact_a(i);
        var eb = physics_contact_b(i);
        var na = entity_name(ea);
        var nb = entity_name(eb);
        var nx = physics_contact_normal_x(i);
        var ny = physics_contact_normal_y(i);
        log('contact #' + i + ' ' + na + '(' + ea + ') <-> ' + nb + '(' + eb + ')' +
            ' normal=(' + nx.toFixed(2) + ',' + ny.toFixed(2) + ')');
        if (firstHit === null) {
            firstHit = (na === 'ball') ? nb : na;
            log('BALL-TOUCHED ' + firstHit + ' at frame ' + director_frame());
        }
    }
}
