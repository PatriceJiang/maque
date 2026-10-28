// 表面参数：贴图采样（albedo / MR / AO / emissive / normal）+ alpha test。
// 无贴图时渲染器绑定 1×1 白（颜色/参数乘 1）与 1×1 中性法线 (0.5,0.5,1)。
// 布局 v1 的顶点色（COLOR_0）⊗ baseColor；TEXCOORD_1 由材质 uv1_mask 选择（flags2 bit4 起）。
struct Surface {
  base: vec3f,
  alpha: f32,
  n: vec3f,
  w: vec3f,
  metallic: f32,
  rough: f32,
  ao: f32,
  emissive: vec3f,
  uv: vec2f,
  alpha_mask: f32,   /* 未乘 opacity 的 alpha（MASK+BLEND 近似用） */
  transmission: f32, /* 有效透射率 = transmissionFactor × transmissionTexture.R（0=不透 1=全透） */
  aniso: f32,        /* KHR_materials_anisotropy strength（0=各向同性；可被贴图 B 通道乘） */
  aniso_dir: vec2f,  /* 切线/副切线空间的各向异性方向（默认 +X，经 anisotropyRotation 旋转） */
  trans_diff: f32,          /* KHR_materials_diffuse_transmission 强度（不透明材质的背光透射，0=无） */
  trans_diff_color: vec3f,  /* diffuseTransmissionColorFactor（透射光色调） */
  thickness: f32,           /* KHR_materials_volume：thicknessFactor × thicknessTexture.G（体积厚度） */
  atten: vec3f,             /* KHR_materials_volume：attenuationColor（Beer-Lambert 吸收色） */
  atten_dist: f32,          /* attenuationDistance（0 = 无限/无吸收） */
  multiscatter: vec3f,      /* KHR_materials_volume_scatter：多次散射色（0 = 无散射） */
};

fn flags2_bits() -> i32 {
  return i32(dr.model_params.z + 0.5);
}

/* uv1_mask 对应位：bit0=baseColor, 1=MR, 2=AO, 3=normal, 4=emissive, 5=matcap,
 * 6=gradient, 7=clearcoat normal。runtime 把 uv1_mask 折进 flags2 的 bit4..12。 */
fn uv1_sel(bit: i32) -> bool {
  return ((flags2_bits() >> u32(4 + bit)) & 1) != 0;
}

fn uv_pick(uv0: vec2f, uv1: vec2f, bit: i32) -> vec2f {
  return select(uv0, uv1, uv1_sel(bit));
}

/* KHR_texture_transform：uv' = T * R * S * uv（先缩放、再旋转、最后平移）。
 * 旋转角由 gfx 打包进 Draw.specular.w（基础贴图组）/ Draw.sheen_color.w（法线组），
 * 避免给 uniform 结构额外扩字段；默认 0 时行为与旧版完全一致。 */
fn rot2(p: vec2f, a: f32) -> vec2f {
  let c = cos(a);
  let s = sin(a);
  return vec2f(c * p.x - s * p.y, s * p.x + c * p.y);
}

fn uv_base(uv: vec2f) -> vec2f {
  return rot2(uv * dr.uv.xy, dr.specular.w) + dr.uv.zw;
}

fn uv_norm(uv: vec2f) -> vec2f {
  return rot2(uv * dr.uv_normal.xy, dr.sheen_color.w) + dr.uv_normal.zw;
}

fn sample_surface_normal(geo_n: vec3f, w: vec3f, uv: vec2f) -> vec3f {
  let nm = textureSample(normal_tex, albedo_smp, uv).xyz * 2.0 - 1.0;
  let nm_s = vec3f(nm.x * dr.mrp.z, nm.y * dr.misc.y, nm.z);
  return normalize(cotangent_frame(geo_n, w, uv) * nm_s);
}

fn sample_clearcoat_normal(geo_n: vec3f, w: vec3f, uv: vec2f) -> vec3f {
  let nm = textureSample(clearcoat_normal_tex, albedo_smp, uv).xyz * 2.0 - 1.0;
  let nm_s = vec3f(nm.x * dr.misc.z, nm.y * dr.misc.z, nm.z);
  return normalize(cotangent_frame(geo_n, w, uv) * nm_s);
}

