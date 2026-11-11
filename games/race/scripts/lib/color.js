/* sRGB → 线性。引擎材质吃线性颜色；调色板按 sRGB 写更直观。 */
export function srgb(c) {
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}
export function srgb3(a) { return [srgb(a[0]), srgb(a[1]), srgb(a[2])]; }
