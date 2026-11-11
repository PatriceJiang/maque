/* 主菜单：选车（8）+ 选赛道（4）→ 进比赛。
 * 纯 2D：背景在 menu.scn 里，其余用 lib/ui.js 动态建。
 * 选择结果写 global，race_<id>.scn 读取。
 */
import { CARS } from './data/cars.js';
import { TRACKS } from './gen/tracks.js';
import { label, setLabelText, setLabelColor, rect, moveTo } from './lib/ui.js';

const TRACK_ORDER = ['desert', 'beach', 'forest', 'city'];
const KEY = { UP: 8, DOWN: 9, LEFT: 6, RIGHT: 7, ENTER: 3, ESC: 1, TAB: 4, SPACE: 2 };

export default function () {
  const S = {
    focus: 0,            // 0 = 车，1 = 赛道
    car: 0,
    track: 0,
    carLabels: [],
    trackLabels: [],
    carHi: -1,
    trackHi: -1,
    started: false,
  };

  const DIM = [0.62, 0.66, 0.72];
  const WHITE = [0.95, 0.96, 0.98];
  const ACCENT = [1.0, 0.78, 0.28];

  function carLine(i) {
    const c = CARS[i];
    return (i + 1) + '. ' + c.name;
  }

  function trackLine(i) {
    const t = TRACKS[TRACK_ORDER[i]];
    return (i + 1) + '. ' + t.name.toUpperCase() + '   ' + Math.round(t.len) + ' M';
  }

  function on_start() {
    // 高亮条先建（同层同 z 按插入序绘制 → 先建的在下面）
    S.carHi = rect(84, 470, 560, 34, [0.14, 0.13, 0.10], 1.0);
    S.trackHi = rect(720, 470, 500, 34, [0.14, 0.13, 0.10], 1.0);

    label(84, 660, 56, 'MAQUE RACING', ACCENT);
    label(86, 592, 22, '8 CARS   ·   4 TRACKS   ·   3 LAPS   ·   ARCADE', DIM);
    label(84, 524, 20, 'CAR', DIM);
    label(720, 524, 20, 'TRACK', DIM);

    for (let i = 0; i < CARS.length; ++i) {
      S.carLabels.push(label(96, 486 - i * 40, 26, carLine(i), DIM));
    }
    for (let i = 0; i < TRACK_ORDER.length; ++i) {
      S.trackLabels.push(label(732, 486 - i * 40, 26, trackLine(i), DIM));
    }

    label(84, 132, 24, 'UP/DOWN  SELECT      LEFT/RIGHT  SWITCH COLUMN', DIM);
    label(84, 92, 26, 'ENTER  RACE           ESC  QUIT', WHITE);
    label(84, 48, 20, 'IN RACE:  W/S THROTTLE-BRAKE   A/D STEER   SPACE DRIFT   C CAMERA   R RESTART', DIM);

    refresh();
    log('menu ready cars=' + CARS.length + ' tracks=' + TRACK_ORDER.length);
  }

  function refresh() {
    for (let i = 0; i < S.carLabels.length; ++i) {
      const on = i === S.car;
      setLabelColor(S.carLabels[i], on ? (S.focus === 0 ? ACCENT : WHITE) : DIM);
    }
    for (let i = 0; i < S.trackLabels.length; ++i) {
      const on = i === S.track;
      setLabelColor(S.trackLabels[i], on ? (S.focus === 1 ? ACCENT : WHITE) : DIM);
    }
    moveTo(S.carHi, 84, 470 - S.car * 40);
    moveTo(S.trackHi, 720, 470 - S.track * 40);
    node_set_active(S.carHi, S.focus === 0 ? 1 : 0);
    node_set_active(S.trackHi, S.focus === 1 ? 1 : 0);
  }

  function scenePath(name) {
    const d = scene_dir();
    if (!d) return name;
    return (d.charAt(d.length - 1) === '/' ? d : d + '/') + name;
  }

  function startRace() {
    if (S.started) return;
    S.started = true;
    const tid = TRACK_ORDER[S.track];
    global_set_text('race.car', CARS[S.car].id);
    global_set_text('race.track', tid);
    log('menu start car=' + CARS[S.car].id + ' track=' + tid + ' dir=' + scene_dir());
    director_load_scene(scenePath('race_' + tid + '.scn'));
  }

  function on_key_down(target, k) {
    if (k === KEY.UP || k === KEY.DOWN) {
      const dir = k === KEY.UP ? -1 : 1;
      if (S.focus === 0) S.car = (S.car + dir + CARS.length) % CARS.length;
      else S.track = (S.track + dir + TRACK_ORDER.length) % TRACK_ORDER.length;
      refresh();
    } else if (k === KEY.LEFT || k === KEY.RIGHT || k === KEY.TAB) {
      S.focus = 1 - S.focus;
      refresh();
    } else if (k === KEY.ENTER || k === KEY.SPACE) {
      startRace();
    } else if (k === KEY.ESC) {
      quit();
    }
  }

  function update() {}

  return { on_start, update, on_key_down };
}
