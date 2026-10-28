/* 运行时 glTF 2.0 / GLB 解析（CPU 侧，无 GPU/无全局状态；plans/23、plans/24）。
 *
 * 解析范围：buffers/bufferViews/accessors、meshes/primitives（静态 + 蒙皮 + morph targets）、
 * nodes 层级（TRS/matrix，供 runtime 逐帧算世界矩阵）、skins（joints + inverse bind）、
 * animations（translation/rotation/scale/weights，STEP/LINEAR/CUBICSPLINE）、
 * materials/images/textures/samplers（核心 PBR；扩展见 plans/24 P4）。
 *
 * 顶点保留 **mesh 空间**（不再把节点世界变换烘焙进顶点）：蒙皮/动画需要逐帧节点世界矩阵；
 * runtime 侧对非蒙皮 primitive 应用 `实体世界 × 节点世界`，对蒙皮 primitive 应用实体世界 +
 * 关节调色板（与 pbr shader 的 `wp = model × (skin × pos)` 一致）。
 *
 * 不支持（显式记录/跳过）：Draco/meshopt、KTX2/BasisU/WebP、sparse accessor、动画 pointer 通道。
 *
 * 调用方（runtime）负责：VFS 读取（ReadFn）、图像解码/GPU 上传、材质字段映射到
 * mq_renderer_pbr_material、动画采样/蒙皮调色板/morph 上传。所有外部 URI 都通过 ReadFn
 * 解析（相对 glTF 文件目录）。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gltf
{

/* glTF sampler（映射到引擎纹理参数：0=nearest 1=linear；wrap 0=repeat 1=clamp 2=mirror）。 */
struct Sampler
{
    int filter = 1;
    int wrap_u = 0;
    int wrap_v = 0;
};

/* 一条材质贴图引用（texinfo → texture → image + sampler）。 */
struct TextureRef
{
    bool present = false;
    bool srgb = false; /* 颜色贴图（baseColor/emissive）= true；数据贴图 = false */
    int image = -1;
    int texcoord = 0; /* 0 = TEXCOORD_0，1 = TEXCOORD_1 */
    Sampler sampler;
    /* KHR_texture_transform（offset/scale/rotation；rotation 为弧度，glTF T*R*S）。 */
    bool has_transform = false;
    float offset[2] = {0.0F, 0.0F};
    float scale[2] = {1.0F, 1.0F};
    float rotation = 0.0F;
};

struct Material
{
    std::string name;
    float base_color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    float emissive[3] = {0.0F, 0.0F, 0.0F};
    float metallic = 1.0F;
    float roughness = 1.0F;
    float normal_scale = 1.0F;
    float occlusion_strength = 1.0F;
    float alpha_cutoff = 0.5F;
    int alpha_mode = 0; /* 0=OPAQUE 1=MASK 2=BLEND */
    bool double_sided = false;
    bool unlit = false;
    bool physical = false; /* 有 clearcoat/sheen/iridescence/specular/ior/anisotropy → Physical 着色模型 */
    float emissive_strength = 1.0F;

    /* plans/24 P4：KHR 材质扩展（因子；默认值 = glTF 规范默认）。 */
    float clearcoat = 0.0F;
    float clearcoat_roughness = 0.0F;
    float sheen = 0.0F; /* 扩展存在即 1（引擎无 sheen 开关） */
    float sheen_roughness = 0.0F;
    float sheen_color[3] = {0.0F, 0.0F, 0.0F};
    float iridescence = 0.0F;
    float iridescence_ior = 1.3F;
    float iridescence_thickness_min = 100.0F;
    float iridescence_thickness_max = 400.0F;
    float specular_intensity = 1.0F;
    float specular_color[3] = {1.0F, 1.0F, 1.0F};
    float ior = 1.5F;
    float anisotropy = 0.0F;
    float anisotropy_rotation = 0.0F;
    float transmission = 0.0F;
    float volume_thickness = 0.0F;
    float attenuation_distance = 0.0F;
    float attenuation_color[3] = {1.0F, 1.0F, 1.0F};
    float dispersion = 0.0F;
    float diffuse_transmission = 0.0F;
    float diffuse_transmission_color[3] = {1.0F, 1.0F, 1.0F};
    float multiscatter_color[3] = {0.0F, 0.0F, 0.0F};
    float opacity = 1.0F;             /* transmission 近似的额外 alpha（与 importer 同语义） */
    bool alpha_mask_in_blend = false; /* 原 MASK + 改 BLEND 时保留 alpha 镂空 */

