// Local authoring evidence, read through the SAME rig/avatar decoders as the game.
// No owned meshes are distributed with the SDK or animation pack.
#include "rig_lab.hpp"
#include "openminerz/modding/animation_authoring.hpp"
#include "openminerz/xbox_avatar.hpp"
#include "openminerz/inventory.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <type_traits>

namespace openminerz::mod_sdk
{
namespace
{
namespace fs = std::filesystem;
namespace graphics = modding::graphics;
template<class T> void json(std::ostream &out, const T &value)
{
    if constexpr (std::is_arithmetic_v<T>) out << +value;
    else
    {
        out << '[';
        bool first = true;
        for (const auto &part : value) { if (!first) out << ','; first = false; json(out, part); }
        out << ']';
    }
}
void vector(std::ostream &out, const rig::Vec3 &v) { json(out, std::array{v.x,v.y,v.z}); }
void texture(std::ostream &out, const fs::path &folder, int id, int width, int height,
             const std::vector<std::uint8_t> &rgba)
{
    const std::string name = "texture-" + std::to_string(id) + ".rgba";
    std::ofstream file(folder / name, std::ios::binary);
    file.write(reinterpret_cast<const char *>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    if (!file) throw std::runtime_error("Cannot write rig-lab texture");
    out << "{\"url\":" << std::quoted(name) << ",\"width\":" << width << ",\"height\":" << height << '}';
}
}

int command_export_rig_lab(std::vector<std::string> args)
{
    fs::path rig_path, avatar_path, output;
    for (std::size_t i=0; i<args.size(); ++i)
    {
        if (i+1 >= args.size()) throw std::runtime_error("export-rig-lab needs --rig FILE --out FOLDER [--avatar FILE]");
        const std::string key = args[i++];
        if (key == "--rig") rig_path = args[i];
        else if (key == "--avatar") avatar_path = args[i];
        else if (key == "--out") output = args[i];
        else throw std::runtime_error("Unknown export-rig-lab argument: " + key);
    }
    if (rig_path.empty() || output.empty()) throw std::runtime_error("--rig and --out are required");
    fs::create_directories(output);
    rig::RetailRig rig{rig_path};
    const auto skeleton = graphics::authoring_skeleton(rig);
    std::shared_ptr<const xbox_avatar::Skin> avatar;
    if (!avatar_path.empty())
    {
        std::ifstream input(avatar_path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open avatar");
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
        std::string error;
        xbox_avatar::Limits limits;
        limits.file_bytes = xbox_avatar::local_file_bytes;
        avatar = xbox_avatar::load(bytes, [](auto png, xbox_avatar::Image &result) {
            auto decoded = graphics::decode_image(png);
            if (!decoded) return false;
            result = {decoded->width, decoded->height, std::move(decoded->rgba)};
            return true;
        }, error, limits);
        if (!avatar) throw std::runtime_error(error);
    }
    std::ofstream out(output / "rig.json", std::ios::binary);
    out << std::setprecision(9) << "{\"schema\":1,\"bones\":[";
    for (std::size_t i=0; i<skeleton.size(); ++i)
    {
        const auto &bone=skeleton[i];
        if (i) out << ',';
        out << "{\"name\":" << std::quoted(bone.name) << ",\"native\":" << std::quoted(rig.attachment[i].native_name)
            << ",\"parent\":" << bone.parent << ",\"position\":";
        json(out, std::array{bone.translation.x,bone.translation.y,bone.translation.z});
        out << '}';
    }
    // Column-vector equivalent of root * root_adjust * authoring_from_avatar.
    const auto root_base=rig::multiply(rig::multiply(rig::invert(graphics::authoring_frame(rig)),rig.root_adjust),
                                       graphics::authoring_from_avatar());
    out << "],\"bodyRoot\":"; json(out,root_base);
    out << ",\"bodyOrder\":"; json(out,rig.body_order);
    out << ",\"body\":[";
    for (std::size_t i=0; i<rig.body.size(); ++i)
    {
        const auto &bone=rig.body[i]; if(i) out << ',';
        out << "{\"source\":" << bone.source << ",\"parent\":" << bone.parent << ",\"position\":";
        json(out,std::array{bone.bind_local[12],bone.bind_local[13],bone.bind_local[14]});
        out << ",\"inverseBind\":"; json(out,bone.inverse_bind); out << '}';
    }
    out << "],\"avatarBones\":[";
    if (avatar) for (std::size_t i=0; i<avatar->bones.size(); ++i)
    {
        const auto &bone=avatar->bones[i]; if(i) out << ',';
        out << "{\"position\":"; vector(out,bone.translation);
        out << ",\"scale\":"; vector(out,bone.scale);
        out << ",\"cumulativeScale\":"; vector(out,bone.cumulative_scale);
        out << ",\"inverseBind\":"; json(out,bone.inverse_bind); out << '}';
    }
    out << "],\"textures\":[";
    int texture_id=0;
    for (const auto &part:rig.parts)
    {
        if(texture_id) out << ',';
        texture(out,output,texture_id++,part.texture_width,part.texture_height,part.rgba);
    }
    if(avatar) for(const auto &image:avatar->textures)
    {
        if(texture_id) out << ',';
        texture(out,output,texture_id++,image.width,image.height,image.rgba);
    }
    out << "],\"surfaces\":[";
    bool first=true;
    for (std::size_t i=0;i<rig.parts.size();++i)
    {
        const auto &part=rig.parts[i]; if(!first) out << ','; first=false;
        out << "{\"skin\":\"stock\",\"texture\":" << i << ",\"position\":[";
        for(std::size_t j=0;j<part.vertices.size();++j) {if(j)out<<',';json(out,part.vertices[j].position);}
        out << "],\"normal\":[";
        for(std::size_t j=0;j<part.vertices.size();++j) {if(j)out<<',';json(out,part.vertices[j].normal);}
        out << "],\"uv\":[";
        for(std::size_t j=0;j<part.vertices.size();++j) {if(j)out<<',';json(out,part.vertices[j].uv);}
        out << "],\"joints\":[";
        for(std::size_t j=0;j<part.vertices.size();++j) {if(j)out<<',';json(out,part.vertices[j].bones);}
        out << "],\"weights\":[";
        for(std::size_t j=0;j<part.vertices.size();++j) {if(j)out<<',';json(out,part.vertices[j].weights);}
        out << "],\"indices\":";json(out,part.indices);out << '}';
    }
    if(avatar) for(const auto &layer:avatar->layers)
    {
        if(!first)out<<','; first=false;
        const auto &geometry=avatar->geometry[layer.geometry];
        out << "{\"skin\":\"avatar\",\"name\":" << std::quoted(layer.name)
            << ",\"texture\":" << (layer.texture<0?-1:layer.texture+static_cast<int>(rig.parts.size()))
            << ",\"blended\":" << (layer.blended?"true":"false") << ",\"position\":";
        json(out,geometry.position);out << ",\"normal\":";json(out,geometry.normal);
        out << ",\"uv\":";json(out,layer.uv);out << ",\"color\":";json(out,layer.color);
        out << ",\"joints\":";json(out,geometry.bones);out << ",\"weights\":";json(out,geometry.weights);
        out << ",\"indices\":";json(out,layer.indices);out << '}';
    }
    out << "],\"masks\":{";
    first=true;
    for(const auto &registration:rig.registrations)
    {
        if(!first)out<<',';first=false;
        out << std::quoted(registration.name) << ':';json(out,registration.influence);
    }
    out << "},\"idleRotation\":[";
    rig::CharacterPose idle{rig};
    rig::ChannelStates idle_channels{};
    idle_channels[0].current=rig.find("Stand");idle_channels[0].blend=1;
    idle_channels[2].current=rig.find("GenericIdle");idle_channels[2].blend=1;
    idle.apply(idle_channels);
    for(std::size_t b=0;b<skeleton.size();++b)
    {
        if(b)out<<',';const auto &q=idle.skeleton()[b].rotation;json(out,std::array{q.x,q.y,q.z,q.w});
    }
    out << "],\"items\":[";
    first=true;
    for(const auto &item:rig.items)
    {
        if(!first)out<<',';first=false;
        out << "{\"id\":" << item.id << ",\"name\":" << std::quoted(item.name)
            << ",\"hand\":";json(out,item.hand);
        const auto *stats=gun_stats(static_cast<InventoryItemId>(item.id));
        out << ",\"reloadSeconds\":" << (stats?stats->reload_seconds:0.0F) << '}';
    }
    // Independent reference matrices for the browser's skinning parity check.
    out << "],\"parity\":[";
    for(int sample=0;sample<3;++sample)
    {
        rig::Clip clip;clip.duration_ticks=1;
        rig::Registration registration;registration.clip=static_cast<int>(rig.clips.size());registration.influence.fill(true);
        for(std::size_t b=0;b<skeleton.size();++b)
        {
            const float angle=sample==0?0.0F:static_cast<float>((static_cast<int>(b)*17+sample*31)%100-50)*0.01F;
            clip.rotations[b].push_back({0,0,std::sin(angle/2),std::cos(angle/2)});
            clip.positions[b].push_back(b==0 ? rig::Vec3{} : rig::Vec3{
                rig.attachment[b].bind[12],rig.attachment[b].bind[13],rig.attachment[b].bind[14]});
            clip.scales[b].push_back({1,1,1});
        }
        rig.clips.push_back(clip);
        rig::CharacterPose character{rig};rig::ChannelStates channels{};
        channels[0].current=&registration;channels[0].blend=1;
        character.apply(channels);
        if(sample)out<<',';
        out << "{\"rotation\":[";
        for(std::size_t b=0;b<skeleton.size();++b)
        {
            if(b)out<<',';const auto &q=clip.rotations[b][0];json(out,std::array{q.x,q.y,q.z,q.w});
        }
        out << "],\"body\":[";
        for(std::size_t b=0;b<character.body_world().size();++b)
        {if(b)out<<',';json(out,rig::multiply(character.body_world()[b],graphics::authoring_from_avatar()));}
        out << "],\"avatar\":[";
        if(avatar)
        {
            xbox_avatar::Pose pose;xbox_avatar::pose(*avatar,character,xbox_avatar::Placement::stock_joints,pose);
            for(std::size_t b=0;b<pose.world.size();++b)
            {if(b)out<<',';json(out,rig::multiply(pose.world[b],graphics::authoring_from_avatar()));}
        }
        out << "]}";
        rig.clips.pop_back();
    }
    out << "]}\n";
    if(!out)throw std::runtime_error("Cannot write rig-lab scene");
    std::cout << "Rig Lab: " << skeleton.size() << " mapped bones, stock skin" << (avatar?" and local avatar":"") << " exported to " << output << '\n';
    return 0;
}
}
