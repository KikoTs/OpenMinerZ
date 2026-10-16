// Authoring animation packs: the authoring frame, the rig export behind
// `openminerz_mod_sdk export-rig` and the checks behind `check-anim`
// (openminerz/modding/animation_authoring.hpp).

#include "openminerz/modding/animation_authoring.hpp"
#include "openminerz/modding/gunfx.hpp" // (GunFX)

#include "openminerz/retail_rig.hpp"
#include "openminerz/world/player_animation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <set>

namespace openminerz::modding::graphics
{
namespace
{

using Quat = std::array<float, 4>; // x y z w

// XNA's AvatarBone enumeration, in ordinal order. The ordinals it skips are
// the avatar's scale and twist helpers, which the game names by number.
constexpr std::array<std::string_view, rig::attachment_bone_count> bone_names{
    "Root",              "BackLower",          "HipLeft",           "HipRight",
    "4",                 "BackUpper",          "KneeLeft",          "7",
    "KneeRight",         "9",                  "10",                "AnkleLeft",
    "CollarLeft",        "13",                 "Neck",              "AnkleRight",
    "CollarRight",       "17",                 "18",                "Head",
    "ShoulderLeft",      "ToeLeft",            "ShoulderRight",     "ToeRight",
    "24",                "ElbowLeft",          "26",                "27",
    "ElbowRight",        "29",                 "30",                "31",
    "32",                "WristLeft",          "34",                "35",
    "WristRight",        "FingerIndexLeft",    "FingerMiddleLeft",  "FingerRingLeft",
    "FingerSmallLeft",   "PropLeft",           "SpecialLeft",       "FingerThumbLeft",
    "FingerIndexRight",  "FingerMiddleRight",  "FingerRingRight",   "FingerSmallRight",
    "PropRight",         "SpecialRight",       "FingerThumbRight",  "FingerIndex2Left",
    "FingerMiddle2Left", "FingerRing2Left",    "FingerSmall2Left",  "FingerThumb2Left",
    "FingerIndex2Right", "FingerMiddle2Right", "FingerRing2Right",  "FingerSmall2Right",
    "FingerThumb2Right", "FingerIndex3Left",   "FingerMiddle3Left", "FingerRing3Left",
    "FingerSmall3Left",  "FingerThumb3Left",   "FingerIndex3Right", "FingerMiddle3Right",
    "FingerRing3Right",  "FingerSmall3Right",  "FingerThumb3Right",
};

// --- Math --------------------------------------------------------------------

Quat quat_multiply(const Quat &a, const Quat &b) noexcept
{
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Quat quat_normalized(const Quat &q) noexcept
{
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 1.0e-8F))
        return {0.0F, 0.0F, 0.0F, 1.0F};
    return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}

// The angle between two rotations, in radians.
float quat_angle(const Quat &a, const Quat &b) noexcept
{
    const float cosine =
        std::min(1.0F, std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]));
    return 2.0F * std::acos(cosine);
}

float determinant3(const Matrix4 &m) noexcept
{
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
           m[8] * (m[1] * m[6] - m[5] * m[2]);
}

struct Decomposed final
{
    Vec3 translation{};
    Quat rotation{0.0F, 0.0F, 0.0F, 1.0F};
    Vec3 scale{1.0F, 1.0F, 1.0F};
    bool mirrored{};
};

// A TRS matrix (column-major, column vectors) back into its parts. A mirror
// is carried as a negative X scale.
Decomposed decompose(const Matrix4 &m) noexcept
{
    Decomposed result;
    result.translation = {m[12], m[13], m[14]};
    float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    result.mirrored = determinant3(m) < 0.0F;
    if (result.mirrored)
        sx = -sx;
    result.scale = {sx, sy, sz};
    const auto safe = [](float value) { return std::abs(value) > 1.0e-8F ? value : 1.0e-8F; };
    const float r00 = m[0] / safe(sx), r10 = m[1] / safe(sx), r20 = m[2] / safe(sx);
    const float r01 = m[4] / safe(sy), r11 = m[5] / safe(sy), r21 = m[6] / safe(sy);
    const float r02 = m[8] / safe(sz), r12 = m[9] / safe(sz), r22 = m[10] / safe(sz);
    const float trace = r00 + r11 + r22;
    Quat q{};
    if (trace > 0.0F)
    {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25F * s};
    }
    else if (r00 > r11 && r00 > r22)
    {
        const float s = std::sqrt(1.0F + r00 - r11 - r22) * 2.0F;
        q = {0.25F * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    }
    else if (r11 > r22)
    {
        const float s = std::sqrt(1.0F + r11 - r00 - r22) * 2.0F;
        q = {(r01 + r10) / s, 0.25F * s, (r12 + r21) / s, (r02 - r20) / s};
    }
    else
    {
        const float s = std::sqrt(1.0F + r22 - r00 - r11) * 2.0F;
        q = {(r02 + r20) / s, (r12 + r21) / s, 0.25F * s, (r10 - r01) / s};
    }
    result.rotation = quat_normalized(q);
    return result;
}

Matrix4 compose_node(const ModelNode &node) noexcept
{
    return matrix_compose(node.translation, node.rotation, node.scale);
}

bool near_identity(const Matrix4 &m, float tolerance) noexcept
{
    const Matrix4 identity = matrix_identity();
    for (std::size_t index = 0U; index < 16U; ++index)
        if (std::abs(m[index] - identity[index]) > tolerance)
            return false;
    return true;
}

// --- Names -------------------------------------------------------------------

