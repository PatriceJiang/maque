// 光照模型：Standard/Physical（GGX）、Lambert、Phong、Toon。
// 8 盏灯循环（Directional/Point/Spot），阴影只作用于第 0 盏（方向光，M5c 约定）。
// 环境项 = analytic studio IBL（无 env 贴图时）或 PMREM env（specular GGX mip + diffuse irradiance）。

/* 标准 metallic-roughness 光照（Standard / Physical 共用）。 */
fn light_standard(s: Surface, f0: vec3f, n_cc: vec3f, cc_factor: f32, cc_roughness: f32,
                  env_intensity: f32) -> vec3f {
  let v = normalize(cam.eye.xyz - s.w);
  let n_dot_v = max(dot(s.n, v), 1e-4);
  /* KHR_materials_anisotropy：切线空间方向 → 世界空间各向异性切线基（无 TANGENT 属性时用
   * cotangent_frame 从 UV 导数构造；与 glTF 规范"切线方向默认 +X"一致）。 */
  let ar = max(s.rough * s.rough, 1e-4);
  let aniso_on = s.aniso > 0.001;
  let at = mix(ar, 1.0, s.aniso * s.aniso);
  var aniso_t = vec3f(1.0, 0.0, 0.0);
  var aniso_b = vec3f(0.0, 1.0, 0.0);
  if (aniso_on) {
    let cf = cotangent_frame(s.n, s.w, s.uv);
    aniso_t = normalize(cf * vec3f(s.aniso_dir, 0.0));
    aniso_b = normalize(cross(s.n, aniso_t));
  }
  var color = vec3f(0.0);
  for (var li = 0; li < 8; li = li + 1) {
    let L = cam.lights[li];
    if (L.pos.w < 0.5) {
      continue;
    }
    var dir = vec3f(0.0, 1.0, 0.0);
    var atten = 1.0;
    if (L.pos.w < 1.5) {
      dir = normalize(-L.dir.xyz);
    } else {
      let to_light = L.pos.xyz - s.w;
      let d2 = max(dot(to_light, to_light), 1e-6);
      let dl = sqrt(d2);
      dir = to_light / dl;
      let rng = L.params.x;
      var win = 1.0;
      if (rng > 1e-4) {                       /* range<=0 = 无限远（three.js/KHR_lights_punctual 语义） */
        win = clamp(1.0 - pow(dl / rng, 4.0), 0.0, 1.0);
      }
      atten = win * win / (d2 + 1.0);
      if (L.pos.w > 2.5) {
        let cone = -dot(normalize(L.dir.xyz), dir);
        atten = atten * smoothstep(L.params.z, L.params.y, cone);
      }
    }
    let ndl = max(dot(s.n, dir), 0.0);
    if (ndl <= 0.0) {
      continue;
    }
    var shade = 1.0;
    if (li == 0) {
      shade = shadow_factor(s.w, ndl);
    }
    let hv = normalize(v + dir);
    let ndh = max(dot(s.n, hv), 0.0);
    let fl = fresnel_schlick(max(dot(hv, v), 0.0), f0);
    var sp = vec3f(0.0);
    if (aniso_on) {
      /* Khronos 规范附录：BRDF = F · V_aniso · D_aniso（V 已含 1/(4·NdotL·NdotV)） */
      let dg = distribution_ggx_aniso(ndh, dot(aniso_t, hv), dot(aniso_b, hv), at, ar);
      let vis = visibility_ggx_aniso(ndl, n_dot_v, dot(aniso_b, v), dot(aniso_t, v),
                                     dot(aniso_t, dir), dot(aniso_b, dir), at, ar);
      sp = dg * vis * fl;
    } else {
      let dg = distribution_ggx(ndh, s.rough);
      let gg = geometry_smith(n_dot_v, ndl, s.rough);
      sp = dg * gg * fl / (4.0 * n_dot_v * ndl + 1e-4);
    }
    let kdl = (vec3f(1.0) - fl) * (1.0 - s.metallic);
    var cc = vec3f(0.0);
    if (cc_factor > 0.001) {
      let cc_rough = clamp(cc_roughness, 0.02, 1.0);
      let ncc = normalize(n_cc);
      let ndh_cc = max(dot(ncc, hv), 0.0);
      let ndv_cc = max(dot(ncc, v), 1e-4);
      let cc_fres = fresnel_schlick(max(dot(hv, v), 0.0), vec3f(0.04));
      let cc_d = distribution_ggx(ndh_cc, cc_rough);
      let cc_g = geometry_smith(ndv_cc, ndl, cc_rough);
      cc = cc_factor * cc_d * cc_g * cc_fres / (4.0 * ndv_cc * ndl + 1e-4);
    }
    color = color + (kdl * s.base / PI + sp + cc) * L.color.rgb * ndl * atten * shade;
    /* KHR_materials_diffuse_transmission 近似：不透明薄壁材质 + 背光穿透（back-lit wrap diffuse；
     * 无散射/厚度/吸收。旧实现用 alpha BLEND 会让陶瓷/叶片变"幽灵玻璃"，官方参考图是实心+透光）。 */
    if (s.trans_diff > 0.001) {
      let back = max(dot(-s.n, dir), 0.0);
      let wrap = back * back * (1.5 - 0.5 * back);
      /* KHR_materials_volume：背光透射先经 Beer-Lambert 吸收（tint 乘 attenuationColor 透过率） */
      let att = volume_transmittance(s.thickness, s.atten, s.atten_dist);
      color = color + s.trans_diff_color * att / PI * L.color.rgb * (s.trans_diff * wrap * 1.6) * atten;
    }
  }
  /* analytic studio IBL：漫反射半球 + 镜面环境（Karis env BRDF）+ 清漆环境反射（plans/19）。
   * 各向异性时按规范附录的 bent-normal 近似：把反射方向向"切线方向的弧"拉偏，并沿切线多采 2 个点
   * （低 roughness + 高 strength 时环境高光会被拉成条/弧，与 KHR_materials_anisotropy 语义一致）。 */
  let env_i = env_irradiance(s.n);
  let env_f = env_brdf_approx(f0, s.rough, n_dot_v);
  let refl = reflect(-v, s.n);
  var env_spec = env_radiance(refl, s.rough);
  if (aniso_on) {
    var bent = cross(aniso_b, v);
    bent = normalize(cross(bent, aniso_b));
    let aa = 1.0 - s.aniso * (1.0 - s.rough);
    let a4 = aa * aa * aa * aa;
    bent = normalize(mix(bent, s.n, a4));
    var r = reflect(-v, bent);
    r = normalize(mix(r, bent, s.rough * s.rough));
    let spread = clamp(at, 0.0, 1.0) * 0.6;
    env_spec = env_radiance(r, s.rough) * 0.55
             + env_radiance(normalize(r + aniso_t * spread), s.rough) * 0.225
             + env_radiance(normalize(r - aniso_t * spread), s.rough) * 0.225;
  }
  var env_c = env_i * s.base * (1.0 - s.metallic) * s.ao
            + env_spec * env_f * mix(1.0, s.ao, 0.7);
  let cc_f = clamp(cc_factor, 0.0, 1.0);
  if (cc_f > 0.001) {
    let cc_r = clamp(cc_roughness, 0.02, 1.0);
    let refl_cc = reflect(-v, normalize(n_cc));
    env_c = env_c + env_radiance(refl_cc, cc_r) * env_brdf_approx(vec3f(0.04), cc_r, n_dot_v) * cc_f;
  }
  color = color + env_c * env_intensity;
  /* diffuse transmission 的环境/背光项：背面半球的环境辐照透过薄壁（色调 = colorFactor）。
   * 对应官方测试板：factor↑ 板材由本色逐渐变成"背后环境色"；陶瓷则只得到轻微暖调透光。 */
  if (s.trans_diff > 0.001) {
    let att = volume_transmittance(s.thickness, s.atten, s.atten_dist);
    color = color + s.trans_diff_color * att * s.trans_diff * env_irradiance(-s.n) * env_intensity * 0.15;
    /* KHR_materials_volume_scatter：多次散射把环境光各向同性地散射回来（in-scatter 近似） */
    color = color + s.multiscatter * s.trans_diff * env_irradiance(s.n) * env_intensity * 0.6;
  }
  return color;
}

