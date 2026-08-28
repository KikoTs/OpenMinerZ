#include "static_model_asset.hpp"

#include "openminerz/asset_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace openminerz
{
namespace
{

class Reader final
{
  public:
    explicit Reader(const std::filesystem::path &path)
        : stream_(path, std::ios::binary), remaining_(std::filesystem::file_size(path))
    {
        if (!stream_)
            throw std::runtime_error{"could not open static model"};
    }

    template <typename Value> Value read()
    {
        Value value{};
        bytes(&value, sizeof(value));
        return value;
    }

    void bytes(void *destination, std::size_t count)
    {
        if (count > remaining_)
            throw std::runtime_error{"truncated static model"};
        stream_.read(static_cast<char *>(destination), static_cast<std::streamsize>(count));
        if (!stream_)
            throw std::runtime_error{"failed reading static model"};
        remaining_ -= count;
    }

  private:
    std::ifstream stream_;
    std::size_t remaining_{};
};

std::array<float, 3> transform_point(const std::array<float, 3> &point,
                                     const StaticModelMatrix &matrix) noexcept
{
    return {point[0] * matrix[0] + point[1] * matrix[4] + point[2] * matrix[8] + matrix[12],
            point[0] * matrix[1] + point[1] * matrix[5] + point[2] * matrix[9] + matrix[13],
            point[0] * matrix[2] + point[1] * matrix[6] + point[2] * matrix[10] + matrix[14]};
}

SDL_Texture *texture_from_rgba(SDL_Renderer *renderer, int width, int height,
                               std::vector<std::uint8_t> &rgba)
{
    if (width <= 0 || height <= 0 || rgba.empty())
        return nullptr;
    // ModelEntity/GunEntity/PickModelEntity all use XNA's opaque pass. DXT edge texels and
    // authored atlas padding can still decode with partial alpha; baking them into an SDL
    // texture otherwise punches translucent seams through the pick head and weapon bodies.
    for (std::size_t alpha = 3U; alpha < rgba.size(); alpha += 4U)
        rgba[alpha] = 255U;
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, rgba.data(),
                                                 width * 4);
    if (surface == nullptr)
        return nullptr;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture != nullptr)
    {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        // The source ModelMesh parts are drawn with XNA's opaque blend state. Some authored
        // RGB texels carry zero alpha outside their atlas islands; blending those here creates
        // false slits through handles and other low-poly props.
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    }
    return texture;
}

} // namespace

StaticModelAsset::StaticModelAsset(SDL_Renderer *renderer, const std::filesystem::path &path)
{
    try
    {
        Reader reader{path};
        if (reader.read<std::uint32_t>() != 0x4D534D4FU)
            throw std::runtime_error{"unsupported static model"};
        const int version = reader.read<std::int32_t>();
        if (version < 1 || version > 3)
            throw std::runtime_error{"unsupported static model"};
        const int part_count = reader.read<std::int32_t>();
        if (part_count <= 0 || part_count > 128)
            throw std::runtime_error{"invalid static model part count"};
        parts_.reserve(static_cast<std::size_t>(part_count));
        std::array<float, 3> minimum{
            std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
        std::array<float, 3> maximum{
            std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};
        for (int part_index = 0; part_index < part_count; ++part_index)
        {
            StaticModelPart part;
            for (float &value : part.transform)
                value = reader.read<float>();
            if (version >= 3)
                part.material_flags = reader.read<std::uint8_t>();
            for (float &value : part.diffuse)
                value = reader.read<float>();
            const int texture_width = reader.read<std::int32_t>();
            const int texture_height = reader.read<std::int32_t>();
            if (texture_width < 0 || texture_height < 0 || texture_width > 8192 ||
                texture_height > 8192)
                throw std::runtime_error{"invalid static model texture"};
            const std::size_t texture_bytes = static_cast<std::size_t>(texture_width) *
                                              static_cast<std::size_t>(texture_height) * 4U;
            std::vector<std::uint8_t> rgba(texture_bytes);
            reader.bytes(rgba.data(), rgba.size());
            part.texture = texture_from_rgba(renderer, texture_width, texture_height, rgba);

            const int vertex_count = reader.read<std::int32_t>();
            if (vertex_count <= 0 || vertex_count > 1000000)
                throw std::runtime_error{"invalid static model vertices"};
            part.vertices.resize(static_cast<std::size_t>(vertex_count));
            for (auto &vertex : part.vertices)
            {
                for (float &value : vertex.position)
                    value = reader.read<float>();
                for (float &value : vertex.normal)
                    value = reader.read<float>();
                for (float &value : vertex.uv)
                    value = reader.read<float>();
                if (version >= 2)
                    reader.bytes(vertex.color.data(), vertex.color.size());
                const auto transformed = transform_point(vertex.position, part.transform);
                for (std::size_t component = 0; component < 3U; ++component)
                {
                    minimum[component] = std::min(minimum[component], transformed[component]);
                    maximum[component] = std::max(maximum[component], transformed[component]);
                }
            }
            const int index_count = reader.read<std::int32_t>();
            if (index_count <= 0 || index_count > 3000000 || index_count % 3 != 0)
                throw std::runtime_error{"invalid static model indices"};
            part.indices.resize(static_cast<std::size_t>(index_count));
            for (std::uint32_t &index : part.indices)
            {
                index = reader.read<std::uint32_t>();
                if (index >= part.vertices.size())
                    throw std::runtime_error{"invalid static model triangle"};
            }
            parts_.push_back(std::move(part));
        }
        for (std::size_t component = 0; component < 3U; ++component)
            center_[component] = (minimum[component] + maximum[component]) * 0.5F;
        float radius_squared{};
        for (const auto &part : parts_)
            for (const auto &vertex : part.vertices)
            {
                const auto point = transform_point(vertex.position, part.transform);
                const float dx = point[0] - center_[0];
                const float dy = point[1] - center_[1];
                const float dz = point[2] - center_[2];
                radius_squared = std::max(radius_squared, dx * dx + dy * dy + dz * dz);
            }
        radius_ = std::sqrt(radius_squared);
        loaded_ = !parts_.empty() && radius_ > 0.000001F;
    }
    catch (const std::exception &)
    {
        for (auto &part : parts_)
            SDL_DestroyTexture(part.texture);
        parts_.clear();
        loaded_ = false;
    }
}

StaticModelAsset::~StaticModelAsset()
{
    for (auto &part : parts_)
        SDL_DestroyTexture(part.texture);
}

std::filesystem::path original_derived_data_path()
{
    return active_derived_data_path();
}

} // namespace openminerz