bool numbered(std::string_view name) noexcept
{
    return !name.empty() &&
           std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int side_of(std::string_view name) noexcept
{
    if (name.find("Left") != std::string_view::npos)
        return 1;
    if (name.find("Right") != std::string_view::npos)
        return 2;
    return 0;
}

// Lower case, letters and digits only, a trailing side marker (".L", "_r",
// " Left") spelled out, common rig prefixes dropped.
std::string folded(std::string_view name)
{
    std::string text;
    for (const char c : name)
        text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (const std::string_view prefix : {"mixamorig:", "mixamorig_", "mixamorig", "armature|",
                                          "armature_", "def-", "org-", "bip01 ", "bip001 "})
        if (text.starts_with(prefix))
            text.erase(0U, prefix.size());
    std::string side;
    for (const auto &[suffix, word] : {std::pair{".l", "left"}, std::pair{"_l", "left"},
                                       std::pair{" l", "left"}, std::pair{"-l", "left"},
                                       std::pair{".r", "right"}, std::pair{"_r", "right"},
                                       std::pair{" r", "right"}, std::pair{"-r", "right"}})
        if (text.size() > 2U && text.ends_with(suffix))
        {
            text.resize(text.size() - 2U);
            side = word;
            break;
        }
    std::string result;
    for (const char c : text)
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
            result.push_back(c);
    return result + side;
}

std::size_t edit_distance(std::string_view a, std::string_view b)
{
    std::vector<std::size_t> row(b.size() + 1U);
    for (std::size_t j = 0U; j < row.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1U; i <= a.size(); ++i)
    {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1U; j <= b.size(); ++j)
        {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1U, row[j - 1U] + 1U,
                               diagonal + (a[i - 1U] == b[j - 1U] ? 0U : 1U)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

// What other rigs call these joints (Mixamo, Blender's Rigify/metarig, Unity
// humanoids), folded, for a "did you mean". Renaming is not retargeting: the
// game reads rotations in its own bones' rest frames.
std::string common_alias(const std::string &name)
{
    static const std::map<std::string, std::string, std::less<>> aliases = [] {
        std::map<std::string, std::string, std::less<>> table{
            {"hips", "Root"},        {"pelvis", "Root"},       {"spine", "BackLower"},
            {"spine1", "BackUpper"}, {"spine2", "BackUpper"},  {"chest", "BackUpper"},
            {"upperchest", "BackUpper"}, {"neck", "Neck"},     {"head", "Head"},
        };
        const std::pair<std::string_view, std::string_view> limbs[]{
            {"shoulder", "Collar"}, {"clavicle", "Collar"},  {"arm", "Shoulder"},
            {"upperarm", "Shoulder"}, {"forearm", "Elbow"},  {"lowerarm", "Elbow"},
            {"hand", "Wrist"},      {"upleg", "Hip"},         {"thigh", "Hip"},
            {"upperleg", "Hip"},    {"leg", "Knee"},          {"shin", "Knee"},
            {"calf", "Knee"},       {"lowerleg", "Knee"},     {"foot", "Ankle"},
            {"toebase", "Toe"},     {"toe", "Toe"},
        };
        for (const auto &[limb, bone] : limbs)
            for (const std::string_view side : {"left", "right"})
            {
                const std::string target = std::string{bone} + (side == "left" ? "Left" : "Right");
                table.emplace(std::string{side} + std::string{limb}, target);
                table.emplace(std::string{limb} + std::string{side}, target);
            }
        return table;
    }();
    const auto found = aliases.find(name);
    return found == aliases.end() ? std::string{} : found->second;
}

std::string bone_suggestion(std::string_view name, std::span<const std::string_view> bones)
{
    const std::string key = folded(name);
    for (const std::string_view bone : bones)
        if (folded(bone) == key)
            return std::format(" (did you mean {}?)", bone);
    if (const std::string alias = common_alias(key); !alias.empty())
        return std::format(" (another rig's name for {}? Renaming is not enough: the game reads "
                           "rotations in its own bones' rest frames)",
                           alias);
    std::string_view best;
    std::size_t best_distance = 4U;
    for (const std::string_view bone : bones)
    {
        if (numbered(bone))
            continue;
        const std::size_t distance = edit_distance(key, folded(bone));
        if (distance < best_distance)
        {
            best_distance = distance;
            best = bone;
        }
    }
    return best.empty() ? std::string{} : std::format(" (did you mean {}?)", best);
}

std::string clip_suggestion(std::string_view name, const std::vector<std::string> &clips)
{
    const std::string key = folded(name);
    std::string_view best;
    std::size_t best_distance = 4U;
    for (const std::string &clip : clips)
    {
        const std::size_t distance = edit_distance(key, folded(clip));
        if (distance < best_distance)
        {
            best_distance = distance;
            best = clip;
        }
    }
    return best.empty() ? std::string{} : std::format(" (did you mean {}?)", best);
}

std::string joined(const std::vector<std::string> &items, std::size_t limit = 12U)
{
    std::string text;
    for (std::size_t index = 0U; index < items.size() && index < limit; ++index)
        text += (index == 0U ? "" : ", ") + items[index];
    if (items.size() > limit)
        text += std::format(" and {} more", items.size() - limit);
    return text;
}

// --- glTF writing ------------------------------------------------------------

std::string json_string(std::string_view text)
{
    std::string result{"\""};
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            result += '\\';
        if (static_cast<unsigned char>(c) < 0x20U)
            result += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else
            result += c;
    }
    return result + "\"";
}

std::string json_number(float value)
{
    return std::isfinite(value) ? std::format("{}", value) : std::string{"0"};
}

std::string json_floats(std::span<const float> values)
{
    std::string text{"["};
    for (std::size_t index = 0U; index < values.size(); ++index)
        text += (index == 0U ? "" : ",") + json_number(values[index]);
    return text + "]";
}

class GltfBuffer final
{
  public:
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> views;
    std::vector<std::string> accessors;

    // Floats as VEC3/VEC4/SCALAR/MAT4 (`components` per element), optionally
    // with the min/max glTF requires of positions and key times.
    int floats(std::span<const float> values, std::size_t components, std::string_view type,
               bool bounds, int target = 0)
    {
        const int view = add_view(values.data(), values.size_bytes(), target);
        std::string extra;
        if (bounds && !values.empty())
        {
            std::vector<float> low(components, values[0]);
            std::vector<float> high(components, values[0]);
            for (std::size_t index = 0U; index < values.size(); ++index)
            {
                low[index % components] = std::min(low[index % components], values[index]);
                high[index % components] = std::max(high[index % components], values[index]);
            }
            extra = std::format(R"(,"min":{},"max":{})", json_floats(low), json_floats(high));
        }
        return add_accessor(view, 5126, values.size() / components, type, extra);
    }

    int bytes4(std::span<const std::uint8_t> values)
    {
        const int view = add_view(values.data(), values.size(), 34962);
        return add_accessor(view, 5121, values.size() / 4U, "VEC4", {});
    }

    int indices(std::span<const std::uint32_t> values)
    {
        const int view = add_view(values.data(), values.size_bytes(), 34963);
        return add_accessor(view, 5125, values.size(), "SCALAR", {});
    }

  private:
    int add_view(const void *data, std::size_t size, int target)
    {
        while (bytes.size() % 4U != 0U)
            bytes.push_back(0U);
        const std::size_t offset = bytes.size();
        const auto *source = static_cast<const std::uint8_t *>(data);
        bytes.insert(bytes.end(), source, source + size);
        views.push_back(std::format(R"({{"buffer":0,"byteOffset":{},"byteLength":{}{}}})", offset,
                                    size, target != 0 ? std::format(R"(,"target":{})", target)
                                                      : std::string{}));
        return static_cast<int>(views.size()) - 1;
    }

    int add_accessor(int view, int component, std::size_t count, std::string_view type,
                     const std::string &extra)
    {
        accessors.push_back(
            std::format(R"({{"bufferView":{},"componentType":{},"count":{},"type":"{}"{}}})", view,
                        component, count, type, extra));
        return static_cast<int>(accessors.size()) - 1;
    }
};

void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::vector<std::uint8_t> glb(std::string json, std::vector<std::uint8_t> bin)
{
    while (json.size() % 4U != 0U)
        json.push_back(' ');
    while (bin.size() % 4U != 0U)
        bin.push_back(0U);
    std::vector<std::uint8_t> out;
    out.reserve(28U + json.size() + bin.size());
    append_u32(out, 0x46546C67U); // "glTF"
    append_u32(out, 2U);
    append_u32(out, static_cast<std::uint32_t>(28U + json.size() + bin.size()));
    append_u32(out, static_cast<std::uint32_t>(json.size()));
    append_u32(out, 0x4E4F534AU); // "JSON"
    out.insert(out.end(), json.begin(), json.end());
    append_u32(out, static_cast<std::uint32_t>(bin.size()));
    append_u32(out, 0x004E4942U); // "BIN"
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

// --- The proxy body ----------------------------------------------------------

struct ProxyBox final
{
    int bone{};
    int side{}; // 0 centre, 1 left, 2 right
    Vec3 from{};
    Vec3 to{};
    float width{};
    float depth{};
};

struct ProxyMesh final
{
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<std::uint8_t> joints;
    std::vector<float> weights;
    std::vector<std::uint32_t> indices;
};

void add_box(ProxyMesh &mesh, const ProxyBox &box)
{
    // A right-handed basis (s, t, u) along the box; the faces wind outward.
    const Vec3 u = vfx::normalized(box.to - box.from);
    const Vec3 reference = std::abs(u.z) < 0.9F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
    const Vec3 s = vfx::normalized(vfx::cross(reference, u), {1.0F, 0.0F, 0.0F});
    const Vec3 t = vfx::cross(u, s);
    const Vec3 centre = (box.from + box.to) * 0.5F;
    const float half_length = vfx::length(box.to - box.from) * 0.5F;
    struct Face final
    {
        Vec3 normal;
        float offset;
        Vec3 first;
        float first_half;
        Vec3 second;
        float second_half;
    };
    const Face faces[]{
        {u, half_length, s, box.width, t, box.depth},
        {u * -1.0F, half_length, t, box.depth, s, box.width},
        {s, box.width, t, box.depth, u, half_length},
        {s * -1.0F, box.width, u, half_length, t, box.depth},
        {t, box.depth, u, half_length, s, box.width},
        {t * -1.0F, box.depth, s, box.width, u, half_length},
    };
    for (const Face &face : faces)
    {
        const auto base = static_cast<std::uint32_t>(mesh.positions.size() / 3U);
        const Vec3 middle = centre + face.normal * face.offset;
        const Vec3 a = face.first * face.first_half;
        const Vec3 b = face.second * face.second_half;
        for (const Vec3 corner : {middle - a - b, middle + a - b, middle + a + b, middle - a + b})
        {
            mesh.positions.insert(mesh.positions.end(), {corner.x, corner.y, corner.z});
            mesh.normals.insert(mesh.normals.end(), {face.normal.x, face.normal.y, face.normal.z});
            mesh.joints.insert(mesh.joints.end(),
                               {static_cast<std::uint8_t>(box.bone), std::uint8_t{0},
                                std::uint8_t{0}, std::uint8_t{0}});
            mesh.weights.insert(mesh.weights.end(), {1.0F, 0.0F, 0.0F, 0.0F});
        }
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1U, base + 2U, base, base + 2U, base + 3U});
    }
}

// A box along every limb of the rest pose: from each joint to each named
// child, plus the pelvis, the head and the tips of toes and fingers.
std::vector<ProxyBox> proxy_boxes(const std::vector<AuthoringBone> &skeleton)
{
    const auto at = [&](std::size_t bone) {
        const Matrix4 &m = skeleton[bone].world;
        return Vec3{m[12], m[13], m[14]};
    };
    const auto shown = [&](std::size_t bone) {
        const std::string &name = skeleton[bone].name;
        return !numbered(name) && !name.starts_with("Prop") && !name.starts_with("Special");
    };
    std::vector<ProxyBox> boxes;
    boxes.push_back({0, 0, at(0U) + Vec3{0.0F, -0.13F, 0.0F}, at(0U) + Vec3{0.0F, 0.06F, 0.0F},
                     0.13F, 0.08F});
    for (std::size_t bone = 0U; bone < skeleton.size(); ++bone)
    {
        if (!shown(bone))
            continue;
        bool has_child = false;
        for (std::size_t child = 1U; child < skeleton.size(); ++child)
        {
            if (skeleton[child].parent != static_cast<int>(bone) || !shown(child))
                continue;
            has_child = true;
            const Vec3 from = at(bone);
            const Vec3 to = at(child);
            const float length = vfx::length(to - from);
            if (length < 0.03F)
                continue;
            const std::string &name = skeleton[bone].name;
            const bool torso = name == "BackLower" || (name == "BackUpper" &&
                                                       skeleton[child].name == "Neck");
            const float width = torso ? 0.12F : std::clamp(length * 0.2F, 0.008F, 0.055F);
            boxes.push_back({static_cast<int>(bone), side_of(skeleton[child].name), from, to, width,
                             torso ? 0.075F : width});
        }
        if (has_child)
            continue;
        const std::string &name = skeleton[bone].name;
        const Vec3 p = at(bone);
        if (name == "Head")
            boxes.push_back({static_cast<int>(bone), 0, p + Vec3{0.0F, 0.0F, 0.02F},
                             p + Vec3{0.0F, 0.22F, 0.02F}, 0.1F, 0.11F});
        else if (name.starts_with("Toe"))
            boxes.push_back({static_cast<int>(bone), side_of(name), p,
                             p + Vec3{0.0F, 0.0F, 0.05F}, 0.035F, 0.02F});
        else if (skeleton[bone].parent >= 0)
        {
            // A finger tip continues its finger.
            const Vec3 along = p - at(static_cast<std::size_t>(skeleton[bone].parent));
            boxes.push_back({static_cast<int>(bone), side_of(name), p, p + along * 0.8F, 0.008F,
                             0.008F});
        }
    }
    return boxes;
}

const rig::Registration *find_registration(const rig::RetailRig &rig, std::string_view name)
{
    return rig.find(name);
}

// The bones of an influence mask whose parent it leaves out: "CollarRight"
// for a clip that moves the right arm.
std::vector<std::string> influence_roots(const rig::RetailRig &rig, const rig::BoneMask &mask)
{
    std::vector<std::string> roots;
    for (std::size_t bone = 0U; bone < mask.size(); ++bone)
    {
        const int parent = rig.attachment[bone].parent;
        if (mask[bone] && (parent < 0 || !mask[static_cast<std::size_t>(parent)]))
            roots.push_back(rig.attachment[bone].name);
    }
    return roots;
}

// How the game plays a clip `seconds` long in place of a registration that
// lasts `game` seconds.
std::string timing_text(std::string_view label, std::string_view registration, double game, bool looping,
                        double seconds)
{
    std::string text = std::format("{}: the game plays {} for {:.3f} s ({:.1f} frames at 30 fps, {})", label,
                                   registration, game, game * clip_frame_rate, looping ? "looping" : "once");
    if (seconds <= 0.0)
        return text + "; this clip is one pose, held throughout";
    if (std::abs(seconds - game) <= 1.0 / clip_frame_rate + 1.0e-4)
        return text + "; this clip fits";
    if (seconds < game)
        return text + std::format("; this clip ends {:.3f} s early and holds its last pose", game - seconds);
    return text + std::format("; the last {:.3f} s of this clip are never seen", seconds - game);
}

} // namespace