    TextureRef base_color_tex;
    TextureRef metallic_roughness_tex;
    TextureRef normal_tex;
    TextureRef occlusion_tex;
    TextureRef emissive_tex;
    /* KHR 扩展贴图槽（与引擎 mq_renderer_pbr_material 的 slots 对应）。 */
    TextureRef clearcoat_tex;
    TextureRef clearcoat_roughness_tex;
    TextureRef clearcoat_normal_tex;
    TextureRef sheen_color_tex;
    TextureRef sheen_roughness_tex;
    TextureRef iridescence_tex;
    TextureRef iridescence_thickness_tex;
    TextureRef anisotropy_tex;
    TextureRef specular_tex;
    TextureRef specular_color_tex;
    TextureRef transmission_tex;
    TextureRef volume_thickness_tex;
    TextureRef diffuse_transmission_tex;
    TextureRef diffuse_transmission_color_tex;
};

/* 编码后的图像字节（PNG/JPEG 原样；解码由 runtime 的 image 模块完成）。 */
struct Image
{
    std::string uri; /* 诊断用（外部路径或 "bufferView:<i>"） */
    std::vector<uint8_t> bytes;
    bool valid = false;
};

/* 节点（glTF nodes[]）：parent 在解析 children 时建立；matrix 与 TRS 二选一（规范）。
 * runtime 静态使用时优先 matrix；一旦有动画通道覆盖该节点，则用 TRS 重新组合。 */
struct Node
{
    std::string name;
    int parent = -1;
    float translation[3] = {0.0F, 0.0F, 0.0F};
    float rotation[4] = {0.0F, 0.0F, 0.0F, 1.0F}; /* (x,y,z,w) */
    float scale[3] = {1.0F, 1.0F, 1.0F};
    bool has_matrix = false;
    float matrix[16] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    bool hidden = false; /* KHR_node_visibility.visible=false（含祖先传播） */
};

/* 蒙皮（glTF skins[]）：joints = node 下标；inverse_bind 列主序 mat4（16 × joint_count）。 */
struct Skin
{
    std::string name;
    std::vector<int> joints;
    std::vector<float> inverse_bind;
};

/* morph target 增量（与顶点等长；normals 可空）。 */
struct MorphTarget
{
    std::vector<float> positions; /* 3 × vertex_count */
    std::vector<float> normals;   /* 3 × vertex_count（可空） */
};

/* 顶点基础布局（不含蒙皮尾部；runtime 按蒙皮追加 24B 后再上传）：
 *   layout 0 = pos3|normal3|tangent4|uv0_2（12 float / 48B）
 *   layout 1 = pos3|normal3|color4|uv0_2|uv1_2（14 float / 56B） */
struct Primitive
{
    std::vector<float> vertices;  /* mesh 空间，行宽 12/14 float */
    std::vector<uint16_t> joints; /* 4 / 顶点（有 skin 时） */
    std::vector<float> weights;   /* 4 / 顶点（有 skin 时，已归一化） */
    std::vector<uint32_t> indices;
    int vertex_layout = 0; /* 0/1；蒙皮由 runtime 升级为 2/3 */
    float bounds_min[3] = {0.0F, 0.0F, 0.0F};
    float bounds_max[3] = {0.0F, 0.0F, 0.0F};
    int material = -1; /* materials[] 下标；-1 = 默认材质 */
    int node = -1;     /* 来源节点（mesh 节点） */
    int skin = -1;     /* skins[] 下标；-1 = 非蒙皮 */
    /* KHR_materials_variants：每个变体索引 → 材质下标（-1 = 未映射，保持 material）。 */
    std::vector<int> variant_materials;
    std::vector<MorphTarget> morph_targets;
    std::vector<float> morph_weights; /* 该 primitive 的默认 morph 权重（node.weights > mesh.weights） */
};