/* glTF doubleSided：背面渲染时先把几何法线取反再做光照（规范 MUST；three.js faceDirection 同义）。
 * 切线基由取反后的几何法线派生，normal map 的 T/B 随 n 一起翻转，与 three.js 行为一致。
 *
 * transmission 近似（flags2 bit20）：gfx 会把同一网格画两遍（先背面 cull front、再正面 cull back），
 * 背面那一层**也必须**把法线翻到朝向相机——否则背面 n·v < 0，Fresnel 权重恒 1 → 整层 alpha≈0.9，
 * 玻璃变成发白的浑浊球（CompareDispersion 曾如此）。 */
fn surface_geo_normal(raw: vec3f, front_facing: bool) -> vec3f {
  var n = normalize(raw);
  let flip_back = dr.alpha.z > 0.5 || (flags2_bits() & 0x100000) != 0;
  if (flip_back && !front_facing) {
    n = -n;
  }
  return n;
}

fn surface_common(in: VOut, front_facing: bool) -> Surface {
  var s: Surface;
  s.uv = in.uv;
  s.w = in.w;
  /* KHR_texture_transform：基础贴图一组 UV、法线贴图单独一组（glTF 允许各贴图不同）。
   * 每张贴图再按 uv1_mask 决定用 TEXCOORD_0 还是 TEXCOORD_1（布局 v1 才有有效 uv1）。 */
  let uv0 = uv_base(in.uv);
  let uv1 = uv_base(in.uv1);
  let uv_base = uv_pick(uv0, uv1, 0);
  let uv_mr = uv_pick(uv0, uv1, 1);
  let uv_ao = uv_pick(uv0, uv1, 2);
  let uv_em = uv_pick(uv0, uv1, 4);
  let nuv0 = uv_norm(in.uv);
  let nuv1 = uv_norm(in.uv1);
  let uv_n = uv_pick(nuv0, nuv1, 3);
  let alb_s = textureSample(albedo_tex, albedo_smp, uv_base);
  /* glTF COLOR_0：顶点色 ⊗ baseColor（layout0 的 vs 输出 (1,1,1,1)，旧场景不变） */
  s.base = dr.base_color.rgb * alb_s.rgb * in.color.rgb;
  s.alpha_mask = clamp(dr.base_color.a * in.color.a * alb_s.a, 0.0, 1.0);
  s.alpha = clamp(s.alpha_mask * dr.model_params.w, 0.0, 1.0);
  /* 有效透射率：transmissionFactor（gfx 复用 Draw.phys2.w）× transmissionTexture.R
   * （flags2 bit23 presence；glTF 的 transmissionTexture 用 TEXCOORD_0，故用原始 in.uv）。
   * 黑 = 不透（例如 CompareDispersion 的 glTF logo 区域 → 白色不透明白 logo）。 */
  s.transmission = clamp(dr.phys2.w, 0.0, 1.0);
  if ((flags2_bits() & 0x800000) != 0) {
    s.transmission = s.transmission * clamp(textureSample(transmission_tex, albedo_smp, in.uv).r, 0.0, 1.0);
  }
  /* KHR_materials_anisotropy：strength = phys2.z（× anisotropyTexture.B）；方向默认切线 +X，
   * 经 anisotropyRotation（phys2.w —— transmission 材质该槽被 transmissionFactor 占用，此时rotation 只能取 0）
   * 旋转。贴图存在时 RG 再给方向（tangent,bitangent，[-1,1]）。 */
  s.aniso = clamp(dr.phys2.z, 0.0, 1.0);
  var aniso_rot = 0.0;
  if ((flags2_bits() & 0x100000) == 0) {
    aniso_rot = dr.phys2.w;
  }
  var aniso_dir = vec2f(cos(aniso_rot), sin(aniso_rot));
  if ((flags2_bits() & 0x40000) != 0) {          /* flags2 bit18 = anisotropyTexture presence */
    let at = textureSample(anisotropy_tex, albedo_smp, in.uv);
    let c = cos(aniso_rot);
    let sn = sin(aniso_rot);
    aniso_dir = mat2x2f(c, sn, -sn, c) * normalize(at.rg * 2.0 - vec2f(1.0));
    s.aniso = s.aniso * clamp(at.b, 0.0, 1.0);
  }
  s.aniso_dir = aniso_dir;
  /* KHR_materials_diffuse_transmission：不透明材质 + 背光透射（陶瓷/叶片/果皮；不是 alpha 透明）。
   * Draw.trans = (factor, color.rgb)；presence = flags2 bit24。 */
  s.trans_diff = 0.0;
  s.trans_diff_color = vec3f(1.0);
  if ((flags2_bits() & 0x1000000) != 0) {
    s.trans_diff = clamp(dr.trans.x, 0.0, 1.0);
    s.trans_diff_color = max(dr.trans.yzw, vec3f(0.0));
  }
  /* 透射率/透射色贴图（KHR 规范：factor *= texture.A；color *= colorTexture.rgb（sRGB→线性））。 */
  if ((flags2_bits() & 0x2000000) != 0) {
    s.trans_diff = s.trans_diff * clamp(textureSample(diffuse_transmission_tex, albedo_smp, in.uv).a, 0.0, 1.0);
  }
  if ((flags2_bits() & 0x4000000) != 0) {
    s.trans_diff_color = s.trans_diff_color *
                         max(textureSample(diffuse_transmission_color_tex, albedo_smp, in.uv).rgb, vec3f(0.0));
  }
  /* KHR_materials_volume（第 24 轮）：体积厚度 + Beer-Lambert 吸收色/距离 + volume_scatter 多次散射色。
   * thicknessTexture.G 存在（flags2 bit27）时乘 thicknessFactor。 */
  s.thickness = max(dr.clearcoat.z, 0.0);
  if ((flags2_bits() & 0x8000000) != 0) {
    s.thickness = s.thickness * clamp(textureSample(volume_thickness_tex, albedo_smp, in.uv).g, 0.0, 1.0);
  }
  s.atten = clamp(dr.vol.rgb, vec3f(0.0), vec3f(1.0));
  s.atten_dist = dr.vol.w;
  s.multiscatter = max(dr.scatter.rgb, vec3f(0.0));
  /* volume_scatter：多次散射决定体积介质的表观反照率（官方 ScatteringSkull 呈青绿色）——
   * 把漫反射基色按 multiscatterColor 色调化（近似散射后的光色）。 */
  if (max(s.multiscatter.r, max(s.multiscatter.g, s.multiscatter.b)) > 0.001) {
    s.base = s.base * clamp(s.multiscatter * 2.0, vec3f(0.02), vec3f(1.0));
    /* 体积散射介质以透射/多次散射为主：按 diffuse transmission factor 压暗表面漫反射，
     * 避免白色基底把散射光颜色冲掉（官方 ScatteringSkull 为暗青绿）。 */
    s.base = s.base * (1.0 - 0.85 * clamp(s.trans_diff, 0.0, 1.0));
  }
  let mr_s = textureSample(mr_tex, albedo_smp, uv_mr);
  s.metallic = clamp(dr.mrp.x * mr_s.b, 0.0, 1.0);
  s.rough = clamp(dr.mrp.y * mr_s.g, 0.02, 1.0);
  /* glTF occlusionTexture.strength 语义 = mix(1, tex.r, strength)，不是 tex.r × strength */
  let ao_s = textureSample(ao_tex, albedo_smp, uv_ao).r;
  s.ao = clamp(1.0 + dr.mrp.w * (ao_s - 1.0), 0.0, 1.0);
  s.emissive = dr.emissive.rgb * dr.emissive.w * textureSample(emissive_tex, albedo_smp, uv_em).rgb;

  var n_geo = surface_geo_normal(in.n, front_facing);
  if ((flags2_bits() & 1) != 0) {
    n_geo = flat_normal(in.w);           /* flat shading 本身已按朝向相机修正符号 */
  }
  s.n = sample_surface_normal(n_geo, in.w, uv_n);
  /* transmission 近似：玻璃中心仍透，但掠射角按 Fresnel 提高不透明度（观感更像玻璃边缘反光）。
   * s.n 已由 surface_geo_normal 统一翻到朝向相机（含两遍绘制的背面层），故 n·v ∈ [0,1]。 */
  if ((flags2_bits() & 0x100000) != 0) {
    let v = normalize(cam.eye.xyz - s.w);
    let n_dot_v = clamp(dot(s.n, v), 0.0, 1.0);
    let fres = pow(1.0 - n_dot_v, 3.0);
    let tf = clamp(dr.phys2.w, 0.0, 1.0);
    s.alpha = clamp(s.alpha + (1.0 - s.alpha) * fres * tf * 0.75, 0.0, 1.0);
  }
  return s;
}