// --- Bones and the frame -------------------------------------------------------

std::span<const std::string_view> avatar_bone_names() noexcept
{
    return bone_names;
}

int find_rig_bone(const rig::RetailRig &rig, std::string_view name) noexcept
{
    if (name.empty())
        return -1;
    for (std::size_t bone = 0U; bone < rig.attachment.size(); ++bone)
        if (rig.attachment[bone].name == name)
            return static_cast<int>(bone);
    for (std::size_t bone = 0U; bone < rig.attachment.size(); ++bone)
        if (!rig.attachment[bone].native_name.empty() && rig.attachment[bone].native_name == name)
            return static_cast<int>(bone);
    return -1;
}

Matrix4 authoring_from_avatar() noexcept
{
    return {-1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,  0.0F,
            0.0F,  0.0F, -1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

Matrix4 authoring_frame(const rig::RetailRig &rig) noexcept
{
    // Avatar.BindPose[0], an XNA row-vector matrix: its sixteen floats ARE the
    // column-major column-vector matrix glTF uses.
    const Matrix4 &bind = rig.attachment[0].bind;
    const float determinant = determinant3(bind);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-6F ||
        std::abs(bind[15] - 1.0F) > 1.0e-4F)
        return matrix_identity();
    return matrix_multiply(authoring_from_avatar(), bind);
}

std::vector<AuthoringBone> authoring_skeleton(const rig::RetailRig &rig)
{
    std::vector<AuthoringBone> bones(rig.attachment.size());
    for (std::size_t bone = 0U; bone < bones.size(); ++bone)
    {
        const rig::AttachmentBone &source = rig.attachment[bone];
        AuthoringBone &out = bones[bone];
        out.name = source.name.empty() ? std::string{bone_names[bone]} : source.name;
        out.parent = bone == 0U ? -1 : source.parent;
        if (bone == 0U)
        {
            const Decomposed frame = decompose(authoring_frame(rig));
            out.translation = frame.translation;
            // The game's half turn cancels the bind pose's to within float
            // noise; a clean identity reads better in Blender.
            if (std::abs(frame.rotation[0]) > 1.0e-6F || std::abs(frame.rotation[1]) > 1.0e-6F ||
                std::abs(frame.rotation[2]) > 1.0e-6F)
                out.rotation = frame.rotation;
        }
        else
            // Avatar.OnUpdate keeps each joint's bind offset; at rest no joint turns.
            out.translation = {source.bind[12], source.bind[13], source.bind[14]};
        const Matrix4 local = matrix_compose(out.translation, out.rotation, {1.0F, 1.0F, 1.0F});
        out.world = out.parent < 0 ? local
                                   : matrix_multiply(bones[static_cast<std::size_t>(out.parent)].world, local);
    }
    return bones;
}

BoneNodes match_bones(const Model &model, const rig::RetailRig &rig)
{
    BoneNodes nodes;
    nodes.fill(-1);
    // A bone's own name wins over the body bone it drives.
    for (const bool native : {false, true})
        for (std::size_t bone = 0U; bone < nodes.size(); ++bone)
        {
            if (nodes[bone] >= 0)
                continue;
            const std::string &name =
                native ? rig.attachment[bone].native_name : rig.attachment[bone].name;
            if (name.empty())
                continue;
            for (std::size_t node = 0U; node < model.nodes.size(); ++node)
                if (model.nodes[node].name == name)
                {
                    nodes[bone] = static_cast<int>(node);
                    break;
                }
        }
    return nodes;
}

void sample_clip_pose(const Model &model, int animation, float seconds,
                      const rig::RetailRig &rig, const BoneNodes &nodes, rig::BonePose &pose)
{
    std::vector<ModelNode> local;
    sample_animation(model, animation, seconds, local);
    for (std::size_t bone = 0U; bone < nodes.size(); ++bone)
    {
        const int node = nodes[bone];
        if (node < 0 || static_cast<std::size_t>(node) >= local.size())
            continue;
        rig::BoneTransform &out = pose[bone];
        if (bone == 0U)
        {
            // The Root where the file draws it, whatever sits above it, taken
            // back out of the authoring frame.
            Matrix4 world = compose_node(local[static_cast<std::size_t>(node)]);
            int parent = model.nodes[static_cast<std::size_t>(node)].parent;
            for (std::size_t guard = 0U; parent >= 0 && guard < local.size(); ++guard)
            {
                world = matrix_multiply(compose_node(local[static_cast<std::size_t>(parent)]), world);
                parent = model.nodes[static_cast<std::size_t>(parent)].parent;
            }
            const Decomposed root =
                decompose(matrix_multiply(rig::invert(authoring_frame(rig)), world));
            out.translation = {root.translation.x, root.translation.y, root.translation.z};
            out.rotation = {root.rotation[0], root.rotation[1], root.rotation[2], root.rotation[3]};
            out.scale = {root.scale.x, root.scale.y, root.scale.z};
            continue;
        }
        const ModelNode &value = local[static_cast<std::size_t>(node)];
        const Quat q = quat_normalized(value.rotation);
        out.translation = {value.translation.x, value.translation.y, value.translation.z};
        out.rotation = {q[0], q[1], q[2], q[3]};
        out.scale = {value.scale.x, value.scale.y, value.scale.z};
    }
}

ModelLimits animation_file_limits() noexcept
{
    ModelLimits limits;
    limits.require_mesh = false;
    return limits;
}

// --- export-rig ----------------------------------------------------------------

std::vector<std::uint8_t> export_rig(const rig::RetailRig &rig, const RigExportOptions &options,
                                     std::string *error)
{
    for (const std::string &name : options.reference_clips)
        if (find_registration(rig, name) == nullptr)
        {
            if (error != nullptr)
                *error = std::format("\"{}\" is not a clip this character registers", name);
            return {};
        }

    const std::vector<AuthoringBone> skeleton = authoring_skeleton(rig);
    const Decomposed frame = decompose(authoring_frame(rig));
    const Matrix4 frame_matrix = authoring_frame(rig);
    GltfBuffer buffer;

    // Bones, their rest pose and the skin.
    std::vector<std::vector<int>> children(skeleton.size());
    for (std::size_t bone = 1U; bone < skeleton.size(); ++bone)
        if (skeleton[bone].parent >= 0)
            children[static_cast<std::size_t>(skeleton[bone].parent)].push_back(static_cast<int>(bone));
    std::string nodes;
    for (std::size_t bone = 0U; bone < skeleton.size(); ++bone)
    {
        const AuthoringBone &source = skeleton[bone];
        nodes += std::format(R"({}{{"name":{},"translation":[{},{},{}],"rotation":[{},{},{},{}])",
                             bone == 0U ? "" : ",", json_string(source.name),
                             json_number(source.translation.x), json_number(source.translation.y),
                             json_number(source.translation.z), json_number(source.rotation[0]),
                             json_number(source.rotation[1]), json_number(source.rotation[2]),
                             json_number(source.rotation[3]));
        if (!children[bone].empty())
        {
            nodes += R"(,"children":[)";
            for (std::size_t child = 0U; child < children[bone].size(); ++child)
                nodes += std::format("{}{}", child == 0U ? "" : ",", children[bone][child]);
            nodes += "]";
        }
        nodes += "}";
    }
    std::vector<float> inverse_binds;
    inverse_binds.reserve(skeleton.size() * 16U);
    for (const AuthoringBone &bone : skeleton)
    {
        const Matrix4 inverse = rig::invert(bone.world);
        inverse_binds.insert(inverse_binds.end(), inverse.begin(), inverse.end());
    }
    const int inverse_bind_accessor = buffer.floats(inverse_binds, 16U, "MAT4", false);
    std::string joints;
    for (std::size_t bone = 0U; bone < skeleton.size(); ++bone)
        joints += std::format("{}{}", bone == 0U ? "" : ",", bone);

    // The proxy body: one primitive per colour.
    std::string meshes;
    std::string materials;
    std::string scene_nodes = "0";
    std::string skins = std::format(
        R"([{{"name":"OpenMinerZ character","inverseBindMatrices":{},"joints":[{}],"skeleton":0}}])",
        inverse_bind_accessor, joints);
    if (options.proxy_mesh)
    {
        std::array<ProxyMesh, 3> sides;
        for (const ProxyBox &box : proxy_boxes(skeleton))
            add_box(sides[static_cast<std::size_t>(box.side)], box);
        std::string primitives;
        for (std::size_t side = 0U; side < sides.size(); ++side)
        {
            const ProxyMesh &mesh = sides[side];
            if (mesh.indices.empty())
                continue;
            const int position = buffer.floats(mesh.positions, 3U, "VEC3", true, 34962);
            const int normal = buffer.floats(mesh.normals, 3U, "VEC3", false, 34962);
            const int joint = buffer.bytes4(mesh.joints);
            const int weight = buffer.floats(mesh.weights, 4U, "VEC4", false, 34962);
            const int index = buffer.indices(mesh.indices);
            primitives += std::format(
                R"({}{{"attributes":{{"POSITION":{},"NORMAL":{},"JOINTS_0":{},"WEIGHTS_0":{}}},"indices":{},"material":{}}})",
                primitives.empty() ? "" : ",", position, normal, joint, weight, index, side);
        }
        meshes = std::format(R"([{{"name":"OpenMinerZ proxy body","primitives":[{}]}}])", primitives);
        materials =
            R"json([{"name":"Centre (grey)","pbrMetallicRoughness":{"baseColorFactor":[0.72,0.72,0.68,1],"metallicFactor":0,"roughnessFactor":0.8}},)json"
            R"json({"name":"Left side (blue)","pbrMetallicRoughness":{"baseColorFactor":[0.22,0.42,0.9,1],"metallicFactor":0,"roughnessFactor":0.8}},)json"
            R"json({"name":"Right side (red)","pbrMetallicRoughness":{"baseColorFactor":[0.9,0.3,0.22,1],"metallicFactor":0,"roughnessFactor":0.8}}])json";
        nodes += std::format(R"(,{{"name":"OpenMinerZ proxy body","mesh":0,"skin":0}})");
        scene_nodes += std::format(",{}", skeleton.size());
    }

    // Reference clips, exactly as the game holds them: every bone the
    // registration lets the clip write, rotation keys as they are, the Root
    // through the authoring frame. Other joints keep their rest offsets (the
    // engine never reads a clip's joint translations below the Root).
    std::string animations;
    std::map<std::size_t, int> times;
    const auto key_times = [&](std::size_t count, int rate) {
        const auto found = times.find(count);
        if (found != times.end())
            return found->second;
        std::vector<float> values(count);
        for (std::size_t key = 0U; key < count; ++key)
            values[key] = static_cast<float>(static_cast<double>(key) / rate);
        const int accessor = buffer.floats(values, 1U, "SCALAR", true);
        times.emplace(count, accessor);
        return accessor;
    };
    const Quat frame_rotation = frame.rotation;
    for (const std::string &name : options.reference_clips)
    {
        const rig::Registration &registration = *find_registration(rig, name);
        const rig::Clip &clip = rig.clips[static_cast<std::size_t>(registration.clip)];
        const int rate = clip.frame_rate > 0 ? clip.frame_rate : clip_frame_rate;
        std::string channels;
        std::string samplers;
        int sampler = 0;
        const auto add = [&](std::size_t bone, std::string_view path, std::size_t keys,
                             std::vector<float> values, std::size_t components) {
            const int input = key_times(keys, rate);
            const int output =
                buffer.floats(values, components, components == 4U ? "VEC4" : "VEC3", false);
            samplers += std::format(R"({}{{"input":{},"output":{},"interpolation":"LINEAR"}})",
                                    sampler == 0 ? "" : ",", input, output);
            channels += std::format(R"({}{{"sampler":{},"target":{{"node":{},"path":"{}"}}}})",
                                    sampler == 0 ? "" : ",", sampler, bone, path);
            ++sampler;
        };
        for (std::size_t bone = 0U; bone < skeleton.size(); ++bone)
        {
            if (!registration.influence[bone] || clip.rotations[bone].empty())
                continue;
            std::vector<float> rotations;
            for (const rig::Quat &key : clip.rotations[bone])
            {
                Quat q{key.x, key.y, key.z, key.w};
                if (bone == 0U)
                    q = quat_normalized(quat_multiply(frame_rotation, q));
                rotations.insert(rotations.end(), q.begin(), q.end());
            }
            add(bone, "rotation", clip.rotations[bone].size(), std::move(rotations), 4U);
            if (bone == 0U && !clip.positions[0].empty())
            {
                std::vector<float> positions;
                for (const rig::Vec3 &key : clip.positions[0])
                {
                    const Vec3 p = transform_point(frame_matrix, {key.x, key.y, key.z});
                    positions.insert(positions.end(), {p.x, p.y, p.z});
                }
                add(0U, "translation", clip.positions[0].size(), std::move(positions), 3U);
            }
            const bool scaled =
                std::any_of(clip.scales[bone].begin(), clip.scales[bone].end(), [](const rig::Vec3 &s) {
                    return std::abs(s.x - 1.0F) > 1.0e-6F || std::abs(s.y - 1.0F) > 1.0e-6F ||
                           std::abs(s.z - 1.0F) > 1.0e-6F;
                });
            if (scaled)
            {
                std::vector<float> scales;
                for (const rig::Vec3 &key : clip.scales[bone])
                    scales.insert(scales.end(), {key.x, key.y, key.z});
                add(bone, "scale", clip.scales[bone].size(), std::move(scales), 3U);
            }
        }
        if (sampler == 0)
            continue;
        animations += std::format(
            R"({}{{"name":{},"channels":[{}],"samplers":[{}],"extras":{{"openminerz":{{"registration":{},"seconds":{},"looping":{},"notice":"The game's own animation, read from your installed CastleMiner Z for local timing reference. Do not ship it in a mod."}}}}}})",
            animations.empty() ? "" : ",", json_string(options.reference_prefix + name), channels,
            samplers, json_string(name),
            json_number(static_cast<float>(rig::seconds_from_ticks(clip.duration_ticks))),
            registration.looping ? "true" : "false");
    }

    // What a script (or a modder) needs to time clips: every registration.
    std::string clips;
    for (const rig::Registration &registration : rig.registrations)
    {
        const rig::Clip &clip = rig.clips[static_cast<std::size_t>(registration.clip)];
        clips += std::format(R"({}{{"name":{},"seconds":{},"looping":{},"bones":{}}})",
                             clips.empty() ? "" : ",", json_string(registration.name),
                             json_number(static_cast<float>(rig::seconds_from_ticks(clip.duration_ticks))),
                             registration.looping ? "true" : "false",
                             std::count(registration.influence.begin(), registration.influence.end(), true));
    }
    const std::string notice =
        options.reference_clips.empty()
            ? "Skeleton read from your installed CastleMiner Z, for authoring OpenMinerZ animation packs. "
              "The proxy body is generated by OpenMinerZ."
            : "Skeleton and the REF_ animations read from your installed CastleMiner Z. The REF_ animations "
              "are the game's own, for local timing reference only: do not ship them in a mod. The proxy "
              "body is generated by OpenMinerZ.";

    std::string json = std::format(
        R"({{"asset":{{"version":"2.0","generator":"OpenMinerZ mod SDK export-rig","copyright":{},)"
        R"("extras":{{"openminerz":{{"format":"OpenMinerZ animation rig 1","generation":{},"game_version":{},)"
        R"json("frame_rate":{},"axes":"+Y up, the character faces +Z, its left hand on +X; one unit is one block (1 m)",)json"
        R"("reference_clips":{},"clips":[{}]}}}}}},"scene":0,"scenes":[{{"name":"OpenMinerZ character","nodes":[{}]}}],)"
        R"("nodes":[{}],"skins":{})",
        json_string(notice), json_string(rig.generation), json_string(rig.game_version),
        clip_frame_rate, options.reference_clips.size(), clips, scene_nodes, nodes, skins);
    if (!meshes.empty())
        json += std::format(R"(,"meshes":{},"materials":{})", meshes, materials);
    if (!animations.empty())
        json += std::format(R"(,"animations":[{}])", animations);
    std::string views;
    for (std::size_t index = 0U; index < buffer.views.size(); ++index)
        views += (index == 0U ? "" : ",") + buffer.views[index];
    std::string accessors;
    for (std::size_t index = 0U; index < buffer.accessors.size(); ++index)
        accessors += (index == 0U ? "" : ",") + buffer.accessors[index];
    while (buffer.bytes.size() % 4U != 0U)
        buffer.bytes.push_back(0U);
    json += std::format(R"(,"buffers":[{{"byteLength":{}}}],"bufferViews":[{}],"accessors":[{}]}})",
                        buffer.bytes.size(), views, accessors);
    return glb(std::move(json), std::move(buffer.bytes));
}

// --- Which clips a file replaces -------------------------------------------------

std::vector<PackTarget> pack_targets(const Model &model, std::string_view file_stem,
                                     const std::function<bool(std::string_view)> &registered,
                                     std::vector<std::string> *warnings)
{
    const auto warn = [&](std::string text) {
        if (warnings != nullptr)
            warnings->push_back(std::move(text));
    };
    const auto reference = [](std::string_view name) {
        return name.starts_with(reference_clip_prefix);
    };
    std::vector<PackTarget> targets;
    const auto taken = [&](std::string_view name) {
        return std::any_of(targets.begin(), targets.end(),
                           [&](const PackTarget &target) { return target.registration == name; });
    };
    // Every animation named after a clip.
    for (std::size_t index = 0U; index < model.animations.size(); ++index)
    {
        const std::string &name = model.animations[index].name;
        if (reference(name) || !registered(name))
            continue;
        if (taken(name))
            warn(std::format("two animations are named \"{}\": the first is used", name));
        else
            targets.push_back({static_cast<int>(index), name});
    }
    // The file's own name picks its only other animation.
    if (registered(file_stem) && !taken(file_stem))
    {
        std::vector<int> others;
        for (std::size_t index = 0U; index < model.animations.size(); ++index)
        {
            const std::string &name = model.animations[index].name;
            if (!reference(name) && !registered(name))
                others.push_back(static_cast<int>(index));
        }
        if (others.size() == 1U)
            targets.push_back({others.front(), std::string{file_stem}});
        else if (others.size() > 1U)
            warn(std::format("{} animations and none is named \"{}\": name the one to use after "
                             "the clip",
                             others.size(), file_stem));
        else if (targets.empty())
            warn(std::format("no animation of its own to replace \"{}\" with", file_stem));
    }
    else if (targets.empty() && !registered(file_stem))
        warn(std::format("\"{}\" is not a clip this character registers, and no animation in the "
                         "file is named after one",
                         file_stem));
    return targets;
}

// --- check-anim ----------------------------------------------------------------------

std::size_t AnimationCheck::count(Level level) const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        messages.begin(), messages.end(), [&](const Message &message) { return message.level == level; }));
}

