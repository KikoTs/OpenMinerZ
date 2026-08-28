#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace openminerz
{

using StaticModelMatrix = std::array<float, 16>;

struct StaticModelVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> color{255U, 255U, 255U, 255U};
};

struct StaticModelPart final
{
    StaticModelMatrix transform{};
    std::uint8_t material_flags{};
    std::array<float, 3> diffuse{1.0F, 1.0F, 1.0F};
    SDL_Texture *texture{};
    std::vector<StaticModelVertex> vertices;
    std::vector<std::uint32_t> indices;
};

class StaticModelAsset final
{
  public:
    StaticModelAsset(SDL_Renderer *renderer, const std::filesystem::path &path);
    ~StaticModelAsset();

    StaticModelAsset(const StaticModelAsset &) = delete;
    StaticModelAsset &operator=(const StaticModelAsset &) = delete;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const std::vector<StaticModelPart> &parts() const noexcept { return parts_; }
    [[nodiscard]] std::array<float, 3> center() const noexcept { return center_; }
    [[nodiscard]] float radius() const noexcept { return radius_; }

  private:
    std::vector<StaticModelPart> parts_;
    std::array<float, 3> center_{};
    float radius_{1.0F};
    bool loaded_{};
};

[[nodiscard]] std::filesystem::path original_derived_data_path();

} // namespace openminerz
