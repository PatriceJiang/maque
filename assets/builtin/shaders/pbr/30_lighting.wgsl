// 光照模型：Standard/Physical（GGX）、Lambert、Phong、Toon。
// 8 盏灯循环（Directional/Point/Spot），阴影只作用于第 0 盏（方向光，M5c 约定）。
// 环境项 = 常量环境色近似（无 IBL），env_intensity 缩放。

/* 标准 metallic-roughness 光照（Standard / Physical 共用）。 */
fn light_standard(s: Surface, f0: vec3f, n_cc: vec3f, cc_factor: f32, cc_roughness: f32,
                  env_intensity: f32) -> vec3f {
  let v = normalize(cam.eye.xyz - s.w);
  let n_dot_v = max(dot(s.n, v), 1e-4);
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
      let rng = max(L.params.x, 1e-4);
      let win = clamp(1.0 - pow(dl / rng, 4.0), 0.0, 1.0);
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
    let dg = distribution_ggx(ndh, s.rough);
    let gg = geometry_smith(n_dot_v, ndl, s.rough);
    let sp = dg * gg * fl / (4.0 * n_dot_v * ndl + 1e-4);
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
  }
  let spec_amb = mix(vec3f(0.04), s.base, s.metallic) * cam.ambient.rgb * (1.0 - s.rough) * 0.35 * env_intensity;
  color = color + cam.ambient.rgb * s.base * s.ao * (1.0 - s.metallic) * env_intensity + spec_amb * s.ao;
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
      let rng = max(L.params.x, 1e-4);
      let win = clamp(1.0 - pow(dl / rng, 4.0), 0.0, 1.0);
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
  color = color + cam.ambient.rgb * s.base * s.ao * env_intensity;
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
      let rng = max(L.params.x, 1e-4);
      let win = clamp(1.0 - pow(dl / rng, 4.0), 0.0, 1.0);
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
  let spec_amb = spec_color * cam.ambient.rgb * 0.2 * env_intensity;
  color = color + cam.ambient.rgb * s.base * s.ao * env_intensity + spec_amb * s.ao;
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
      let rng = max(L.params.x, 1e-4);
      let win = clamp(1.0 - pow(dl / rng, 4.0), 0.0, 1.0);
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
  color = color + cam.ambient.rgb * s.base * s.ao * env_intensity * 0.7;
  return color;
}