std::vector<std::string> registered_clip_names(std::span<const AnimationCheckRig> rigs)
{
    std::set<std::string, std::less<>> names;
    bool any_rig = false;
    for (const AnimationCheckRig &entry : rigs)
        if (entry.rig != nullptr)
        {
            any_rig = true;
            for (const rig::Registration &registration : entry.rig->registrations)
                names.insert(registration.name);
        }
    if (!any_rig)
        for (std::size_t index = 1U; index < static_cast<std::size_t>(world::AnimationClip::count); ++index)
        {
            const world::AnimationClipInfo &info =
                world::animation_clip_info(static_cast<world::AnimationClip>(index));
            if (info.classic_ticks > 0 || info.modern_ticks > 0)
                names.insert(std::string{info.name});
        }
    return {names.begin(), names.end()};
}

AnimationCheck check_animation(const Model &model, std::string_view file_stem,
                               std::span<const AnimationCheckRig> rigs)
{
    using Level = AnimationCheck::Level;
    AnimationCheck result;
    const auto say = [&](Level level, std::string text) {
        result.messages.push_back({level, std::move(text)});
    };
    const std::vector<std::string> names = registered_clip_names(rigs);
    const auto plain = [&](std::string_view name) {
        return std::binary_search(names.begin(), names.end(), name, std::less<>{});
    };
    // (GunFX) "PistolReloadEmpty", "PumpShotgunReload@rpg": variants of a clip.
    const auto registered = [&](std::string_view name) {
        if (plain(name))
            return true;
        const gunfx::ClipName split = gunfx::split_clip_name(name);
        return split.base != name && plain(split.base) &&
               (split.model.empty() || gunfx::valid_variant_model(split.model));
    };
    const rig::RetailRig *skeleton = nullptr;
    for (const AnimationCheckRig &entry : rigs)
        if (entry.rig != nullptr)
        {
            skeleton = entry.rig;
            break;
        }
    std::vector<std::string_view> bone_list;
    for (std::size_t bone = 0U; bone < bone_names.size(); ++bone)
        bone_list.push_back(skeleton != nullptr ? std::string_view{skeleton->attachment[bone].name}
                                                : bone_names[bone]);
    const auto bone_named = [&](std::string_view name) -> int {
        if (skeleton != nullptr)
            return find_rig_bone(*skeleton, name);
        const auto found = std::find(bone_names.begin(), bone_names.end(), name);
        return found == bone_names.end() ? -1 : static_cast<int>(found - bone_names.begin());
    };

    if (model.animations.empty())
    {
        say(Level::error, "the file has no animation (in Blender's glTF export, tick Animation)");
        return result;
    }

    // --- Bones.
    BoneNodes nodes;
    nodes.fill(-1);
    std::vector<int> bone_of(model.nodes.size(), -1);
    for (std::size_t node = 0U; node < model.nodes.size(); ++node)
    {
        const int bone = bone_named(model.nodes[node].name);
        if (bone < 0)
            continue;
        if (nodes[static_cast<std::size_t>(bone)] >= 0)
        {
            say(Level::warning,
                std::format("two nodes are named \"{}\": the first is used", model.nodes[node].name));
            continue;
        }
        nodes[static_cast<std::size_t>(bone)] = static_cast<int>(node);
        bone_of[node] = bone;
    }
    std::vector<bool> animated(model.nodes.size());
    for (const ModelAnimation &animation : model.animations)
        for (const AnimationChannel &channel : animation.channels)
            if (channel.node >= 0 && static_cast<std::size_t>(channel.node) < animated.size())
                animated[static_cast<std::size_t>(channel.node)] = true;
    std::size_t animated_bones{};
    std::vector<std::string> part_tracks;
    for (std::size_t node = 0U; node < model.nodes.size(); ++node)
    {
        if (!animated[node])
            continue;
        if (bone_of[node] >= 0)
        {
            ++animated_bones;
            continue;
        }
        // (GunFX) A lowercase name is a weapon part's track (gunfx.parts),
        // unless it is a bone's name written another way ("elbow_left") or
        // another rig's ("lowerarm_l").
        const auto bone_spelling = [&](std::string_view name) {
            const std::string key = folded(name);
            if (!common_alias(key).empty())
                return true;
            for (const std::string_view bone : bone_list)
                if (!numbered(bone) && (folded(bone) == key || (key.size() >= 7U && edit_distance(key, folded(bone)) <= 2U)))
                    return true;
            return false;
        };
        if (gunfx::valid_part_name(model.nodes[node].name) && !bone_spelling(model.nodes[node].name))
        {
            part_tracks.push_back(model.nodes[node].name);
            continue;
        }
        say(Level::error, std::format("\"{}\" is animated but is not a bone of the character{}: its "
                                      "motion is lost",
                                      model.nodes[node].name,
                                      bone_suggestion(model.nodes[node].name, bone_list)));
    }
    if (!part_tracks.empty())
        say(Level::note, std::format("{} {} a part track: it moves the held weapon's part of that name "
                                     "(gunfx.parts), about the part's pivot in the model's own space",
                                     joined(part_tracks), part_tracks.size() == 1U ? "is" : "are each"));
    if (animated_bones == 0U && part_tracks.empty())
        say(Level::error, "no animated node is named after a bone of the character (Root, BackLower, "
                          "ShoulderRight, ...): export-rig writes the names");
    else if (animated_bones == 0U)
        say(Level::note, "only part tracks: the character's own motion stays the game's");

    // --- The Root: upright, unmirrored, one unit per block.
    std::vector<Matrix4> rest;
    evaluate_pose(model, -1, 0.0F, false, rest);
    if (nodes[0] < 0)
    {
        if (animated_bones > 0U)
            say(Level::warning, "no node is named Root: the whole-body motion stays the game's, and "
                                "the file's bones hang from nothing the game knows");
    }
    else
    {
        const Matrix4 &root = rest[static_cast<std::size_t>(nodes[0])];
        const Decomposed parts = decompose(root);
        if (parts.mirrored)
            say(Level::error, "the Root is mirrored (a negative scale): the game would draw every "
                              "limb on the wrong side. Apply the scale in Blender and export again");
        const float largest = std::max({std::abs(parts.scale.x), parts.scale.y, parts.scale.z});
        const float smallest = std::min({std::abs(parts.scale.x), parts.scale.y, parts.scale.z});
        if (largest > 1.02F || smallest < 0.98F)
            say(Level::error,
                std::format("bad scale: the Root is scaled {:.3g}x in the file. Apply the scale (Ctrl+A "
                            "> Scale) and export at scale 1: one unit is one block",
                            largest));
        const Vec3 up = vfx::normalized({root[4], root[5], root[6]});
        if (up.y < 0.7F)
            say(Level::error,
                std::format("the skeleton is not upright: the Root's up axis points ({:.2f}, {:.2f}, "
                            "{:.2f}). Export with +Y Up ticked (glTF is Y-up)",
                            up.x, up.y, up.z));
        else
        {
            const Vec3 forward = vfx::normalized({root[8], root[9], root[10]});
            if (forward.z < -0.7F)
                say(Level::warning, "at rest the character faces -Z: export-rig's skeleton faces +Z, "
                                    "and the game would draw this one turned round");
        }
    }

    // --- Hierarchy and bone lengths against the game's skeleton.
    if (skeleton != nullptr)
    {
        std::vector<float> ratios;
        for (std::size_t bone = 1U; bone < nodes.size(); ++bone)
        {
            const int node = nodes[bone];
            if (node < 0)
                continue;
            const int expected = skeleton->attachment[bone].parent;
            int parent = model.nodes[static_cast<std::size_t>(node)].parent;
            bool shifted = false;
            for (std::size_t guard = 0U; parent >= 0 && bone_of[static_cast<std::size_t>(parent)] < 0 &&
                                         guard < model.nodes.size();
                 ++guard)
            {
                shifted = shifted || !near_identity(compose_node(model.nodes[static_cast<std::size_t>(parent)]),
                                                    1.0e-4F);
                parent = model.nodes[static_cast<std::size_t>(parent)].parent;
            }
            const int found = parent >= 0 ? bone_of[static_cast<std::size_t>(parent)] : -1;
            if (found != expected)
                say(Level::error,
                    std::format("{} hangs from {} in the file but from {} on the game's skeleton: its "
                                "rotation would be read in the wrong frame",
                                bone_list[bone], found < 0 ? std::string_view{"nothing"} : bone_list[static_cast<std::size_t>(found)],
                                bone_list[static_cast<std::size_t>(expected)]));
            else if (shifted)
                say(Level::warning, std::format("a node between {} and {} moves or turns it; the game "
                                                "reads {}'s own transform only",
                                                bone_list[bone], bone_list[static_cast<std::size_t>(expected)],
                                                bone_list[bone]));
            const ModelNode &value = model.nodes[static_cast<std::size_t>(node)];
            const float file_length = vfx::length(value.translation);
            const rig::Matrix &bind = skeleton->attachment[bone].bind;
            const float game_length = std::sqrt(bind[12] * bind[12] + bind[13] * bind[13] + bind[14] * bind[14]);
            if (game_length > 0.02F && file_length > 1.0e-4F)
                ratios.push_back(file_length / game_length);
        }
        if (ratios.size() >= 3U)
        {
            std::nth_element(ratios.begin(), ratios.begin() + static_cast<std::ptrdiff_t>(ratios.size() / 2U),
                             ratios.end());
            const float ratio = ratios[ratios.size() / 2U];
            if (std::abs(ratio - 1.0F) > 0.05F)
                say(Level::error,
                    std::format("bad scale: the file's bones are {:.3g}x as long as the game's (a unit "
                                "scale other than 1 m?). Export at scale 1: one unit is one block",
                                ratio));
        }
    }
    else
    {
        float farthest{};
        for (std::size_t node = 0U; node < model.nodes.size(); ++node)
            if (bone_of[node] > 0)
                farthest = std::max(farthest, vfx::length(model.nodes[node].translation));
        if (farthest > 5.0F)
            say(Level::warning,
                std::format("bones sit up to {:.3g} units from their parents, but the character is 1.4 "
                            "units tall: exported in centimetres? One unit is one block",
                            farthest));
    }

    // --- The animations.
    std::vector<std::string> target_notes;
    const std::vector<PackTarget> targets = pack_targets(model, file_stem, registered, &target_notes);
    for (std::string &note : target_notes)
        say(Level::warning, std::move(note));
    std::size_t used{};
    for (std::size_t index = 0U; index < model.animations.size(); ++index)
    {
        const ModelAnimation &animation = model.animations[index];
        AnimationCheck::Clip clip;
        clip.animation = animation.name;
        clip.seconds = animation.duration;
        std::array<std::array<bool, 3>, rig::attachment_bone_count> writes{};
        // Scale keys of exactly 1 (Blender writes them for every bone) change nothing.
        std::array<bool, rig::attachment_bone_count> resized{};
        for (const AnimationChannel &channel : animation.channels)
        {
            if (channel.node < 0 || static_cast<std::size_t>(channel.node) >= bone_of.size())
                continue;
            const int bone = bone_of[static_cast<std::size_t>(channel.node)];
            if (bone < 0)
                continue;
            writes[static_cast<std::size_t>(bone)]
                  [channel.path == AnimationChannel::Path::translation ? 0U
                   : channel.path == AnimationChannel::Path::rotation  ? 1U
                                                                        : 2U] = true;
            if (channel.path == AnimationChannel::Path::scale)
                resized[static_cast<std::size_t>(bone)] =
                    resized[static_cast<std::size_t>(bone)] ||
                    std::any_of(channel.values.begin(), channel.values.end(),
                                [](float value) { return std::abs(value - 1.0F) > 1.0e-4F; });
        }
        for (std::size_t bone = 0U; bone < writes.size(); ++bone)
        {
            if (writes[bone][1])
                clip.rotated.emplace_back(bone_list[bone]);
            if (writes[bone][0])
                clip.moved.emplace_back(bone_list[bone]);
            if (resized[bone])
                clip.scaled.emplace_back(bone_list[bone]);
        }
        const auto target = std::find_if(targets.begin(), targets.end(), [&](const PackTarget &entry) {
            return entry.animation == static_cast<int>(index);
        });
        if (target != targets.end())
            clip.registration = target->registration;

        if (animation.name.starts_with(reference_clip_prefix))
        {
            say(Level::error,
                std::format("\"{}\" is one of the game's own animations, exported by export-rig for "
                            "timing reference: the game ignores it, and a mod must not ship it",
                            animation.name));
            result.clips.push_back(std::move(clip));
            continue;
        }
        if (clip.registration.empty())
        {
            say(Level::warning,
                std::format("\"{}\" replaces nothing: name it (or the file) after a clip the game "
                            "plays{}. `check-anim --clips` lists them",
                            animation.name, clip_suggestion(animation.name, names)));
            result.clips.push_back(std::move(clip));
            continue;
        }
        ++used;
        std::vector<std::string> moved_joints;
        for (const std::string &bone : clip.moved)
            if (bone != bone_list[0])
                moved_joints.push_back(bone);
        if (!moved_joints.empty())
            say(Level::note, std::format("{}: translation keys on {} are not used; only the Root moves, "
                                         "every other joint keeps the skeleton's length",
                                         animation.name, joined(moved_joints)));
        if (!clip.scaled.empty())
            say(Level::warning, std::format("{}: {} {} scale keys; the game draws them, but no game "
                                            "clip scales a bone (a stray scale?)",
                                            animation.name, joined(clip.scaled),
                                            clip.scaled.size() == 1U ? "has" : "have"));

        // A held rest pose over the game's motion.
        std::size_t rotating{};
        std::vector<std::string> held;
        for (std::size_t bone = 0U; bone < writes.size(); ++bone)
        {
            if (!writes[bone][1] || nodes[bone] < 0)
                continue;
            ++rotating;
            const auto node = static_cast<std::size_t>(nodes[bone]);
            const Quat rest_rotation = quat_normalized(model.nodes[node].rotation);
            bool still = true;
            for (const AnimationChannel &channel : animation.channels)
                if (channel.node == static_cast<int>(node) && channel.path == AnimationChannel::Path::rotation)
                {
                    const std::size_t stride =
                        channel.interpolation == AnimationChannel::Interpolation::cubic ? 12U : 4U;
                    const std::size_t offset =
                        channel.interpolation == AnimationChannel::Interpolation::cubic ? 4U : 0U;
                    for (std::size_t at = offset; at + 4U <= channel.values.size() && still; at += stride)
                        still = quat_angle(quat_normalized({channel.values[at], channel.values[at + 1U],
                                                            channel.values[at + 2U], channel.values[at + 3U]}),
                                           rest_rotation) < 0.003F;
                }
            if (still)
                held.emplace_back(bone_list[bone]);
        }
        if (held.size() >= 8U && held.size() * 2U >= rotating)
            say(Level::warning,
                std::format("{}: {} of its {} rotating bones hold the rest pose (the T-pose) for the "
                            "whole clip ({}). They will hold it over the game's own motion. Blender "
                            "writes these with \"Force keeping channels for bones\" ticked; untick it so "
                            "bones you did not key keep the game's motion",
                            animation.name, held.size(), rotating, joined(held, 6U)));

        if (skeleton == nullptr)
        {
            // Timing from the registration table both generations share.
            const world::AnimationClipInfo &info = world::animation_clip_info(
                world::animation_clip_from_name(gunfx::split_clip_name(clip.registration).base));
            for (const auto &[label, ticks] :
                 {std::pair{"Classic 1.6.3.8", info.classic_ticks}, std::pair{"Modern 1.9.9.8", info.modern_ticks}})
            {
                if (ticks <= 0)
                    clip.timing.push_back(std::format("{}: not registered there, so not used", label));
                else
                    clip.timing.push_back(timing_text(label, clip.registration, rig::seconds_from_ticks(ticks),
                                                      info.looping, animation.duration));
            }
            result.clips.push_back(std::move(clip));
            continue;
        }

        for (const AnimationCheckRig &entry : rigs)
        {
            if (entry.rig == nullptr)
                continue;
            // (GunFX) A variant plays as long as its base clip.
            const rig::Registration *registration = entry.rig->find(clip.registration);
            if (registration == nullptr)
                registration = entry.rig->find(gunfx::split_clip_name(clip.registration).base);
            if (registration == nullptr)
            {
                clip.timing.push_back(
                    std::format("{}: does not register {}, so it is not used there", entry.label, clip.registration));
                continue;
            }
            const rig::Clip &retail = entry.rig->clips[static_cast<std::size_t>(registration->clip)];
            const double game = rig::seconds_from_ticks(retail.duration_ticks);
            if (gunfx::reload_clip(clip.registration))
            {
                // (GunFX) Reloads are stretched onto the game's clip.
                clip.timing.push_back(std::format("{}: {} lasts {:.3f} s; this {:.3f} s clip is stretched to fit, "
                                                  "and the game plays it over the gun's reload time",
                                                  entry.label, clip.registration, game, animation.duration));
            }
            else
                clip.timing.push_back(
                    timing_text(entry.label, clip.registration, game, registration->looping, animation.duration));
            if (!gunfx::reload_clip(clip.registration) && animation.duration > 0.0F &&
                std::abs(animation.duration - game) > 2.0 / clip_frame_rate)
                say(Level::warning,
                    std::format("{}: lasts {:.3f} s but {} plays {} for {:.3f} s, so {}. Make it {} frames "
                                "long at 30 fps",
                                animation.name, animation.duration, entry.label, clip.registration, game,
                                animation.duration < game
                                    ? (registration->looping ? "it stalls on its last pose before every loop"
                                                             : "it holds its last pose for the rest")
                                    : (registration->looping ? "every loop restarts before its end"
                                                             : "its end is never seen"),
                                static_cast<int>(std::lround(game * clip_frame_rate))));
            if (clip.ignored.empty())
            {
                for (std::size_t bone = 0U; bone < writes.size(); ++bone)
                    if ((writes[bone][0] || writes[bone][1] || writes[bone][2]) && !registration->influence[bone])
                        clip.ignored.emplace_back(bone_list[bone]);
                if (!clip.ignored.empty())
                    say(Level::note,
                        std::format("{}: {} only moves {} and the bones below; {} {} not used",
                                    animation.name, clip.registration,
                                    joined(influence_roots(*entry.rig, registration->influence)),
                                    joined(clip.ignored), clip.ignored.size() == 1U ? "is" : "are"));
            }
        }

        // The game's own animation, renamed or not.
        if (animation.duration > 0.0F)
        {
            const rig::RetailRig &game = *skeleton;
            const BoneNodes matched = match_bones(model, game);
            const int frames =
                std::min(1800, static_cast<int>(std::floor(animation.duration * clip_frame_rate + 1.0e-3)) + 1);
            std::vector<rig::BonePose> poses(static_cast<std::size_t>(frames));
            for (int frame = 0; frame < frames; ++frame)
                sample_clip_pose(model, static_cast<int>(index), static_cast<float>(frame) / clip_frame_rate,
                                 game, matched, poses[static_cast<std::size_t>(frame)]);
            for (const rig::Clip &retail : game.clips)
            {
                std::size_t compared{};
                bool same = true;
                for (std::size_t bone = 0U; bone < writes.size() && same; ++bone)
                {
                    if (!writes[bone][1] || retail.rotations[bone].size() < 2U)
                        continue;
                    const std::size_t keys = std::min(retail.rotations[bone].size(), poses.size());
                    if (keys < 2U)
                        continue;
                    for (std::size_t key = 0U; key < keys && same; ++key)
                    {
                        const rig::Quat &a = retail.rotations[bone][key];
                        const rig::Quat &b = poses[key][bone].rotation;
                        same = quat_angle({a.x, a.y, a.z, a.w}, {b.x, b.y, b.z, b.w}) < 0.01F;
                    }
                    ++compared;
                }
                if (same && compared >= 4U)
                {
                    say(Level::error,
                        std::format("{}: this is the game's own \"{}\" animation ({} bones match it to within "
                                    "0.6 degrees). A mod must not ship the game's animations: make your own",
                                    animation.name, retail.asset, compared));
                    break;
                }
            }
        }
        result.clips.push_back(std::move(clip));
    }
    if (used == 0U)
        say(Level::error, std::format("nothing in this file replaces a clip: name the file or an animation "
                                      "after one the game plays (Stand, Walk, PickUse, GunReload, ...)"));
    return result;
}

} // namespace openminerz::modding::graphics
