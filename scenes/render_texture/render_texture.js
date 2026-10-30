function on_start() {
  const camera = entity_find("capture_camera");
  const ref = camera_texture(camera);
  log("render texture=" + ref);
  camera_update(camera);
  const path = env("MAQUE_RT_SHOT");
  if (path !== "") camera_capture(camera, path);
}