/* KHR_animation_pointer：只支持材质数值属性（材质/贴图指针通道的常用子集）。 */
enum AnimationPointerField : int
{
    PF_NONE = 0,
    PF_BASE_COLOR = 1, /* vec4 */
    PF_METALLIC = 2,   /* scalar */
    PF_ROUGHNESS = 3,  /* scalar */
    PF_EMISSIVE = 4,   /* vec3 */
    PF_EMISSIVE_STRENGTH = 5,
    PF_ALPHA_CUTOFF = 6,
    PF_CLEARCOAT = 7,
    PF_CLEARCOAT_ROUGHNESS = 8,
    PF_SHEEN_COLOR = 9, /* vec3 */
    PF_SHEEN_ROUGHNESS = 10,
    PF_IOR = 11,
    PF_TRANSMISSION = 12,
    PF_VOLUME_THICKNESS = 13,
    PF_ATTENUATION_DISTANCE = 14,
    PF_DIFFUSE_TRANSMISSION = 15,
    PF_SPECULAR = 16,
    PF_IRIDESCENCE = 17,
    PF_ANISOTROPY = 18,
    PF_DISPERSION = 19,
};

/* 动画通道：path 0=translation 1=rotation 2=scale 3=weights 4=pointer；
 * interp 0=STEP 1=LINEAR 2=CUBICSPLINE。
 * CUBICSPLINE 时 values 每个关键帧存 3 组（in-tangent, value, out-tangent），comps 是单组分量数。 */
struct AnimationChannel
{
    int node = -1;
    int path = 0;
    int interp = 1;
    int comps = 0;
    std::vector<float> times;
    std::vector<float> values; /* keys × comps × (interp==2 ? 3 : 1) */
    /* path==4（KHR_animation_pointer）时：目标材质 + 字段枚举。 */
    int pointer_material = -1;
    int pointer_field = PF_NONE;
};

struct Animation
{
    std::string name;
    float duration = 0.0F;
    std::vector<AnimationChannel> channels;
};

/* KHR_lights_punctual：每个引用灯光定义的节点实例一条（node = 节点下标）。 */
struct Light
{
    int node = -1;
    int type = 0; /* 0=directional 1=point 2=spot */
    float color[3] = {1.0F, 1.0F, 1.0F};
    float intensity = 1.0F;
    float range = 0.0F;       /* 0 = 无限远（引擎语义） */
    float inner_angle = 0.0F; /* 弧度（glTF 半角） */
    float outer_angle = 0.785398F;
};

/* glTF cameras[]：每个引用相机的节点实例一条；只取 perspective（ortho 记录但 runtime 回退）。 */
struct Camera
{
    int node = -1;
    bool perspective = true;
    float yfov = 0.6F; /* 弧度 */
    float znear = 0.1F;
    float zfar = 1000.0F;
    float aspect = 0.0F; /* 0 = 由视口决定 */
};

struct Model
{
    std::vector<Primitive> primitives;
    std::vector<Material> materials;
    std::vector<Image> images; /* 仅加载被材质引用到的 image */
    std::vector<Node> nodes;
    std::vector<int> node_order; /* 父先子（世界矩阵累积顺序） */
    std::vector<Skin> skins;
    std::vector<Animation> animations;
    std::vector<Light> lights;
    std::vector<Camera> cameras;
    std::vector<std::string> variants; /* KHR_materials_variants 名称（按索引） */
    bool has_skin = false;
    bool has_animation = false;
    bool has_morph = false;
    bool failed = false;
    std::string error;
};

using ReadFn = std::function<bool(const std::string& path, std::vector<uint8_t>& out)>;

/* path = glTF 在 VFS 中的路径（用于解析相对 URI 与诊断）；bytes = 文件内容（GLB 也直接传）。
 * read 用于读取外部 buffer / image（参数是已拼接的 VFS 路径）。 */
Model parse(const std::string& path, const std::vector<uint8_t>& bytes, const ReadFn& read);

bool is_gltf_path(const std::string& path);

} // namespace gltf
