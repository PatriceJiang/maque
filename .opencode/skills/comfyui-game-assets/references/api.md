# 本地 ComfyUI API 速查（Qwen Image 2.1）

`scripts/gen.py` 已封装了下面全部流程。**只有要改结构**（加节点、改采样器组合、
做 img2img、串 LoRA 等）才需要直接碰 API。服务器默认 `http://127.0.0.1:8188`。

## 端点

| 端点 | 用途 |
|---|---|
| `GET /system_stats` | 探活 / 看显存 |
| `POST /prompt` `{"prompt": <API格式图>}` | 提交任务，返回 `prompt_id` 与 `node_errors` |
| `GET /history/<prompt_id>` | 查结果；`{}` 表示还在排队/运行 |
| `GET /queue` | 当前队列（其他人生成时会看到排队） |
| `POST /interrupt` | 取消当前任务 |
| `GET /view?filename=..&subfolder=..&type=output` | 下载结果图（二进制） |
| `POST /upload/image`（multipart，字段 `image`） | 上传输入图到 ComfyUI `input/`，参考图必用 |
| `GET /object_info/<NodeName>` | 查节点参数 schema（写/改工作流前先查它） |

## 本机模型文件（固定三件套）

| 角色 | 文件 | 加载节点 |
|---|---|---|
| 主模型 | `qwen_image_2.1_int8_convrot.safetensors` | `UNETLoader`（weight_dtype=default） |
| 文本编码器 | `qwen3vl_8b_int8_convrot.safetensors` | `CLIPLoader`（**type=qwen_image**） |
| VAE | `qwen_image_2.1_vae_bf16.safetensors` | `VAELoader` |

目录：`~/Ext/ComfyUI/models/{diffusion_models,text_encoders,vae}/`。
官方模板（含 2.1 的 t2i / image edit JSON）在
`~/Ext/ComfyUI/.venv/lib/python3.12/site-packages/comfyui_workflow_templates_json/templates/image_qwen_image_2_1_*.json`
——注意它们是前端格式（`nodes[]`），**不能直接提交**；节点参数以 `/object_info` 为准。

## 文生图 API 格式图（已实测通过）

```json
{
  "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "qwen_image_2.1_int8_convrot.safetensors", "weight_dtype": "default"}},
  "2": {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen3vl_8b_int8_convrot.safetensors", "type": "qwen_image", "device": "default"}},
  "3": {"class_type": "VAELoader",  "inputs": {"vae_name": "qwen_image_2.1_vae_bf16.safetensors"}},
  "4": {"class_type": "TextEncodeQwenImage21",
        "inputs": {"clip": ["2", 0], "prompt": "…", "negative_prompt": "", "resolution": 1024}},
  "5": {"class_type": "EmptyLatentImage", "inputs": {"width": 1024, "height": 1024, "batch_size": 1}},
  "6": {"class_type": "KSampler",
        "inputs": {"model": ["1", 0], "positive": ["4", 0], "negative": ["4", 1],
                   "latent_image": ["5", 0], "seed": 42, "steps": 25, "cfg": 1.0,
                   "sampler_name": "euler", "scheduler": "simple", "denoise": 1.0}},
  "7": {"class_type": "VAEDecode", "inputs": {"samples": ["6", 0], "vae": ["3", 0]}},
  "8": {"class_type": "SaveImage", "inputs": {"images": ["7", 0], "filename_prefix": "skillgen/out"}}
}
```

要点：

- `TextEncodeQwenImage21` 一次输出 positive / negative 两个 conditioning（slot 0、1），
  第三个输出 slot 2 是"按参考图尺寸的空 latent"（纯文生图用不上）。
- `cfg=1` 时 `negative_prompt` 不生效（官方默认路径）；要 negative 就同时调大 cfg。
- 宽高建议 32 的倍数；`batch_size` = 一次出几张（种子相同但每张噪声不同，可复现）。
- 采样参数来自官方 2.1 模板：`euler + simple`，25 步起步；官方说 40–50 步更精。

## 参考图（一致性 / 改图）

1. 上传：`curl -F "image=@ref.png" -F "overwrite=true" http://127.0.0.1:8188/upload/image`
   → 返回 `{"name": "...", "subfolder": "", "type": "input"}`；
   `LoadImage` 的 `image` 填 `subfolder/name`（无 subfolder 就只填 name）。
2. 图上接：`{"class_type": "LoadImage", "inputs": {"image": "ref.png"}}`。
3. 在 `TextEncodeQwenImage21` 的 inputs 里加**扁平键**（autogrow 输入）：
   `"images.image_1": ["9", 0]`、`"images.image_2": ["10", 0]` …（最多 16）。
   同时把 `"vae": ["3", 0]` 接上——这样参考图会作为 reference latent 注入，保真最强
   （官方 edit 模板的接法）。
4. prompt 里用 `<image1>` `<image2>` 指代参考图。

## 可选增强节点（按需加）

- `QwenImage21Cache`（`device=auto/gpu/cpu/off`, `dtype=default/int8/int4`）：
  缓存 KV，改图工作流提速明显；显存紧张时 `dtype=int8`。
- `ResolutionSelector`：按宽高比 + 百万像素选尺寸（官方模板用它，API 里直接用
  `EmptyLatentImage` 宽高即可）。
- `SaveImageAdvanced`：指定 png/bit depth/色彩空间（`SaveImage` 默认 png 就够）。
- LoRA（如 `Qwen-Image-Lightning-4steps-V1.0.safetensors`）走 `LoraLoaderModelOnly`，
  采样步数可降到 4–8；目前没有 2.1 专用加速 LoRA，**默认不要挂**。

## 排错

- `POST /prompt` 返回里 `node_errors` 非空 → 参数名/类型不对，按报错对照 `/object_info` 改。
- 失败任务在 `GET /history/<id>` 的 `status.messages` 里找 `execution_error`
  （含 `node_id`、异常文本）。
- 一直不完成：`GET /queue` 看是否排队；显存不足看 ComfyUI 日志
  `~/Ext/ComfyUI/logs/` 或启动终端；`POST /interrupt` 可取消。
- 服务器没开：`cd ~/Ext/ComfyUI && nohup .venv/bin/python3 main.py >> logs/server.log 2>&1 &`
  （gen.py 会自动做这件事）。
