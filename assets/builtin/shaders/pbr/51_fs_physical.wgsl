// Physical（MeshPhysicalMaterial）：Standard + clearcoat/sheen/iridescence/specular/ior/anisotropy。
// transmission/refraction 需要 backbuffer 折射 pass，本期不做（docs/plans/19 已知差异）。
// 扩展贴图槽（clearcoat/roughness/sheen/iridescence/thickness/anisotropy）见 surface_ext_params。
@fragment fn fs_physical(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  let v = normalize(cam.eye.xyz - s.w);
  let n_dot_v = max(dot(s.n, v), 1e-4);
  let ep = surface_ext_params(uv_base(in.uv));
  /* F0：IOR + specularIntensity/specularColor（KHR_materials_specular / ior）。
   * glTF 规范：specular 扩展只调制电介质 F0；金属 F0 = baseColor（不受影响）。 */
  let ior = clamp(dr.phys0.y, 1.0, 2.5);
  let f0_d = pow((ior - 1.0) / (ior + 1.0), 2.0);
  let f0_dielectric = vec3f(f0_d) * ep.specular_factor * ep.specular_color;
  var f0 = mix(f0_dielectric, s.base, s.metallic);
  /* iridescence：薄膜干涉色调制 F0（有厚度贴图用贴图 G，否则视角近似） */
  if (ep.iridescence > 0.001) {
    var thick = mix(dr.phys2.x, dr.phys2.y, 0.5 + 0.5 * n_dot_v);
    if (ep.iridescence_thickness >= 0.0) {
      thick = ep.iridescence_thickness;
    }
    f0 = mix(f0, iridescence_tint(clamp(thick, 0.0, 2000.0), n_dot_v), ep.iridescence * 0.6);
  }
  /* anisotropy：近似为沿主方向的粗糙度压缩（观感提示，非精确各向异性 BRDF） */
  if (ep.anisotropy > 0.001) {
    s.rough = clamp(s.rough * (1.0 - 0.4 * ep.anisotropy), 0.02, 1.0);
  }
  var n_cc = s.n;
  if ((flags2_bits() & 8) != 0) {
    n_cc = sample_clearcoat_normal(surface_geo_normal(in.n, ff), in.w, clearcoat_normal_uv(in));
  }
  var color = light_standard(s, f0, n_cc, ep.clearcoat.x, ep.clearcoat.y, dr.phys0.w);
  /* sheen：边缘绒光（three.js sheen 的观感近似；颜色可由 sheenColorTexture 调制） */
  if (dr.phys1.x > 0.001) {
    let rim = pow(1.0 - n_dot_v, 3.0);
    color = color + ep.sheen_color * dr.phys1.x * rim * (0.4 + 0.6 * (1.0 - ep.sheen_roughness))
                  * (cam.ambient.rgb * dr.phys0.w + vec3f(0.12));
  }
  color = color + s.emissive;
  return tone_output(color, s.alpha);
}