fn light_lambert(s: Surface, env_intensity: f32) -> vec3f {
  var color = vec3f(0.0);
  for (var li = 0; li < 8; li = li + 1) {
    let L = cam.lights[li];
    if (L.pos.w < 0.5) {
      continue;
    }
    var dir = vec3f(0.0, 1.0, 0.0);
    var atten = 1.0;
    if (L.pos.w < 1.5) {
      dir = normalize(-L.dir.xyz);
    } else {
      let to_light = L.pos.xyz - s.w;
      let d2 = max(dot(to_light, to_light), 1e-6);
      let dl = sqrt(d2);
      dir = to_light / dl;
      let rng = L.params.x;
      var win = 1.0;
      if (rng > 1e-4) {                       /* range<=0 = 无限远（three.js/KHR_lights_punctual 语义） */
        win = clamp(1.0 - pow(dl / rng, 4.0), 0.0, 1.0);
      }
      atten = win * win / (d2 + 1.0);
      if (L.pos.w > 2.5) {
        let cone = -dot(normalize(L.dir.xyz), dir);
        atten = atten * smoothstep(L.params.z, L.params.y, cone);
      }
    }
    let ndl = max(dot(s.n, dir), 0.0);
    if (ndl <= 0.0) {
      continue;
    }
    var shade = 1.0;
    if (li == 0) {
      shade = shadow_factor(s.w, ndl);
    }
    color = color + (s.base / PI) * L.color.rgb * ndl * atten * shade;
  }
  color = color + env_irradiance(s.n) * s.base * s.ao * env_intensity;
  return color;
}

