// sokoban.js —— 推箱子（第二 demo）：网格关卡 + 键盘驱动 + 撤销 + 关卡推进。
// 游戏逻辑全在资源（本脚本 + .scn）；引擎提供实体/组件、渲染与输入，不认识"推箱子"。
//
// 关卡用文本地图描述（Sokoban 通用记号）：
//   # 墙   . 目标点   $ 箱子   * 箱子已在目标点   @ 玩家   + 玩家站在目标点   空格 地板
// 规则：玩家四向移动一格；若前方是箱子则尝试推动（箱子后必须是地板/目标点，且不能是墙或箱子）；
// 全部箱子到达目标点即过关（N 进入下一关）。所有状态变化由**按键事件**驱动 → 与帧率无关，可确定性回放。
(function () {
  'use strict';

  const CELL = 22;              // 单元格边长（像素）
  const GRID_X = 14, GRID_Y = 40;   // 网格左上角（世界坐标，y 向上）

  const LEVELS = [
    { name: 'Level 1 - Warmup', map: [
      '#######',
      '#     #',
      '# .$@ #',
      '#     #',
      '#######' ] },
    { name: 'Level 2 - Corner', map: [
      '########',
      '#      #',
      '# .$$@ #',
      '#  .   #',
      '#      #',
      '########' ] },
    { name: 'Level 3 - Two boxes', map: [
      '#########',
      '#       #',
      '# . . $ #',
      '#   #   #',
      '# $ @   #',
      '#       #',
      '#########' ] }
  ];

  const S = (globalThis.__maque = globalThis.__maque || { log: [] });
  const KEY = { LEFT: 6, RIGHT: 7, UP: 8, DOWN: 9, R: 82, Z: 90, N: 78,
                A: 65, D: 68, W: 87, SS: 83 };

  let level = 0;
  let gridRows = 1;     // 行数（网格 y 向下 → 世界 y 向上换算用）
  let grid = [];        // 静态：'#' 墙 / '.' 目标 / ' ' 地板
  let boxes = [];       // 动态：{x,y,onGoal}
  let player = { x: 0, y: 0 };
  let steps = 0, pushes = 0, done = false;
  let history = [];     // 撤销栈（玩家 + 箱子快照）
  let ents = { walls: [], goals: [], boxes: [], player: -1 };

  // 网格坐标（行列，行 0 在**上**）→ 世界坐标（中心，y 向上）
  function cellPos(gx, gy) {
    return { x: GRID_X + gx * CELL + CELL * 0.5,
             y: GRID_Y + (gridRows - 1 - gy) * CELL + CELL * 0.5 };
  }

  function spawnTile(cx, cy, w, h, r, g, b, z) {
    const id = entity_spawn();
    if (id < 0) return -1;
    node_add_component(id, 'scn.Transform2D');
    node_add_component(id, 'scn.Sprite');
    node_set_position(id, cx, cy);
    node_set_number(id, 'scn.Sprite', 'width', w);
    node_set_number(id, 'scn.Sprite', 'height', h);
    node_set_number(id, 'scn.Sprite', 'r', r);
    node_set_number(id, 'scn.Sprite', 'g', g);
    node_set_number(id, 'scn.Sprite', 'b', b);
    node_set_number(id, 'scn.Sprite', 'z_index', z);
    return id;
  }

  function destroyAll() {
    for (const k of ['walls', 'goals', 'boxes']) for (const id of ents[k]) node_destroy(id);
    if (ents.player >= 0) node_destroy(ents.player);
    ents = { walls: [], goals: [], boxes: [], player: -1 };
  }

  function loadLevel(i) {
    level = i % LEVELS.length;
    const map = LEVELS[level].map;
    destroyAll();
    gridRows = map.length;
    grid = []; boxes = []; history = [];
    steps = 0; pushes = 0; done = false;

    for (let gy = 0; gy < map.length; ++gy) {
      const row = [];
      for (let gx = 0; gx < map[gy].length; ++gx) {
        const ch = map[gy][gx];
        const p = cellPos(gx, gy);
        let kind = ' ';
        if (ch === '#') kind = '#';
        else if (ch === '.' || ch === '*' || ch === '+') kind = '.';
        row.push(kind);

        if (kind === '#') {
          ents.walls.push(spawnTile(p.x, p.y, CELL, CELL, 0.22, 0.24, 0.30, 1));
        } else {
          // 地板底色（暗）+ 目标点标记（绿）
          ents.goals.push(spawnTile(p.x, p.y, CELL - 1, CELL - 1, 0.10, 0.11, 0.14, 0));
          if (kind === '.') ents.goals.push(spawnTile(p.x, p.y, 6, 6, 0.35, 0.85, 0.45, 2));
        }
        if (ch === '$' || ch === '*') boxes.push({ x: gx, y: gy, id: -1 });
        if (ch === '@' || ch === '+') player = { x: gx, y: gy };
      }
      grid.push(row);
    }
    // 箱子实体
    for (const b of boxes) {
      const p = cellPos(b.x, b.y);
      b.id = spawnTile(p.x, p.y, CELL - 3, CELL - 3, 0.95, 0.62, 0.25, 3);
    }
    // 玩家（略亮的方块，中心一点）
    const pp = cellPos(player.x, player.y);
    ents.player = spawnTile(pp.x, pp.y, CELL - 5, CELL - 5, 0.45, 0.75, 1.0, 4);

    refresh();
    log('sokoban_level name=' + JSON.stringify(LEVELS[level].name)
        + ' index=' + level + ' boxes=' + boxes.length + ' goals=' + countGoals());
  }

  function countGoals() {
    let n = 0;
    for (const row of grid) for (const c of row) if (c === '.') ++n;
    return n;
  }

  function boxAt(x, y) { return boxes.find(b => b.x === x && b.y === y); }
  function isWall(x, y) { return !grid[y] || grid[y][x] === undefined || grid[y][x] === '#'; }

  function paint(b) {                 // 箱子在目标点上换色（绿）
    if (b.id < 0) return;
    const on = grid[b.y] && grid[b.y][b.x] === '.';
    b.onGoal = !!on;
    node_set_number(b.id, 'scn.Sprite', 'r', on ? 0.35 : 0.95);
    node_set_number(b.id, 'scn.Sprite', 'g', on ? 0.85 : 0.62);
    node_set_number(b.id, 'scn.Sprite', 'b', on ? 0.45 : 0.25);
  }

  function refresh() {
    for (const b of boxes) paint(b);
    done = boxes.length > 0 && boxes.every(b => b.onGoal);
    const name = LEVELS[level].name + (done ? '   [SOLVED - press N]' : '');
    node_set_string(entity_find('LevelName'), 'scn.Label', 'text', name);
    node_set_string(entity_find('Status'), 'scn.Label', 'text',
      'Steps ' + steps + '   Pushes ' + pushes + '   Boxes '
        + boxes.filter(b => b.onGoal).length + '/' + boxes.length);
    if (done) log('sokoban_done level=' + level + ' steps=' + steps + ' pushes=' + pushes);
  }

  function snapshot() {
    history.push({ player: { x: player.x, y: player.y }, boxes: boxes.map(b => ({ x: b.x, y: b.y, id: b.id })), steps, pushes, done });
    if (history.length > 200) history.shift();
  }

  function move(dx, dy) {
    if (done) return;
    const nx = player.x + dx, ny = player.y + dy;
    if (isWall(nx, ny)) { log('sokoban_block wall'); return; }
    const b = boxAt(nx, ny);
    if (b) {
      const bx = nx + dx, by = ny + dy;
      if (isWall(bx, by) || boxAt(bx, by)) { log('sokoban_block box'); return; }
      snapshot();
      b.x = bx; b.y = by;
      const p = cellPos(bx, by);
      node_set_position(b.id, p.x, p.y);
      ++pushes;
    } else {
      snapshot();
    }
    player.x = nx; player.y = ny;
    const pp = cellPos(nx, ny);
    node_set_position(ents.player, pp.x, pp.y);
    ++steps;
    refresh();
  }

  function undo() {
    const h = history.pop();
    if (!h) { log('sokoban_undo empty'); return; }
    player.x = h.player.x; player.y = h.player.y;
    const pp = cellPos(player.x, player.y);
    node_set_position(ents.player, pp.x, pp.y);
    for (let i = 0; i < boxes.length; ++i) {
      boxes[i].x = h.boxes[i].x; boxes[i].y = h.boxes[i].y;
      const bp = cellPos(boxes[i].x, boxes[i].y);
      node_set_position(boxes[i].id, bp.x, bp.y);
    }
    steps = h.steps; pushes = h.pushes; done = false;
    refresh();
    log('sokoban_undo steps=' + steps);
  }

  globalThis.on_start = function () {
    S.levelName = entity_find('LevelName');
    S.status = entity_find('Status');
    loadLevel(0);
    log('sokoban ready cells=' + CELL + ' levels=' + LEVELS.length);
  };

  globalThis.on_key_down = function (target, key) {
    if (key === KEY.LEFT || key === KEY.A) move(-1, 0);
    else if (key === KEY.RIGHT || key === KEY.D) move(1, 0);
    else if (key === KEY.UP || key === KEY.W) move(0, -1);      // 网格 y 向下 → 世界 y 向上由 cellPos 处理
    else if (key === KEY.DOWN || key === KEY.SS) move(0, 1);
    else if (key === KEY.R) { log('sokoban_restart'); loadLevel(level); }
    else if (key === KEY.Z) undo();
    else if (key === KEY.N) { log('sokoban_next'); loadLevel(level + 1); }
  };

  // 每 60 帧输出一次状态摘要（供 e2e 断言：位置/步数/胜负完全可复现）
  globalThis.update = function () {
    S.frames = (S.frames || 0) + 1;
    if (S.frames % 60 === 0) {
      log('sokoban_state ' + JSON.stringify({
        level: level, steps: steps, pushes: pushes, done: done,
        player: [player.x, player.y],
        boxes: boxes.map(b => [b.x, b.y, b.onGoal ? 1 : 0])
      }));
    }
  };
})();
