// Member functions of AvatarViewmodel::Impl: Xbox avatars as skins on the retail
// character. Included inside the class body by avatar_viewmodel.cpp, after
// retail_character_view.inc.
//
// A skin replaces the SWATMale mesh and nothing else. The pose is the same
// rig::CharacterPose the stock body is skinned from (so every clip, blend and
// the hidden first-person head apply as they are), the held item is the same
// entity under the same hand transform, and both views light and composite the
// result through the same two passes. See openminerz/xbox_avatar.hpp for how an
// avatar's own skeleton is placed on the rig.
//
// GPU resources are bounded and never built in one go: a decoded skin arrives
// from a worker thread with its textures as plain RGBA, pump_skin_uploads()
// creates and fills the textures a band at a time within a per-frame texel
// budget, and the character switches to the new skin only when the last band is
// in - until then it keeps what it was wearing (the stock body, or its previous
// avatar). A replaced or departed skin's textures are destroyed a few frames
// later, once no submitted draw can still name them.

// The local player in the per-player tables (remote players use their gamer id).
static constexpr std::uint16_t local_player_gid = 256U;

struct SkinInstance final
{
    std::shared_ptr<const xbox_avatar::Skin> skin;
    std::vector<SDL_Texture *> textures; // parallel to skin->textures
    std::size_t next_texture{};
    int next_row{};

    [[nodiscard]] bool ready() const noexcept { return skin != nullptr && next_texture >= skin->textures.size(); }
};

struct RetiredTexture final
{
    SDL_Texture *texture{};
    int frames{};
};

// Texels uploaded per frame, across every pending skin: one megabyte, well
// under a millisecond of UpdateSubresource.
static constexpr std::size_t skin_upload_texels_per_frame = 256U * 1024U;
// Frames a retired texture outlives its skin. The entity batch that named it
// was submitted at most one frame ago.
static constexpr int skin_texture_retire_frames = 3;

void retire_skin(SkinInstance &instance)
{
    for (SDL_Texture *texture : instance.textures)
        if (texture != nullptr)
            retired_skin_textures_.push_back({texture, skin_texture_retire_frames});
    instance = {};
}

void destroy_skin_texture(SDL_Texture *texture) noexcept
{
    if (texture == nullptr)
        return;
    // Both passes cache a shader view per SDL texture; a freed texture's
    // address may be handed to the next one created.
    if (skin_texture_release_hook_)
        skin_texture_release_hook_(texture);
#if defined(_WIN32)
    depth_srv_cache_.erase(texture);
#endif
    SDL_DestroyTexture(texture);
}

void set_player_skin(std::uint16_t gid, std::shared_ptr<const xbox_avatar::Skin> skin)
{
    const auto active = active_skins_.find(gid);
    const auto pending = pending_skins_.find(gid);
    if (skin == nullptr)
    {
        if (active != active_skins_.end())
        {
            retire_skin(active->second);
            active_skins_.erase(active);
        }
        if (pending != pending_skins_.end())
        {
            retire_skin(pending->second);
            pending_skins_.erase(pending);
        }
        return;
    }
    // Already worn, or already on its way.
    if (pending != pending_skins_.end())
    {
        if (pending->second.skin == skin)
            return;
        retire_skin(pending->second);
        pending_skins_.erase(pending);
    }
    if (active != active_skins_.end() && active->second.skin == skin)
        return;
    SkinInstance instance;
    instance.skin = std::move(skin);
    instance.textures.assign(instance.skin->textures.size(), nullptr);
    pending_skins_.emplace(gid, std::move(instance));
}

[[nodiscard]] bool player_skin_applied(std::uint16_t gid) const noexcept
{
    return active_skins_.contains(gid);
}