fn light_phong(s: Surface, spec_color: vec3f, shininess: f32, env_intensity: f32) -> vec3f {
  let v = normalize(cam.eye.xyz - s.w);
  let sh = max(shininess, 1.0);
  var color = vec3f(0.0);
  for (var li = 0; li < 8; li = li + 1) {
    let L = cam.lights[li];
    if (L.pos.w < 0.5) {
      continue;
    }
    var dir = vec3f(0.0, 1.0, 0.0);
    var atten = 1.0;
    if (L.pos.w < 1.5) {
      dir = normalize(-L.dir.xyz);
    } else {
      let to_light = L.pos.xyz - s.w;
      let d2 = max(dot(to_light, to_light), 1e-6);
      let dl = sqrt(d2);
      dir = to_light / dl;
      let rng = L.params.x;
      var win = 1.0;
      if (rng > 1e-4) {                       /* range<=0 = 无限远（three.js/KHR_lights_punctual 语义） */
        win = clamp(1.0 - pow(dl / rng, 4.0), 0.0, 1.0);
      }
      atten = win * win / (d2 + 1.0);
      if (L.pos.w > 2.5) {
        let cone = -dot(normalize(L.dir.xyz), dir);
        atten = atten * smoothstep(L.params.z, L.params.y, cone);
      }
    }
    let ndl = max(dot(s.n, dir), 0.0);
    if (ndl <= 0.0) {
      continue;
    }
    var shade = 1.0;
    if (li == 0) {
      shade = shadow_factor(s.w, ndl);
    }
    let hv = normalize(v + dir);
    let spec = spec_color * pow(max(dot(s.n, hv), 0.0), sh) * (sh + 8.0) / (8.0 * PI);
    color = color + (s.base / PI + spec) * L.color.rgb * ndl * atten * shade;
  }
  let n_dot_v = max(dot(s.n, v), 1e-4);
  let refl = reflect(-v, s.n);
  let spec_env = env_radiance(refl, 0.3) * env_brdf_approx(spec_color, 0.3, n_dot_v);
  color = color + env_irradiance(s.n) * s.base * s.ao * env_intensity + spec_env * env_intensity * 0.35;
  return color;
}

fn light_toon(s: Surface, steps: i32, has_gradient: bool, env_intensity: f32) -> vec3f {
  let n_steps = f32(max(steps, 2));
  var color = vec3f(0.0);
  for (var li = 0; li < 8; li = li + 1) {
    let L = cam.lights[li];
    if (L.pos.w < 0.5) {
      continue;
    }
    var dir = vec3f(0.0, 1.0, 0.0);
    var atten = 1.0;
    if (L.pos.w < 1.5) {
      dir = normalize(-L.dir.xyz);
    } else {
      let to_light = L.pos.xyz - s.w;
      let d2 = max(dot(to_light, to_light), 1e-6);
      let dl = sqrt(d2);
      dir = to_light / dl;
      let rng = L.params.x;
      var win = 1.0;
      if (rng > 1e-4) {                       /* range<=0 = 无限远（three.js/KHR_lights_punctual 语义） */
        win = clamp(1.0 - pow(dl / rng, 4.0), 0.0, 1.0);
      }
      atten = win * win / (d2 + 1.0);
      if (L.pos.w > 2.5) {
        let cone = -dot(normalize(L.dir.xyz), dir);
        atten = atten * smoothstep(L.params.z, L.params.y, cone);
      }
    }
    let ndl = max(dot(s.n, dir), 0.0);
    if (ndl <= 0.0) {
      continue;
    }
    var lit = clamp(floor(ndl * n_steps) / n_steps, 0.0, 1.0);
    if (has_gradient) {
      lit = textureSample(gradient_tex, albedo_smp, vec2f(clamp(ndl, 0.0, 1.0), 0.5)).r;
    }
    var shade = 1.0;
    if (li == 0) {
      shade = shadow_factor(s.w, ndl);
    }
    color = color + (s.base / PI) * L.color.rgb * lit * atten * shade;
  }
  color = color + env_irradiance(s.n) * s.base * s.ao * env_intensity * 0.6;
  return color;
}