/* clearcoat normal 的 UV（bit7；法线贴图一组 transform） */
fn clearcoat_normal_uv(in: VOut) -> vec2f {
  let nuv0 = uv_norm(in.uv);
  let nuv1 = uv_norm(in.uv1);
  return uv_pick(nuv0, nuv1, 7);
}

/* ---- KHR 扩展材质贴图槽（binding 10..18；presence = flags2 bit12..18 / bit21..22）----
 * glTF 语义（通道）：
 *   clearcoatTexture.R × clearcoatFactor
 *   clearcoatRoughnessTexture.G × clearcoatRoughnessFactor
 *   sheenColorTexture.RGB × sheenColorFactor（sRGB）
 *   sheenRoughnessTexture.A × sheenRoughnessFactor
 *   iridescenceTexture.R × iridescenceFactor
 *   iridescenceThicknessTexture.G → mix(thicknessMin, thicknessMax, G)
 *   anisotropyTexture.B × anisotropyStrength（方向 RG 暂不参与近似 BRDF）
 *   specularTexture.A × specularFactor、specularColorTexture.RGB × specularColorFactor（sRGB）
 * 缺贴图时绑 white：乘 1 不改变因子；厚度用 presence 决定是否 mix（避免 white G=1 恒取 max）。 */
struct ExtParams {
  clearcoat: vec2f,        // x=factor y=roughness
  sheen_color: vec3f,
  sheen_roughness: f32,
  iridescence: f32,
  iridescence_thickness: f32,   // <0 = 无厚度贴图（调用方走视角近似）
  anisotropy: f32,
  specular_factor: f32,         // specularIntensity × specularTexture.A
  specular_color: vec3f,        // specularColorFactor × specularColorTexture.RGB
};

