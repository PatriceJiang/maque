/* 8 辆车：物理与外观的唯一真相。
 * 单位：米 / 秒 / 弧度。颜色按 sRGB 写（运行时转线性）。
 * vMax 是"直道极速"，实际会被风阻与弯道抓地上限压住。
 */
export const CARS = [
  {
    id: 'viper', name: 'VIPER GT', cls: 'S',
    color: [0.80, 0.10, 0.09], accent: [0.06, 0.06, 0.07],
    rim: "#b8bcc0",
    vMax: 66.0, accel: 9.4, brake: 26.0, grip: 16.0, steerRate: 3.1, mass: 1180,
    len: 4.35, wid: 1.98, hgt: 1.06, wheelR: 0.35, wheelW: 0.30,
    spoiler: 1.0, nose: 1.0, cabin: [0.66, 0.34, 0.94], cabinZ: -0.12,
  },
  {
    id: 'comet', name: 'COMET RS', cls: 'A',
    color: [0.13, 0.34, 0.86], accent: [0.90, 0.90, 0.92],
    rim: "#c9d1d4",
    vMax: 62.0, accel: 8.7, brake: 25.0, grip: 15.2, steerRate: 3.2, mass: 1120,
    len: 4.20, wid: 1.90, hgt: 1.10, wheelR: 0.34, wheelW: 0.28,
    spoiler: 0.55, nose: 0.6, cabin: [0.60, 0.36, 0.92], cabinZ: -0.06,
  },
  {
    id: 'bandit', name: 'BANDIT V8', cls: 'A',
    color: [0.86, 0.52, 0.08], accent: [0.10, 0.10, 0.11],
    rim: "#8f9296",
    vMax: 64.0, accel: 9.0, brake: 25.0, grip: 14.2, steerRate: 2.95, mass: 1260,
    len: 4.52, wid: 1.96, hgt: 1.16, wheelR: 0.36, wheelW: 0.32,
    spoiler: 0.0, nose: 0.9, cabin: [0.55, 0.38, 0.90], cabinZ: -0.20,
  },
  {
    id: 'dune', name: 'DUNE BUGGY', cls: 'B',
    color: [0.88, 0.82, 0.22], accent: [0.20, 0.20, 0.18],
    rim: "#3a3f44",
    vMax: 54.0, accel: 8.2, brake: 22.0, grip: 19.0, steerRate: 3.5, mass: 980,
    len: 3.95, wid: 1.92, hgt: 1.34, wheelR: 0.44, wheelW: 0.36,
    spoiler: 0.0, nose: 0.0, cabin: [0.50, 0.40, 0.80], cabinZ: -0.05,
  },
  {
    id: 'kite', name: 'KITE TURBO', cls: 'S',
    color: [0.10, 0.66, 0.56], accent: [0.95, 0.95, 0.95],
    rim: "#d5d8cf",
    vMax: 67.0, accel: 9.8, brake: 25.5, grip: 15.6, steerRate: 3.3, mass: 1050,
    len: 4.16, wid: 1.92, hgt: 1.02, wheelR: 0.33, wheelW: 0.28,
    spoiler: 0.8, nose: 1.0, cabin: [0.55, 0.42, 0.95], cabinZ: 0.02,
  },
  {
    id: 'hound', name: 'HOUND 4WD', cls: 'B',
    color: [0.90, 0.90, 0.92], accent: [0.14, 0.32, 0.72],
    rim: "#2c3339",
    vMax: 56.0, accel: 8.3, brake: 24.0, grip: 18.2, steerRate: 3.25, mass: 1240,
    len: 4.26, wid: 1.98, hgt: 1.26, wheelR: 0.37, wheelW: 0.30,
    spoiler: 0.7, nose: 0.4, cabin: [0.58, 0.40, 0.88], cabinZ: -0.10,
  },
  {
    id: 'nova', name: 'NOVA EV', cls: 'A',
    color: [0.56, 0.24, 0.82], accent: [0.35, 0.95, 1.0],
    rim: "#9aa7b0",
    vMax: 60.0, accel: 10.4, brake: 27.0, grip: 15.0, steerRate: 3.2, mass: 1320,
    len: 4.10, wid: 1.94, hgt: 1.06, wheelR: 0.34, wheelW: 0.30,
    spoiler: 0.35, nose: 0.0, cabin: [0.62, 0.46, 0.98], cabinZ: -0.02,
  },
  {
    id: 'brute', name: 'BRUTE PU', cls: 'C',
    color: [0.36, 0.39, 0.44], accent: [0.85, 0.70, 0.20],
    rim: "#6b7076",
    vMax: 51.0, accel: 7.5, brake: 21.0, grip: 17.0, steerRate: 2.9, mass: 1380,
    len: 4.62, wid: 2.02, hgt: 1.42, wheelR: 0.40, wheelW: 0.34,
    spoiler: 0.0, nose: 0.5, cabin: [0.52, 0.44, 0.84], cabinZ: -0.14,
  },
];

export const CAR_INDEX = {};
CARS.forEach((c, i) => { CAR_INDEX[c.id] = i; });