// Once per frame, before anything is drawn. `everything` finishes every pending
// upload in this one call (deterministic captures); it is never used in play.
void pump_skin_uploads(bool everything = false)
{
    for (auto retired = retired_skin_textures_.begin();
         !everything && retired != retired_skin_textures_.end();)
    {
        if (--retired->frames > 0)
        {
            ++retired;
            continue;
        }
        destroy_skin_texture(retired->texture);
        retired = retired_skin_textures_.erase(retired);
    }
    std::size_t budget =
        everything ? std::numeric_limits<std::size_t>::max() : skin_upload_texels_per_frame;
    for (auto pending = pending_skins_.begin(); pending != pending_skins_.end();)
    {
        SkinInstance &instance = pending->second;
        bool failed = false;
        while (!instance.ready() && budget > 0U && !failed)
        {
            const xbox_avatar::Image &image = instance.skin->textures[instance.next_texture];
            SDL_Texture *&texture = instance.textures[instance.next_texture];
            if (texture == nullptr)
            {
                texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                            image.width, image.height);
                if (texture == nullptr)
                {
                    failed = true;
                    break;
                }
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
                instance.next_row = 0;
            }
            const std::size_t width = static_cast<std::size_t>(image.width);
            // Counted in size_t: an unlimited budget does not fit an int.
            const int rows = static_cast<int>(
                std::min(static_cast<std::size_t>(image.height - instance.next_row),
                         std::max<std::size_t>(1U, budget / width)));
            const SDL_Rect band{0, instance.next_row, image.width, rows};
            if (!SDL_UpdateTexture(texture, &band,
                                   image.rgba.data() + static_cast<std::size_t>(instance.next_row) * width * 4U,
                                   image.width * 4))
            {
                failed = true;
                break;
            }
            budget -= std::min(budget, width * static_cast<std::size_t>(rows));
            instance.next_row += rows;
            if (instance.next_row >= image.height)
            {
                ++instance.next_texture;
                instance.next_row = 0;
            }
        }
        if (failed)
        {
            // Out of video memory, or a lost device: this player keeps the
            // stock body (or the avatar already worn) rather than half a skin.
            std::cerr << "Xbox avatar: could not create a texture for player " << pending->first
                      << "; keeping the previous character\n";
            retire_skin(instance);
            pending = pending_skins_.erase(pending);
            continue;
        }
        if (!instance.ready())
        {
            ++pending;
            continue;
        }
        // Complete: switch in place.
        SkinInstance &active = active_skins_[pending->first];
        retire_skin(active);
        active = std::move(instance);
        pending = pending_skins_.erase(pending);
    }
}

[[nodiscard]] const SkinInstance *skin_for(std::uint16_t gid) const noexcept
{
    const auto found = active_skins_.find(gid);
    return found == active_skins_.end() ? nullptr : &found->second;
}

// The avatar's surfaces for this pose, appended to retail_vertices_ /
// retail_spans_ in the avatar's own draw order.
void append_skin_geometry(const SkinInstance &instance, const rig::CharacterPose &pose,
                          xbox_avatar::Placement placement)
{
    const xbox_avatar::Skin &skin = *instance.skin;
    xbox_avatar::pose(skin, pose, placement, skin_pose_);
    // From the character's own eyes the head is scaled to a point; what hangs
    // from it alone collapses with it and need not be skinned at all.
    const bool from_own_eyes = placement == xbox_avatar::Placement::stock_joints;
    if (skin_surfaces_.size() < skin.geometry.size())
        skin_surfaces_.resize(skin.geometry.size());
    skin_surface_posed_.assign(skin.geometry.size(), false);
    for (const xbox_avatar::Layer &layer : skin.layers)
    {
        const xbox_avatar::Geometry &geometry = skin.geometry[layer.geometry];
        if (from_own_eyes && geometry.head_only)
            continue;
        xbox_avatar::SkinnedGeometry &surface = skin_surfaces_[layer.geometry];
        if (!skin_surface_posed_[layer.geometry])
        {
            xbox_avatar::skin_geometry(geometry, skin_pose_.palette, surface);
            // The build scales a normal with its bone; lighting wants a direction.
            for (std::array<float, 3> &normal : surface.normal)
            {
                const float length =
                    std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                if (length > 0.000001F)
                    for (float &component : normal)
                        component /= length;
            }
            skin_surface_posed_[layer.geometry] = true;
        }
        RetailSpan span;
        SDL_Texture *texture = layer.texture >= 0 ? instance.textures[static_cast<std::size_t>(layer.texture)] : nullptr;
        span.texture = texture != nullptr ? texture : white_texture_;
        span.alpha = layer.blended;
        span.clamp = layer.clamp;
        span.first = static_cast<std::uint32_t>(retail_vertices_.size());
        for (const std::uint16_t index : layer.indices)
        {
            RetailVertex vertex;
            vertex.position = surface.position[index];
            vertex.normal = surface.normal[index];
            vertex.uv = layer.uv[index];
            vertex.color = layer.color[index];
            retail_vertices_.push_back(vertex);
        }
        span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
        if (span.count > 0U)
            retail_spans_.push_back(span);
    }
}

void destroy_player_skins() noexcept
{
    for (auto &[gid, instance] : active_skins_)
        for (SDL_Texture *texture : instance.textures)
            destroy_skin_texture(texture);
    for (auto &[gid, instance] : pending_skins_)
        for (SDL_Texture *texture : instance.textures)
            destroy_skin_texture(texture);
    for (const RetiredTexture &retired : retired_skin_textures_)
        destroy_skin_texture(retired.texture);
    active_skins_.clear();
    pending_skins_.clear();
    retired_skin_textures_.clear();
}

std::unordered_map<std::uint16_t, SkinInstance> active_skins_;
std::unordered_map<std::uint16_t, SkinInstance> pending_skins_;
std::vector<RetiredTexture> retired_skin_textures_;
std::function<void(SDL_Texture *)> skin_texture_release_hook_;
xbox_avatar::Pose skin_pose_;
std::vector<xbox_avatar::SkinnedGeometry> skin_surfaces_;
std::vector<bool> skin_surface_posed_;