fn ext_tex_present(bit: u32) -> bool {
  return ((u32(flags2_bits()) >> (12u + bit)) & 1u) != 0u;
}

/* KHR_materials_specular 两槽 presence（bit21/22） */
fn spec_tex_present(bit: u32) -> bool {
  return ((u32(flags2_bits()) >> (21u + bit)) & 1u) != 0u;
}

fn surface_ext_params(uv: vec2f) -> ExtParams {
  var e: ExtParams;
  e.clearcoat = vec2f(dr.clearcoat.x, dr.clearcoat.y);
  if (ext_tex_present(0u)) { e.clearcoat.x *= textureSample(clearcoat_tex, albedo_smp, uv).r; }
  if (ext_tex_present(1u)) { e.clearcoat.y *= textureSample(clearcoat_rough_tex, albedo_smp, uv).g; }
  e.sheen_color = dr.sheen_color.rgb;
  e.sheen_roughness = dr.phys1.y;
  if (ext_tex_present(2u)) { e.sheen_color *= textureSample(sheen_color_tex, albedo_smp, uv).rgb; }
  if (ext_tex_present(3u)) { e.sheen_roughness *= textureSample(sheen_rough_tex, albedo_smp, uv).a; }
  e.iridescence = dr.phys1.z;
  e.iridescence_thickness = -1.0;
  if (ext_tex_present(4u)) { e.iridescence *= textureSample(iridescence_tex, albedo_smp, uv).r; }
  if (ext_tex_present(5u)) {
    let g = textureSample(iridescence_thickness_tex, albedo_smp, uv).g;
    e.iridescence_thickness = mix(dr.phys2.x, dr.phys2.y, clamp(g, 0.0, 1.0));
  }
  e.anisotropy = dr.phys2.z;
  if (ext_tex_present(6u)) { e.anisotropy *= textureSample(anisotropy_tex, albedo_smp, uv).b; }
  /* KHR_materials_specular：factor/color 因子来自 uniform，贴图按通道调制（white 时乘 1）。 */
  e.specular_factor = dr.phys0.x;
  e.specular_color = max(dr.specular.rgb, vec3f(0.0));   /* factor 可 >1（CompareSpecular 用 10×） */
  if (spec_tex_present(0u)) { e.specular_factor *= textureSample(specular_tex, albedo_smp, uv).a; }
  if (spec_tex_present(1u)) { e.specular_color *= textureSample(specular_color_tex, albedo_smp, uv).rgb; }
  e.clearcoat = clamp(e.clearcoat, vec2f(0.0), vec2f(1.0));
  e.sheen_roughness = clamp(e.sheen_roughness, 0.0, 1.0);
  e.iridescence = clamp(e.iridescence, 0.0, 1.0);
  e.anisotropy = clamp(e.anisotropy, 0.0, 1.0);
  e.specular_factor = clamp(e.specular_factor, 0.0, 1.0);
  e.specular_color = max(e.specular_color, vec3f(0.0));   /* glTF 允许 >1（CompareSpecular 用 10× 做对照） */
  return e;
}

/* MASK：alpha 低于 cutoff 直接丢弃（不透明/混合管线里统一做 alpha test）。 */
fn surface_alpha_test(s: Surface) {
  if (i32(dr.alpha.y + 0.5) == 1 && s.alpha < dr.alpha.x) {
    discard;
  }
  /* transmission/diffuse 近似：原 MASK 材质被改成 BLEND，但仍按未乘 opacity 的 alpha 做镂空 */
  if ((flags2_bits() & 0x80000) != 0 && s.alpha_mask < dr.alpha.x) {
    discard;
  }
}

/* ---- transmission 屏幕空间折射（plans/19 P1 近似；fs_standard / fs_physical 使用）----
 * gfx 在 opaque pass 结束后把颜色拷贝到 scene_color（group0 binding6），这里按屏幕空间法线偏移采样：
 *   · 偏移方向 = 法线的屏幕投影（正对相机的中心几乎不偏移、掠射边缘偏移最大 → 球/管件的透镜感）；
 *   · 偏移量 ∝ KHR_materials_volume.thicknessFactor（gfx 打包在 dr.clearcoat.z）；
 *   · RGB 三通道不同偏移量 = KHR_materials_dispersion（gfx 打包在 dr.alpha.w）的色散。
 * 返回值是**显示空间**颜色：scene_color 已是 tone map + sRGB 编码后的像素，透射部分直接用它，
 * 表面高光/环境反射先 tone_output 再叠加（避免二次 tonemap 造成偏色）。 */
fn transmission_refraction(in: VOut, s: Surface, color: vec3f, f0: vec3f) -> vec3f {
  /* 逐像素折射：入射视线 v 随像素连续变化（法线 facet 内为常量），射线在玻璃内走 thickness
   * 后的出射点投影到屏幕 → 采样背景。这样同一 facet 内也是连续的透镜形变（真实玻璃球观感），
   * 比"法线屏幕投影偏移"（facet 内常量 → 整块平色）更接近官方参考图。 */
  let v = normalize(cam.eye.xyz - s.w);
  let eta = 1.0 / clamp(dr.phys0.y, 1.0, 2.5);               // 1/IOR
  let thick = clamp(s.thickness, 0.02, 2.0);                 // KHR_materials_volume.thicknessFactor × thicknessTexture.G
  let dsp = clamp(dr.alpha.w, 0.0, 20.0) * 0.02;             // KHR_materials_dispersion
  let r_r = refract(-v, s.n, eta * (1.0 + dsp));
  let r_g = refract(-v, s.n, eta);
  let r_b = refract(-v, s.n, eta * (1.0 - dsp));
  let uv_r = ndc_to_uv(cam.vp * vec4f(s.w + r_r * thick, 1.0));
  let uv_g = ndc_to_uv(cam.vp * vec4f(s.w + r_g * thick, 1.0));
  let uv_b = ndc_to_uv(cam.vp * vec4f(s.w + r_b * thick, 1.0));
  let samp = vec3f(
      textureSampleLevel(scene_color, env_smp, uv_r, 0.0).r,
      textureSampleLevel(scene_color, env_smp, uv_g, 0.0).g,
      textureSampleLevel(scene_color, env_smp, uv_b, 0.0).b);
  /* KHR_materials_volume：Beer-Lambert 吸收（路径 = thickness/attenuationDistance）+ 多次散射入射光
   * （volume_scatter 的 multiscatterColorFactor 近似为各向同性 in-scatter）。 */
  let att = volume_transmittance(s.thickness, s.atten, s.atten_dist);
  let in_scatter = s.multiscatter * env_irradiance(s.n) * 0.6;
  let samp_v = samp * att + in_scatter;
  /* 玻璃色调（显示空间近似；baseColor 已是线性） */
  let tint = pow(clamp(s.base, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2));
  let n_dot_v = clamp(dot(s.n, v), 0.0, 1.0);
  let F = fresnel_schlick(n_dot_v, f0);
  let refl = env_radiance(reflect(-v, s.n), s.rough) * dr.phys0.w;
  let refl_d = tone_output(refl, 1.0).rgb * F;
  let base_d = tone_output(color, 1.0).rgb;
  return clamp(mix(base_d, samp_v * tint, s.transmission) + refl_d, vec3f(0.0), vec3f(1.0));
}
