// Member functions of AvatarViewmodel::Impl: the retail character in both views.
// Included inside the class body by avatar_viewmodel.cpp.
//
// The game draws ONE thing for a player: the SWATMale body skinned by the
// Avatar's animated pose, with the held item's entity parented to PropRight.
// Third person places it at the player's feet and heading. First person renders
// the very same body from Avatar.EyePointCamera (the head, 90 degrees vertical,
// near plane 0.01) with the head bone collapsed (Avatar.HideHead) - there is no
// separate "arms" model. build_retail_geometry() therefore produces the body and
// the item once, in avatar space, and each view only transforms and lights it.
//
// Nothing here is retargeted: the pose comes from rig::CharacterPose (the port of
// the original engine, verified bone-for-bone by openminerz_original_rig_parity)
// and the item transform is the original CreateEntity(ItemUse.Hand) matrix
// imported into the content pack.

struct RetailVertex final
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct RetailSpan final
{
    SDL_Texture *texture{};
    std::uint32_t first{};
    std::uint32_t count{};
    // Viewmodel material response: negative = matte cloth/skin, positive = the
    // held item's specular lobe (see the viewmodel pixel shader).
    float material{-0.36F};
    bool item{};
    bool torch{};
    // Xbox-avatar skins only (xbox_avatar_view.inc): a layer painted over the
    // surface beneath it, and a texture addressed with clamping.
    bool alpha{};
    bool clamp{};
    // (Items) The colour is the vertices' own, not a texture's (a mod item):
    // first person in Enhanced decodes it to linear light as it does texels.
    bool vertex_albedo{};
};

struct RetailHeldItem final
{
    std::uint16_t item{};
    bool local{};
    world::BlockType block{world::BlockType::empty};
    // Instruments turn their dial from live state (first person only).
    bool live_instruments{};
    float yaw{};
    world::Vec3 position{};
    world::Vec3 waypoint{};
    float clock_time_of_day{};
    // Modding, visuals area: a mod item drawn in place of the fallback's
    // model (set_held_mod_item); null for every retail item. (Items) Its
    // glTF model or icon, grip and clips (openminerz/modding/held_item.hpp).
    const modding::graphics::HeldItemPose *mod{};
};

struct RetailWorldPlayer final
{
    std::unique_ptr<rig::CharacterPose> pose;
    rig::DriveState drive;
    float last_seconds{-1.0F};
    // This frame's animation from world::PlayerAnimator, when the caller supplies it.
    std::optional<world::PlayerAnimationPose> animation;
};

// The model every InventoryItemClass hands to its hand entity. Shared by the
// retail character and the Xbox-avatar path; a missing export reports !loaded().
void load_held_item_models(const std::filesystem::path &derived)
{
    const auto load = [&](const char *file) { return std::make_unique<StaticModelAsset>(renderer_, derived / file); };
    pickaxe_ = load("pickaxe.omsm");
    compass_ = load("compass.omsm");
    pistol_ = load("pistol.omsm");
    knife_ = load("knife.omsm");
    ammo_ = load("ammo.omsm");
    torch_ = load("torch.omsm");
    spade_ = load("spade.omsm");
    axe_ = load("axe.omsm");
    ak_ = load("ak.omsm");
    shotgun_ = load("shotgun.omsm");
    rifle_ = load("rifle.omsm");
    smg_ = load("smg.omsm");
    lmg_ = load("lmg.omsm");
    rpg_ = load("rpg.omsm");
    grenade_ = load("grenade.omsm");
    space_ak_ = load("space_ak.omsm");
    space_pistol_ = load("space_pistol.omsm");
    space_shotgun_ = load("space_shotgun.omsm");
    space_rifle_ = load("space_rifle.omsm");
    space_smg_ = load("space_smg.omsm");
    saber_ = load("saber.omsm");
    ore_ = load("ore.omsm");
    gems_ = load("gems.omsm");
    bars_ = load("bars.omsm");
    clock_ = load("clock.omsm");
    locator_ = load("locator.omsm");
    teleporter_ = load("teleporter.omsm");
    gunpowder_ = load("gunpowder.omsm");
}

// world::PlayerAnimator (the port of Player.UpdateAnimation) reports each
// channel as clip + position + fade weight; translate that to the rig's form.
[[nodiscard]] rig::ChannelStates retail_channel_states(const world::PlayerAnimationPose &animation) const noexcept
{
    rig::ChannelStates states{};
    const auto registration = [&](world::AnimationClip clip) -> const rig::Registration * {
        const auto index = static_cast<std::size_t>(clip);
        return clip == world::AnimationClip::none || index >= retail_clip_lookup_.size() ? nullptr
                                                                                        : retail_clip_lookup_[index];
    };
    for (std::size_t channel = 0; channel < animation.size() && channel < states.size(); ++channel)
    {
        const world::AnimationChannelPose &source = animation[channel];
        rig::ChannelState &target = states[channel];
        target.current = registration(source.current.clip);
        target.current_position = source.current.sample_ticks;
        target.previous = registration(source.previous.clip);
        target.previous_position = source.previous.sample_ticks;
        target.blend = source.weight;
    }
    return states;
}

void load_retail_character()
{
    const std::filesystem::path path = default_character_rig_path();
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        std::cerr << "Retail character not imported (" << path.string()
                  << "); re-import the content pack with tools/ContentBridge\n";
        return;
    }
    try
    {
        retail_rig_ = std::make_unique<rig::RetailRig>(path);
        // (Graphics area) an installed animation pack replaces registered
        // clips here, before anything plays one; without one, nothing changes.
        // (Animation packs) What the import holds is remembered, so a change
        // of packs can start again from it.
        retail_imported_clips_ = retail_rig_->clips.size();
        retail_imported_registrations_.clear();
        for (const rig::Registration &registration : retail_rig_->registrations)
            retail_imported_registrations_.push_back(registration.clip);
        modding::graphics::apply_installed_animation_pack(*retail_rig_);
        for (const rig::MeshPart &part : retail_rig_->parts)
        {
            SDL_Texture *texture = nullptr;
            if (!part.rgba.empty())
            {
                // The body is opaque; XNA leaves the fourth byte of an opaque
                // texture unspecified, so it must not become coverage.
                std::vector<std::uint8_t> pixels = part.rgba;
                for (std::size_t alpha = 3U; alpha < pixels.size(); alpha += 4U)
                    pixels[alpha] = 255U;
                if (SDL_Surface *surface = SDL_CreateSurfaceFrom(part.texture_width, part.texture_height,
                                                                 SDL_PIXELFORMAT_RGBA32, pixels.data(),
                                                                 part.texture_width * 4))
                {
                    texture = SDL_CreateTextureFromSurface(renderer_, surface);
                    SDL_DestroySurface(surface);
                    if (texture != nullptr)
                    {
                        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
                        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
                    }
                }
            }
            retail_textures_.push_back(texture);
        }
        retail_first_person_ = std::make_unique<rig::CharacterPose>(*retail_rig_);
        retail_first_person_->hide_head = true;
        for (std::size_t clip = 1; clip < retail_clip_lookup_.size(); ++clip)
            retail_clip_lookup_[clip] =
                retail_rig_->find(world::animation_clip_info(static_cast<world::AnimationClip>(clip)).name);
        gunfx_pack_changed(); // (GunFX) the pack's clip variants
    }
    catch (const std::exception &exception)
    {
        std::cerr << "Retail character failed to load: " << exception.what() << '\n';
        for (SDL_Texture *texture : retail_textures_)
            SDL_DestroyTexture(texture);
        retail_textures_.clear();
        retail_first_person_.reset();
        retail_rig_.reset();
    }
}

// (Animation packs) The installed packs changed (a world was entered after
// the character loaded, or mods were added or removed): back to the imported
// clips, then the packs installed now. Players and the first-person driver
// hold pointers to clips, so every character's animation starts afresh.
void reload_retail_animation_pack()
{
    if (retail_rig_ == nullptr || retail_imported_registrations_.size() > retail_rig_->registrations.size())
        return;
    // (GunFX) A pack's clip variants are registrations of their own, added
    // after the imported ones: they go with the pack.
    const bool had_pack = retail_rig_->clips.size() != retail_imported_clips_ ||
                          retail_rig_->registrations.size() != retail_imported_registrations_.size();
    retail_rig_->registrations.resize(retail_imported_registrations_.size());
    retail_rig_->clips.resize(retail_imported_clips_);
    for (std::size_t index = 0; index < retail_imported_registrations_.size(); ++index)
        retail_rig_->registrations[index].clip = retail_imported_registrations_[index];
    modding::graphics::apply_installed_animation_pack(*retail_rig_);
    // Adding variants may have moved the registrations.
    for (std::size_t clip = 1; clip < retail_clip_lookup_.size(); ++clip)
        retail_clip_lookup_[clip] =
            retail_rig_->find(world::animation_clip_info(static_cast<world::AnimationClip>(clip)).name);
    gunfx_pack_changed();
    if (!had_pack && retail_rig_->clips.size() == retail_imported_clips_ &&
        retail_rig_->registrations.size() == retail_imported_registrations_.size())
        return; // no pack before or after: nothing to restart
    retail_first_person_ = std::make_unique<rig::CharacterPose>(*retail_rig_);
    retail_first_person_->hide_head = true;
    retail_first_person_drive_ = {};
    retail_first_person_seen_ = -1.0F;
    retail_world_players_.clear();
}

void destroy_retail_character() noexcept
{
    for (SDL_Texture *texture : retail_textures_)
        SDL_DestroyTexture(texture);
    retail_textures_.clear();
}

[[nodiscard]] bool retail_active() const noexcept
{
    return retail_rig_ != nullptr && !xbox_avatar_skin_enabled();
}

[[nodiscard]] bool retail_world_animation_ready() const noexcept
{
    if (retail_rig_ == nullptr)
        return false;
    for (const std::string_view required : {"Stand", "Walk", "Run", "Die", "Tilt", "IdleHead", "GenericIdle"})
        if (retail_rig_->find(required) == nullptr)
            return false;
    return true;
}

// The model each InventoryItemClass hands to its entity.
[[nodiscard]] StaticModelAsset *retail_held_model(std::uint16_t item) const noexcept
{
    switch (item)
    {
    case 13U: return pickaxe_.get();
    case 14U: return torch_.get();
    case 15U: case 16U: case 17U: case 18U: return ore_.get();
    case 19U: return gems_.get();
    case 20U: case 21U: case 22U: return bars_.get();
    case 23U: case 24U: case 25U: case 26U: case 27U: case 28U: return pickaxe_.get();
    case 29U: case 30U: case 31U: case 32U: case 33U: return spade_.get();
    case 34U: case 35U: case 36U: case 37U: case 38U: return axe_.get();
    case 39U: return compass_.get();
    case 40U: return clock_.get();
    case 41U: case 42U: case 43U: case 44U: case 45U: case 46U: case 47U: case 48U:
    case 105U: case 109U: case 111U: return ammo_.get();
    case 49U: case 55U: case 61U: case 67U: return knife_.get();
    case 50U: case 56U: case 62U: case 68U: return ak_.get();
    case 51U: case 57U: case 63U: case 69U: return pistol_.get();
    case 52U: case 58U: case 64U: case 70U: return shotgun_.get();
    case 53U: case 59U: case 65U: case 71U: return rifle_.get();
    case 54U: case 60U: case 66U: case 72U: return smg_.get();
    case 77U: return locator_.get();
    case 78U: return teleporter_.get();
    case 81U: case 114U: case 115U: case 116U: case 117U: case 118U: return saber_.get();
    case 82U: case 87U: case 92U: case 97U: return space_ak_.get();
    case 83U: case 88U: case 93U: case 98U: return space_pistol_.get();
    case 84U: case 89U: case 94U: case 99U: return space_shotgun_.get();
    case 85U: case 90U: case 95U: case 100U: return space_rifle_.get();
    case 86U: case 91U: case 96U: case 101U: return space_smg_.get();
    case 103U: case 104U: case 112U: case 113U: return rpg_.get();
    case 106U: case 108U: return gunpowder_.get();
    case 110U: return grenade_.get();
    case 119U: case 120U: case 121U: case 122U: return lmg_.get();
    default: return nullptr;
    }
}

// CastleMinerToolModel.ToolColor / ToolColor2 per item.
static void retail_item_tints(std::uint16_t item, std::array<float, 3> &primary,
                              std::array<float, 3> &secondary) noexcept
{
    const auto rgb = [](int red, int green, int blue) {
        constexpr float scale = 1.0F / 255.0F;
        return std::array<float, 3>{red * scale, green * scale, blue * scale};
    };
    primary = item_material_tint(static_cast<InventoryItemId>(item));
    secondary = {1.0F, 1.0F, 1.0F};
    switch (item)
    {
    case 15U: primary = rgb(20, 20, 20); break;
    case 16U: primary = rgb(99, 146, 131); break;
    case 17U: primary = rgb(183, 65, 14); break;
    case 18U: primary = rgb(255, 215, 0); break;
    case 19U: primary = rgb(0, 255, 255); break;
    case 20U: primary = rgb(128, 128, 128); break;
    case 21U: primary = rgb(184, 115, 51); break;
    case 22U: primary = rgb(255, 215, 0); break;
    case 41U: case 42U: case 43U: case 44U: case 45U: case 46U: case 47U: case 48U:
    case 105U: case 109U: case 111U:
        primary = rgb(169, 169, 169);
        secondary = rgb(234, 227, 150);
        switch (item)
        {
        case 41U: secondary = rgb(181, 166, 66); break;
        case 42U: secondary = rgb(128, 128, 128); break;
        case 43U: secondary = rgb(255, 215, 0); break;
        case 111U: secondary = rgb(0, 255, 255); break;
        case 45U: primary = rgb(128, 128, 128); break;
        case 46U: primary = rgb(255, 215, 0); break;
        case 47U: primary = rgb(0, 255, 255); break;
        case 48U: primary = rgb(139, 0, 0); secondary = rgb(0, 255, 255); break;
        case 109U: primary = rgb(50, 205, 50); secondary = rgb(169, 169, 169); break;
        default: break;
        }
        break;
    case 108U: primary = rgb(224, 48, 40); break;
    default: break;
    }
}

[[nodiscard]] static float retail_item_material(std::uint16_t item, std::uint8_t flags) noexcept
{
    const bool recolored = (flags & 1U) != 0U;
    if ((flags & 4U) != 0U)
        return -3.0F; // procedural emissive blade
    if ((flags & 8U) != 0U)
        return 1.22F; // texture alpha is the gun's emissive tier mask
    const bool saber = item == 81U || (item >= 114U && item <= 118U);
    const bool space = item >= 82U && item <= 104U;
    const bool firearm = (item >= 50U && item <= 72U && item != 55U && item != 61U && item != 67U) ||
                         (item >= 119U && item <= 122U);
    const bool edged = (item >= 23U && item <= 38U) || item == 49U || item == 55U || item == 61U || item == 67U;
    if (saber)
        return 0.30F;
    if (space)
        return 0.22F;
    if (firearm)
        return recolored ? 0.22F : 0.14F;
    if (edged)
        return recolored ? 0.18F : 0.025F;
    return recolored ? 0.12F : 0.02F;
}

// (Items) Eat and drink: while the use runs the use channel plays BlockUse
// on the item's own clock (held_item.hpp, mouth_clip_fraction) - the hand
// goes up to the mouth, takes a few bites and comes down - however short the
// retail clip itself is.
void apply_mouth_use(rig::ChannelStates &states, const modding::graphics::HeldItemPose *mod) const
{
    using modding::graphics::HoldStyle;
    if (mod == nullptr || mod->use_progress <= 0.0F ||
        (mod->look.hold != HoldStyle::eat && mod->look.hold != HoldStyle::drink))
        return;
    const auto index = static_cast<std::size_t>(world::AnimationClip::BlockUse);
    const rig::Registration *use = index < retail_clip_lookup_.size() ? retail_clip_lookup_[index] : nullptr;
    if (use == nullptr || use->clip < 0 || static_cast<std::size_t>(use->clip) >= retail_rig_->clips.size())
        return;
    const std::int64_t duration = retail_rig_->clips[static_cast<std::size_t>(use->clip)].duration_ticks;
    rig::ChannelState &channel = states[static_cast<std::size_t>(world::AnimationChannel::upper_use)];
    channel.current = use;
    channel.current_position = static_cast<std::int64_t>(
        static_cast<double>(duration) * modding::graphics::mouth_clip_fraction(mod->use_progress));
    channel.previous = nullptr;
    channel.previous_position = 0;
    channel.blend = 1.0F;
}

// (Items) A held mod item, in avatar space: placed on the hand transform of
// its grip's reference item (the knife for the fist, a block for the front)
// under the hand bone, the reference's own scale undone so the item keeps its
// authored size. A glTF model is posed by the item's clips and drawn with its
// materials; an icon as its solid sprite.
void append_held_mod_item(const modding::graphics::HeldItemPose &mod, bool local, const rig::Matrix &right_hand)
{
    namespace graphics = modding::graphics;
    const graphics::ItemLook &look = mod.look;
    const rig::HeldItem *reference =
        retail_rig_->item(static_cast<std::uint16_t>(graphics::grip_reference_item(look.hold)));
    const rig::Matrix hand = reference != nullptr ? (local ? reference->hand : reference->hand_remote) : rig::identity();
    const float determinant = hand[0] * (hand[5] * hand[10] - hand[6] * hand[9]) -
                              hand[1] * (hand[4] * hand[10] - hand[6] * hand[8]) +
                              hand[2] * (hand[4] * hand[9] - hand[5] * hand[8]);
    const bool icon = look.model == nullptr;
    // A glTF matrix (column vectors) and an XNA one (row vectors) that move
    // points the same way share their memory layout.
    const rig::Matrix grip = graphics::held_item_transform(look, mod.use_progress,
                                                           std::cbrt(std::abs(determinant)), icon);
    const rig::Matrix placed = rig::multiply(rig::multiply(grip, hand), right_hand);
    const auto unit = [](std::array<float, 3> normal) {
        const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        return length > 1.0e-6F ? std::array<float, 3>{normal[0] / length, normal[1] / length, normal[2] / length}
                                : std::array<float, 3>{0.0F, 1.0F, 0.0F};
    };
    if (icon)
    {
        RetailSpan span;
        span.texture = white_texture_;
        span.first = static_cast<std::uint32_t>(retail_vertices_.size());
        span.material = 0.02F;
        span.item = true;
        span.vertex_albedo = true;
        for (const graphics::IconModelVertex &source : look.icon->vertices)
        {
            RetailVertex vertex;
            vertex.position = rig::transform_point(source.position.data(), placed);
            vertex.normal = unit(rig::transform_vector(source.normal.data(), placed));
            vertex.uv = {0.5F, 0.5F};
            vertex.color = source.color;
            retail_vertices_.push_back(vertex);
        }
        span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
        if (span.count > 0U)
            retail_spans_.push_back(span);
        return;
    }
    graphics::pose_item_model(*look.model, {mod.layers.data(), mod.layer_count}, held_item_parts_,
                              held_item_scratch_);
    for (const graphics::PosedItemPart &part : held_item_parts_)
    {
        if (part.vertices.empty())
            continue;
        SDL_Texture *texture = white_texture_;
        if (part.material >= 0 && static_cast<std::size_t>(part.material) < look.model->materials.size() &&
            mod_material_textures_)
            if (SDL_Texture *own = mod_material_textures_(look.model,
                                                          look.model->materials[static_cast<std::size_t>(part.material)]))
                texture = own;
        const std::array<float, 4> color = graphics::item_material_color(*look.model, part.material);
        RetailSpan span;
        span.texture = texture;
        span.first = static_cast<std::uint32_t>(retail_vertices_.size());
        span.material = 0.12F; // a little of the held items' sheen
        span.item = true;
        span.vertex_albedo = true;
        for (const graphics::PosedItemVertex &source : part.vertices)
        {
            const std::array<float, 3> position{source.position.x, source.position.y, source.position.z};
            const std::array<float, 3> normal{source.normal.x, source.normal.y, source.normal.z};
            RetailVertex vertex;
            vertex.position = rig::transform_point(position.data(), placed);
            vertex.normal = unit(rig::transform_vector(normal.data(), placed));
            vertex.uv = source.uv;
            vertex.color = color;
            retail_vertices_.push_back(vertex);
        }
        span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
        retail_spans_.push_back(span);
    }
}

// The body and the held item, in avatar space (XNA axes, feet at the origin,
// facing -Z), as a triangle list in retail_vertices_/retail_spans_. With a
// `skin`, the player's Xbox avatar stands in for the SWATMale mesh on the same
// pose (xbox_avatar_view.inc); everything else is unchanged.
void build_retail_geometry(const rig::CharacterPose &pose, const RetailHeldItem &held, SDL_Texture *block_atlas,
                           const SkinInstance *skin = nullptr,
                           xbox_avatar::Placement placement = xbox_avatar::Placement::own_proportions)
{
    retail_vertices_.clear();
    retail_spans_.clear();
    const rig::RetailRig &rig = *retail_rig_;
    const std::vector<rig::Matrix> &palette = pose.skin();
    if (skin != nullptr)
        append_skin_geometry(*skin, pose, placement);
    for (std::size_t part_index = 0; skin == nullptr && part_index < rig.parts.size(); ++part_index)
    {
        const rig::MeshPart &part = rig.parts[part_index];
        // Skin every vertex once, then expand the index list.
        retail_skinned_.resize(part.vertices.size());
        for (std::size_t index = 0; index < part.vertices.size(); ++index)
        {
            const rig::MeshVertex &source = part.vertices[index];
            RetailVertex &target = retail_skinned_[index];
            std::array<float, 3> position{};
            std::array<float, 3> normal{};
            for (std::size_t influence = 0; influence < 4U; ++influence)
            {
                const float weight = source.weights[influence];
                if (weight == 0.0F)
                    continue;
                const rig::Matrix &bone = palette[source.bones[influence]];
                const auto moved = rig::transform_point(source.position.data(), bone);
                const auto turned = rig::transform_vector(source.normal.data(), bone);
                for (std::size_t axis = 0; axis < 3U; ++axis)
                {
                    position[axis] += moved[axis] * weight;
                    normal[axis] += turned[axis] * weight;
                }
            }
            target.position = position;
            target.normal = normal;
            target.uv = source.uv;
        }
        RetailSpan span;
        span.texture = part_index < retail_textures_.size() && retail_textures_[part_index] != nullptr
                           ? retail_textures_[part_index]
                           : white_texture_;
        span.first = static_cast<std::uint32_t>(retail_vertices_.size());
        for (const std::uint32_t index : part.indices)
            retail_vertices_.push_back(retail_skinned_[index]);
        span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
        if (span.count > 0U)
            retail_spans_.push_back(span);
    }

    // Player.PutItemInHand: RightHand.Children = { CreateEntity(ItemUse.Hand) }.
    // An avatar standing on its own proportions carries the item on its own
    // PropRight, the same bone on a differently sized body.
    const rig::Matrix right_hand =
        skin != nullptr ? xbox_avatar::held_item_parent(pose, skin_pose_, placement) : pose.right_hand();
    rig::Matrix hand = rig::identity();
    if (const rig::HeldItem *entry = rig.item(held.item))
        hand = held.local ? entry->hand : entry->hand_remote;
    const rig::Matrix item_parent = rig::multiply(hand, right_hand);

    // Modding, visuals area: a mod item is drawn instead of its fallback's
    // model. (Items) Its glTF model posed by its own clips, or its icon in
    // solid form, gripped for its hold style.
    if (held.mod != nullptr && !held.mod->look.empty())
    {
        append_held_mod_item(*held.mod, held.local, right_hand);
        return;
    }

    StaticModelAsset *model = retail_held_model(held.item);
    // (GunFX) A model a client mod cut into parts: each part where its clip
    // and cues put it.
    if (gunfx_frame_ != nullptr && gunfx_frame_->model == model && gunfx_frame_->item == held.item &&
        model != nullptr && model->loaded())
    {
        gunfx_append_model(*gunfx_frame_, pose, item_parent, held);
        return;
    }
    if (model != nullptr && model->loaded())
    {
        std::array<float, 3> primary{};
        std::array<float, 3> secondary{};
        retail_item_tints(held.item, primary, secondary);
        const bool gray_launcher = held.item == 104U || held.item == 113U;
        constexpr float pi = std::numbers::pi_v<float>;
        // CompassEntity, GPSEntity and ClockEntity turn their model under the
        // fixed hand transform.
        Matrix4 dial = identity_matrix();
        if (held.live_instruments)
        {
            float toward_x{};
            float toward_z{};
            bool points = false;
            if (held.item == 39U)
            {
                toward_x = -held.position.x;
                toward_z = -held.position.z;
                points = true;
            }
            else if (held.item == 77U || held.item == 78U)
            {
                toward_x = held.waypoint.x - held.position.x;
                toward_z = held.waypoint.z - held.position.z;
                points = true;
            }
            if (points && toward_x * toward_x + toward_z * toward_z > 0.000001F)
                dial = rotation_y(held.yaw - std::atan2(toward_x, -toward_z));
            if (held.item == 40U)
                dial = rotation_y(-2.0F * pi * held.clock_time_of_day);
        }
        for (const StaticModelPart &part : model->parts())
        {
            // StickInventoryItemClass shows the pick's handle without its head.
            if (held.item == 13U && (part.material_flags & 1U) != 0U)
                continue;
            const Matrix4 part_world = multiply_matrix(multiply_matrix(part.transform, dial), item_parent);
            const std::array<float, 3> tint =
                (part.material_flags & 8U) != 0U ? projectile_tint(static_cast<InventoryItemId>(held.item))
                : gray_launcher ? std::array<float, 3>{128.0F / 255.0F, 128.0F / 255.0F, 128.0F / 255.0F}
                : (part.material_flags & 2U) != 0U ? secondary
                : (part.material_flags & 1U) != 0U ? primary
                                                   : std::array<float, 3>{1.0F, 1.0F, 1.0F};
            RetailSpan span;
            span.texture = part.texture != nullptr ? part.texture : white_texture_;
            if ((part.material_flags & 4U) != 0U)
                span.texture = white_texture_;
            span.first = static_cast<std::uint32_t>(retail_vertices_.size());
            span.material = retail_item_material(held.item, part.material_flags);
            span.item = true;
            span.torch = held.item == 14U;
            constexpr float color_scale = 1.0F / 255.0F;
            for (const std::uint32_t index : part.indices)
            {
                if (index >= part.vertices.size())
                    continue;
                const StaticModelVertex &source = part.vertices[index];
                RetailVertex vertex;
                vertex.position = rig::transform_point(source.position.data(), part_world);
                vertex.normal = rig::transform_vector(source.normal.data(), part_world);
                vertex.uv = source.uv;
                vertex.color = {part.diffuse[0] * source.color[0] * color_scale * tint[0],
                                part.diffuse[1] * source.color[1] * color_scale * tint[1],
                                part.diffuse[2] * source.color[2] * color_scale * tint[2],
                                source.color[3] * color_scale};
                retail_vertices_.push_back(vertex);
            }
            span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
            span.count -= span.count % 3U;
            retail_vertices_.resize(span.first + span.count);
            if (span.count > 0U)
                retail_spans_.push_back(span);
        }
    }

    if (held.block != world::BlockType::empty && held.item != 14U && block_atlas != nullptr &&
        (model == nullptr || !model->loaded()))
    {
        // BlockInventoryItemClass: a BlockEntity with Scale 0.1 whose cube is
        // centred on the entity origin, under the imported hand transform.
        constexpr std::array<std::array<float, 3>, 8> corners{{
            {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
            {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 1.0F, 1.0F},
        }};
        struct Face final
        {
            std::array<std::size_t, 4> corners{};
            std::array<float, 3> normal{};
            world::FaceDirection direction{};
            float shade{};
        };
        constexpr std::array<Face, 6> faces{{
            {{{1U, 2U, 6U, 5U}}, {1.0F, 0.0F, 0.0F}, world::FaceDirection::positive_x, 0.76F},
            {{{0U, 4U, 7U, 3U}}, {-1.0F, 0.0F, 0.0F}, world::FaceDirection::negative_x, 0.70F},
            {{{3U, 7U, 6U, 2U}}, {0.0F, 1.0F, 0.0F}, world::FaceDirection::positive_y, 1.00F},
            {{{0U, 1U, 5U, 4U}}, {0.0F, -1.0F, 0.0F}, world::FaceDirection::negative_y, 0.58F},
            {{{4U, 5U, 6U, 7U}}, {0.0F, 0.0F, 1.0F}, world::FaceDirection::positive_z, 0.86F},
            {{{0U, 3U, 2U, 1U}}, {0.0F, 0.0F, -1.0F}, world::FaceDirection::negative_z, 0.82F},
        }};
        constexpr std::array<std::size_t, 6> order{0U, 1U, 2U, 0U, 2U, 3U};
        constexpr float cube_scale = 0.1F;
        RetailSpan span;
        span.texture = block_atlas;
        span.first = static_cast<std::uint32_t>(retail_vertices_.size());
        span.material = 0.0F;
        span.item = true;
        for (const Face &face : faces)
        {
            const auto tile = world::atlas_tile(held.block, face.direction);
            const float u0 = static_cast<float>(tile & 7U) / 8.0F + 0.0012F;
            const float v0 = static_cast<float>(tile >> 3U) / 8.0F + 0.0012F;
            const float u1 = static_cast<float>((tile & 7U) + 1U) / 8.0F - 0.0012F;
            const float v1 = static_cast<float>((tile >> 3U) + 1U) / 8.0F - 0.0012F;
            const auto uv = world::atlas_uv_corners(face.direction, u0, v0, u1, v1);
            for (const std::size_t corner_index : order)
            {
                const auto &corner = corners[face.corners[corner_index]];
                const std::array<float, 3> local{(corner[0] - 0.5F) * cube_scale, (corner[1] - 0.5F) * cube_scale,
                                                 (corner[2] - 0.5F) * cube_scale};
                RetailVertex vertex;
                vertex.position = rig::transform_point(local.data(), item_parent);
                vertex.normal = rig::transform_vector(face.normal.data(), item_parent);
                vertex.uv = {uv[corner_index][0], uv[corner_index][1]};
                vertex.color = {face.shade, face.shade, face.shade, 1.0F};
                retail_vertices_.push_back(vertex);
            }
        }
        span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
        retail_spans_.push_back(span);
    }
}

// First person: the body through Avatar.EyePointCamera.
void draw_retail_first_person(const ui::UiMetrics &metrics, const world::PlayerState &player,
                              float elapsed_seconds, float viewmodel_fov_degrees, SDL_Texture *block_atlas)
{
    rig::CharacterPose &pose = *retail_first_person_;
    const float width = static_cast<float>(metrics.output_width);
    const float height = static_cast<float>(metrics.output_height);
    const bool first_frame = last_elapsed_seconds_ < 0.0F || elapsed_seconds < last_elapsed_seconds_;
    const float delta_seconds = first_frame ? 0.0F : std::min(elapsed_seconds - last_elapsed_seconds_, 0.25F);
    last_elapsed_seconds_ = elapsed_seconds;

    const bool holding_block = player.selected_block != world::BlockType::empty && player.selected_item_id != 14U;
    rig::FirstPersonInput input;
    input.mode = holding_block ? PlayerAnimationMode::block
                               : item_player_animation_mode(static_cast<InventoryItemId>(player.selected_item_id));
    input.cue = player.viewmodel_cue;
    if (input.cue == world::ViewmodelCue::idle && player.using_tool)
        input.cue = world::ViewmodelCue::primary_use;
    input.cue_progress = player.viewmodel_cue_progress;
    // Retail walks at 4 m/s; the stance walk clip plays at the stick's share of it.
    input.movement = std::clamp(std::hypot(player.velocity.x, player.velocity.z) / 4.0F, 0.0F, 1.0F);
    input.snap = first_frame;
    pose.hide_head = true;
    // Avatar.EyePointCamera is built from the head of the PREVIOUS update. That
    // is right while the view runs; across a break (the first frame, a return
    // from death or a menu, a change of who drives the pose) the previous head
    // belongs to another moment, so the pose is applied twice and the camera
    // starts on the head it is about to look from.
    const bool external = retail_first_person_animation_.has_value();
    const bool resumed = first_frame || elapsed_seconds - retail_first_person_seen_ > 0.25F ||
                         external != retail_first_person_external_;
    retail_first_person_seen_ = elapsed_seconds;
    retail_first_person_external_ = external;
    // (GunFX) channels 2 and 3 as drawn, for the parts and cues.
    rig::ChannelStates gunfx_states{};
    if (external)
    {
        // The animation state machine evaluated this frame itself.
        rig::ChannelStates states = retail_channel_states(*retail_first_person_animation_);
        gunfx_adjust(states, static_cast<std::uint16_t>(player.selected_item_id), true); // (GunFX)
        gunfx_states = states;
        if (SDL_getenv("OPENMINERZ_RIG_TRACE") != nullptr)
        {
            std::cerr << "rig first-person t=" << elapsed_seconds;
            for (std::size_t channel = 0; channel < 6U; ++channel)
            {
                const rig::ChannelState &state = states[channel];
                std::cerr << " | c" << channel << ' ' << (state.current != nullptr ? state.current->name : "-")
                          << '@' << state.current_position << " <- "
                          << (state.previous != nullptr ? state.previous->name : "-") << '@'
                          << state.previous_position << " w=" << state.blend;
            }
            std::cerr << '\n';
        }
        retail_first_person_animation_.reset();
        apply_mouth_use(states, held_mod_item_.get()); // (Items)
        pose.apply(states);
        if (resumed)
            pose.apply(states);
    }
    else
    {
        // (Items) A mod item's hold style picks the arm clips, and an eat or
        // drink use holds the hand at the mouth as the running animation does.
        if (held_mod_item_ != nullptr && !held_mod_item_->look.empty())
        {
            using modding::graphics::HoldStyle;
            const HoldStyle hold = held_mod_item_->look.hold;
            input.mode = modding::graphics::hold_animation_mode(hold, static_cast<InventoryItemId>(player.selected_item_id));
            if ((hold == HoldStyle::eat || hold == HoldStyle::drink) && held_mod_item_->use_progress > 0.0F)
            {
                input.cue = world::ViewmodelCue::primary_use;
                input.cue_progress = modding::graphics::mouth_clip_fraction(held_mod_item_->use_progress);
            }
        }
        input.snap = input.snap || resumed;
        rig::drive_first_person(pose, retail_first_person_drive_, input);
        pose.update_seconds(static_cast<double>(delta_seconds));
        if (resumed)
        {
            // Finish every blend the resumed driver started and seat the camera.
            pose.update(0);
            pose.update(0);
        }
        gunfx_states = gunfx_states_of(pose); // (GunFX)
    }

    RetailHeldItem held;
    held.item = static_cast<std::uint16_t>(player.selected_item_id);
    held.local = true;
    held.block = holding_block ? player.selected_block : world::BlockType::empty;
    held.live_instruments = true;
    held.yaw = player.yaw;
    held.position = player.position;
    held.waypoint = player.selected_waypoint;
    held.clock_time_of_day = player.clock_time_of_day;
    held.mod = held_mod_item_.get(); // Modding, visuals area; (Items)
    // (GunFX) the held model's parts and this frame's cues.
    const bool gunfx_model = held.mod == nullptr || held.mod->look.empty();
    gunfx_frame_ = gunfx_model ? gunfx_prepare(gunfx_states, held.item, true) : nullptr;
    // Seen from its own head, an avatar keeps the stock joints: the camera and
    // the held item are where the stock skeleton puts them.
    build_retail_geometry(pose, held, block_atlas, skin_for(local_player_gid),
                          xbox_avatar::Placement::stock_joints);
    gunfx_frame_ = nullptr;
    if (gunfx_library() != nullptr)
        gunfx_fire(gunfx_self_, gunfx_states, held.item, true, {});

    constexpr float pi = std::numbers::pi_v<float>;
    Matrix4 view = rig::invert(pose.eye());
    // (GunFX) the procedural motion moves the viewmodel, never the camera.
    view = rig::multiply(view, gunfx_motion_offset(player, delta_seconds));
    if (!gunfx_pending_drops_.empty())
    {
        // A part dropped from the hands is handed to the world where it is
        // drawn: through the viewmodel camera into the world camera, which
        // shares its eye and direction (its position scaled by the two
        // fields of view, as view::viewmodel_to_world_view does).
        const float cy = std::cos(player.yaw);
        const float sy = std::sin(player.yaw);
        const float cp = std::cos(player.pitch);
        const float sp = std::sin(player.pitch);
        const std::array<float, 3> forward{sy * cp, sp, -cy * cp};
        const std::array<float, 3> right{cy, 0.0F, sy};
        const std::array<float, 3> up{right[1] * forward[2] - right[2] * forward[1],
                                      right[2] * forward[0] - right[0] * forward[2],
                                      right[0] * forward[1] - right[1] * forward[0]};
        rig::Matrix camera = rig::identity();
        for (std::size_t axis = 0U; axis < 3U; ++axis)
        {
            camera[axis] = right[axis];
            camera[4U + axis] = up[axis];
            camera[8U + axis] = -forward[axis];
        }
        const world::Vec3 eye{player.position.x, player.position.y + world::PlayerSimulation::eye_height,
                              player.position.z};
        const float scale =
            gunfx_world_fov_ > 1.0F
                ? std::tan(view::sanitized_fov_degrees(gunfx_world_fov_) * pi / 360.0F) /
                      std::tan(view::sanitized_fov_degrees(viewmodel_fov_degrees) * pi / 360.0F)
                : 1.0F;
        gunfx_flush_drops(held.item, player.velocity,
                          [&](const rig::Matrix &avatar, rig::Matrix &linear, world::Vec3 &origin) {
                              const rig::Matrix in_view = rig::multiply(avatar, view);
                              rig::Matrix turn = in_view;
                              turn[12] = turn[13] = turn[14] = 0.0F;
                              linear = rig::multiply(turn, camera);
                              const float x = in_view[12] * scale;
                              const float y = in_view[13] * scale;
                              const float z = in_view[14];
                              origin = {eye.x + x * right[0] + y * up[0] - z * forward[0],
                                        eye.y + x * right[1] + y * up[1] - z * forward[1],
                                        eye.z + x * right[2] + y * up[2] - z * forward[2]};
                          });
    }
    const float aspect = width / std::max(1.0F, height);
    const float fov = view::sanitized_fov_degrees(viewmodel_fov_degrees) * pi / 180.0F;
    const float focal = 1.0F / std::tan(fov * 0.5F);
    const bool depth_pass = depth_ready_;

    std::vector<ViewmodelTriangle> &triangles = retail_triangles_;
    triangles.clear();
    triangles.reserve(retail_vertices_.size() / 3U + 2U);
    std::array<float, 3> torch_tip_view{};
    float torch_tip_screen_y = std::numeric_limits<float>::max();
    for (const RetailSpan &span : retail_spans_)
    {
        for (std::uint32_t offset = 0; offset + 2U < span.count; offset += 3U)
        {
            ViewmodelTriangle output;
            output.render_layer = span.item ? 1 : 0;
            output.texture = span.texture;
            bool all_visible = true;
            bool any_visible = false;
            for (std::size_t corner = 0; corner < 3U; ++corner)
            {
                const RetailVertex &source = retail_vertices_[span.first + offset + corner];
                const auto camera = rig::transform_point(source.position.data(), view);
                const float depth = -camera[2];
                const bool visible = depth > 0.011F;
                SDL_FPoint point{};
                if (visible)
                    point = {(camera[0] * focal / (depth * aspect) * 0.5F + 0.5F) * width,
                             (0.5F - camera[1] * focal / depth * 0.5F) * height};
                all_visible = all_visible && visible;
                any_visible = any_visible || visible;
                if (span.torch && visible && point.y < torch_tip_screen_y)
                {
                    torch_tip_screen_y = point.y;
                    torch_tip_view = {camera[0], camera[1], depth};
                }
                const std::array<float, 3> camera_normal = transform_direction(source.normal, view);
                const std::array<float, 3> light = span.material < -2.5F || span.material > 1.0F
                    ? std::array<float, 3>{1, 1, 1} : environment_light(camera_normal);
                output.view[corner] = {camera[0], camera[1], depth};
                // (Items) A vertex colour stands for a texel: decoded like one.
                const auto albedo = [&](float channel) {
                    return span.vertex_albedo && env_rebirth_ ? channel * channel : channel;
                };
                output.vertices[corner] = {
                    point,
                    {albedo(source.color[0]) * light[0], albedo(source.color[1]) * light[1],
                     albedo(source.color[2]) * light[2], source.color[3]},
                    {source.uv[0], source.uv[1]}};
                output.normal_material[corner] = {camera_normal[0], camera_normal[1], -camera_normal[2],
                                                  span.material};
                output.depth += depth;
            }
            if (depth_pass ? any_visible : all_visible)
                triangles.push_back(output);
        }
    }
    if (gun_stats(static_cast<InventoryItemId>(held.item)) != nullptr)
    {
        // GunEntity.BarrelTipLocation is the gun model's "BarrelTip" bone, which
        // the static-model import does not carry. A gun is held pointing away
        // from the eye, so its muzzle is the part of the model furthest ahead:
        // the centre of everything within two centimetres of that depth.
        float furthest = 0.0F;
        for (const ViewmodelTriangle &triangle : triangles)
            if (triangle.render_layer == 1)
                for (const std::array<float, 3> &corner : triangle.view)
                    furthest = std::max(furthest, corner[2]);
        std::array<double, 3> sum{};
        std::size_t count{};
        for (const ViewmodelTriangle &triangle : triangles)
            if (triangle.render_layer == 1)
                for (const std::array<float, 3> &corner : triangle.view)
                    if (corner[2] >= furthest - 0.02F)
                    {
                        sum[0] += corner[0];
                        sum[1] += corner[1];
                        sum[2] += corner[2];
                        ++count;
                    }
        if (count > 0U && furthest > 0.011F)
        {
            const double scale = 1.0 / static_cast<double>(count);
            muzzle_ = AvatarViewmodel::Muzzle{{static_cast<float>(sum[0] * scale),
                                               static_cast<float>(sum[1] * scale),
                                               static_cast<float>(sum[2] * scale)},
                                              fov * 180.0F / pi};
        }
    }
    if (depth_pass && torch_tip_screen_y < std::numeric_limits<float>::max())
    {
        // TorchEntity carries a fire emitter at the head of the torch; shown as
        // a camera-facing flame at the highest point of the posed model.
        const float flicker =
            0.86F + 0.14F * std::sin(elapsed_seconds * 17.0F + std::sin(elapsed_seconds * 7.3F));
        const float pixel_to_view_y = 2.0F * torch_tip_view[2] / (height * focal);
        const float pixel_to_view_x = 2.0F * torch_tip_view[2] * aspect / (width * focal);
        const float half_width = height * 0.024F * pixel_to_view_x * flicker;
        const float flame_height = height * 0.092F * pixel_to_view_y * flicker;
        const float bottom = torch_tip_view[1] - height * 0.004F * pixel_to_view_y;
        const float top = torch_tip_view[1] + flame_height;
        const float sway = std::sin(elapsed_seconds * 11.0F) * half_width * 0.22F;
        const float flame_depth = std::max(0.0105F, torch_tip_view[2] - 0.0020F);
        const std::array<std::array<float, 3>, 4> corners{{
            {torch_tip_view[0] - half_width, bottom, flame_depth},
            {torch_tip_view[0] + half_width, bottom, flame_depth},
            {torch_tip_view[0] + half_width * 0.72F + sway, top, flame_depth},
            {torch_tip_view[0] - half_width * 0.72F + sway, top, flame_depth},
        }};
        constexpr std::array<SDL_FPoint, 4> uv{{{0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}, {0.0F, 0.0F}}};
        constexpr std::array<std::size_t, 6> order{0U, 1U, 2U, 0U, 2U, 3U};
        for (std::size_t half = 0; half < 2U; ++half)
        {
            ViewmodelTriangle flame;
            flame.render_layer = 3;
            for (std::size_t corner = 0; corner < 3U; ++corner)
            {
                const std::size_t index = order[half * 3U + corner];
                flame.view[corner] = corners[index];
                flame.vertices[corner] = {{}, {1.0F, 1.0F, 1.0F, 1.0F}, uv[index]};
                flame.normal_material[corner] = {0.0F, 0.0F, 1.0F, -2.0F};
                flame.depth += corners[index][2];
            }
            triangles.push_back(flame);
        }
    }
#if defined(_WIN32)
    if (depth_pass && render_depth_pass(triangles, metrics.output_width, metrics.output_height, focal, aspect, 0.0F))
        return;
#endif
    // No depth buffer: far-to-near painter order.
    std::stable_sort(triangles.begin(), triangles.end(), [](const ViewmodelTriangle &left, const ViewmodelTriangle &right) {
        return left.depth > right.depth;
    });
    std::vector<SDL_Vertex> run;
    SDL_Texture *run_texture{};
    const auto flush = [&]() {
        if (!run.empty())
            SDL_RenderGeometry(renderer_, run_texture, run.data(), static_cast<int>(run.size()), nullptr, 0);
        run.clear();
    };
    for (const ViewmodelTriangle &triangle : triangles)
    {
        if (triangle.texture != run_texture)
            flush();
        run_texture = triangle.texture;
        run.insert(run.end(), triangle.vertices.begin(), triangle.vertices.end());
    }
    flush();
}

// Third person: the same body, as a child of the Player entity (feet + heading).
void append_retail_world_player(EntityBatch &output, std::uint16_t gid, InventoryItemId carried_item,
                                world::Vec3 feet, float yaw, float torso_pitch, float animation_seconds,
                                float movement_x, float movement_y, PlayerAnimationMode animation_mode,
                                bool using_tool, bool dead, bool shouldering, bool reloading,
                                bool throwing_grenade, bool ready_to_throw_grenade, float sky_light,
                                float torch_light, SDL_Texture *block_atlas, const world::SkyState &sky,
                                bool rebirth_presentation, bool underwater, bool hide_head,
                                world::Vec3 *eye_position, world::Vec3 *eye_forward,
                                const modding::graphics::HeldItemPose *mod_item = nullptr)
{
    RetailWorldPlayer &state = retail_world_players_[gid];
    const bool first = state.pose == nullptr;
    if (first)
        state.pose = std::make_unique<rig::CharacterPose>(*retail_rig_);
    rig::CharacterPose &pose = *state.pose;
    const float delta_seconds =
        state.last_seconds < 0.0F || animation_seconds < state.last_seconds
            ? 0.0F
            : std::clamp(animation_seconds - state.last_seconds, 0.0F, 0.25F);
    state.last_seconds = animation_seconds;

    rig::ThirdPersonInput input;
    input.walk = movement_y;
    input.strafe = movement_x;
    input.torso_pitch_radians = torso_pitch;
    input.mode = animation_mode;
    input.using_tool = using_tool;
    input.dead = dead;
    input.shouldering = shouldering;
    input.reloading = reloading;
    input.play_grenade = throwing_grenade;
    input.ready_to_throw_grenade = ready_to_throw_grenade;
    input.underwater = underwater;
    input.snap = first;
    pose.hide_head = hide_head;
    // (GunFX) channels 2 and 3 as drawn, for the parts and cues.
    rig::ChannelStates gunfx_states{};
    if (state.animation.has_value())
    {
        // The animation state machine evaluated this frame itself.
        rig::ChannelStates states = retail_channel_states(*state.animation);
        state.animation.reset();
        gunfx_adjust(states, static_cast<std::uint16_t>(carried_item), false); // (GunFX)
        gunfx_states = states;
        apply_mouth_use(states, mod_item); // (Items)
        pose.apply(states);
        if (first)
            pose.apply(states);
    }
    else
    {
        rig::drive_third_person(pose, state.drive, input);
        if (gid == 256U && dead)
        {
            // The local death view owns an exact clock (captures, frames spent
            // outside the frustum): place the faint and its fade from it.
            if (rig::ClipPlayer *death = pose.animations.active(5))
            {
                const std::int64_t elapsed =
                    rig::ticks_from_seconds(static_cast<double>(std::max(0.0F, animation_seconds)));
                death->pause();
                death->set_current_ticks(std::min(death->duration_ticks(), elapsed));
                pose.animations.set_blend_elapsed_ticks(5, elapsed);
            }
        }
        pose.update_seconds(static_cast<double>(delta_seconds));
        if (first)
        {
            pose.update(0);
            // Capture aid: OPENMINERZ_DIAGNOSTIC_AVATAR_SECONDS=<s> shows a
            // newly seen character that far into its animation (mid-stride,
            // settled into an aim) instead of at its first frame.
            if (const char *warm = SDL_getenv("OPENMINERZ_DIAGNOSTIC_AVATAR_SECONDS"))
            {
                const int frames = std::clamp(static_cast<int>(std::atof(warm) * 60.0), 0, 3600);
                input.snap = false;
                for (int frame = 0; frame < frames; ++frame)
                {
                    rig::drive_third_person(pose, state.drive, input);
                    pose.update(166'667);
                }
            }
        }
    }

    RetailHeldItem held;
    held.item = static_cast<std::uint16_t>(carried_item);
    held.local = false;
    held.block = item_block(carried_item);
    held.mod = mod_item; // (Items)
    // (GunFX) another player's weapon parts and cues (not your own body in
    // the death view).
    const bool gunfx_character = gid != 256U && !dead && (mod_item == nullptr || mod_item->look.empty()) &&
                                 gunfx_library() != nullptr;
    if (gunfx_character && !state.animation.has_value() && gunfx_states[2].current == nullptr)
        gunfx_states = gunfx_states_of(pose);
    gunfx_frame_ = gunfx_character ? gunfx_prepare(gunfx_states, held.item, false) : nullptr;
    // `hide_head` is the view from this character's own head (the local death
    // camera), which is placed by the stock skeleton.
    build_retail_geometry(pose, held, block_atlas, skin_for(gid),
                          hide_head ? xbox_avatar::Placement::stock_joints
                                    : xbox_avatar::Placement::own_proportions);
    gunfx_frame_ = nullptr;

    // Player.LocalToWorld: a turn about Y by the heading, then the feet.
    const float model_yaw = -yaw;
    const float cosine = std::cos(model_yaw);
    const float sine = std::sin(model_yaw);
    const auto to_world = [&](const std::array<float, 3> &point) {
        return world::Vec3{feet.x + point[0] * cosine + point[2] * sine, feet.y + point[1],
                           feet.z - point[0] * sine + point[2] * cosine};
    };
    if (eye_position != nullptr)
    {
        const rig::Matrix eye = pose.eye_now();
        *eye_position = to_world({eye[12], eye[13], eye[14]});
    }
    if (eye_forward != nullptr)
    {
        const rig::Matrix eye = pose.eye_now();
        const world::Vec3 end = to_world({-eye[8], -eye[9], -eye[10]});
        *eye_forward = {end.x-feet.x, end.y-feet.y, end.z-feet.z};
    }
    if (gunfx_character)
    {
        // (GunFX) their cues sound at their hands; what they drop falls from
        // where it is drawn.
        const rig::Matrix &hand = pose.right_hand();
        gunfx_fire(gunfx_others_[gid], gunfx_states, held.item, false, to_world({hand[12], hand[13], hand[14]}));
        if (!gunfx_pending_drops_.empty())
        {
            rig::Matrix turn = rig::identity();
            turn[0] = cosine;
            turn[2] = -sine;
            turn[8] = sine;
            turn[10] = cosine;
            gunfx_flush_drops(held.item, {},
                              [&](const rig::Matrix &avatar, rig::Matrix &linear, world::Vec3 &origin) {
                                  rig::Matrix rotation = avatar;
                                  rotation[12] = rotation[13] = rotation[14] = 0.0F;
                                  linear = rig::multiply(rotation, turn);
                                  origin = to_world({avatar[12], avatar[13], avatar[14]});
                              });
        }
    }
    const std::size_t first_vertex = output.vertices.size();
    output.vertices.reserve(first_vertex + retail_vertices_.size());
    for (const RetailSpan &source_span : retail_spans_)
    {
        EntitySpan span;
        span.texture = source_span.texture;
        span.repeat_texture = !source_span.clamp;
        if (source_span.alpha)
            span.blend = EntityBlend::alpha_lit;
        span.first = static_cast<std::uint32_t>(output.vertices.size());
        for (std::uint32_t index = 0; index < source_span.count; ++index)
        {
            const RetailVertex &source = retail_vertices_[source_span.first + index];
            const world::Vec3 position = to_world(source.position);
            std::array<float, 3> normal{source.normal[0] * cosine + source.normal[2] * sine, source.normal[1],
                                        -source.normal[0] * sine + source.normal[2] * cosine};
            const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (length > 0.000001F)
                for (float &component : normal)
                    component /= length;
            else
                normal = {0.0F, 1.0F, 0.0F};
            EntityVertex vertex;
            vertex.x = position.x;
            vertex.y = position.y;
            vertex.z = position.z;
            vertex.u = source.uv[0];
            vertex.v = source.uv[1];
            vertex.red = source.color[0];
            vertex.green = source.color[1];
            vertex.blue = source.color[2];
            vertex.alpha = source.color[3];
            vertex.normal_x = normal[0];
            vertex.normal_y = normal[1];
            vertex.normal_z = normal[2];
            vertex.sky_light = std::clamp(sky_light, 0.0F, 1.0F);
            if (source_span.material < -2.5F)
                vertex.sky_light = -3.0F;
            else if (source_span.material > 1.0F)
                vertex.sky_light = -2.0F;
            vertex.torch_light = std::clamp(torch_light, 0.0F, 1.5F);
            output.vertices.push_back(vertex);
        }
        span.count = static_cast<std::uint32_t>(output.vertices.size()) - span.first;
        if (span.count > 0U)
            output.spans.push_back(span);
    }
    for (std::size_t index = first_vertex; index < output.vertices.size(); ++index)
    {
        EntityVertex &vertex = output.vertices[index];
        if (rebirth_presentation)
        {
            // The shared HDR shader decodes texture albedo to linear light;
            // decode vertex albedo too, before the one world tone map.
            vertex.red *= vertex.red;
            vertex.green *= vertex.green;
            vertex.blue *= vertex.blue;
        }
        else
        {
            const float sun = std::clamp(sky_light, 0.0F, 1.0F);
            const float torch = std::clamp(torch_light, 0.0F, 1.0F) * (1.0F - sun * sky.direct.green);
            const float direct = std::pow(sun, 30.0F) *
                                 std::max(0.0F, vertex.normal_x * sky.sun_x + vertex.normal_y * sky.sun_y +
                                                    vertex.normal_z * sky.sun_z);
            vertex.red *= sky.ambient.red * sun + torch + sky.direct.red * direct;
            vertex.green *= sky.ambient.green * sun + torch * (235.0F / 255.0F) + sky.direct.green * direct;
            vertex.blue *= sky.ambient.blue * sun + torch * (190.0F / 255.0F) + sky.direct.blue * direct;
        }
    }
}

std::unique_ptr<rig::RetailRig> retail_rig_;
// (Animation packs) The imported clip count and each registration's clip.
std::size_t retail_imported_clips_{};
std::vector<int> retail_imported_registrations_;
std::vector<SDL_Texture *> retail_textures_;
std::array<const rig::Registration *, static_cast<std::size_t>(world::AnimationClip::count)> retail_clip_lookup_{};
std::unique_ptr<rig::CharacterPose> retail_first_person_;
rig::DriveState retail_first_person_drive_;
// This frame's animation from world::PlayerAnimator, when the caller supplies it.
std::optional<world::PlayerAnimationPose> retail_first_person_animation_;
float retail_first_person_seen_{-1.0F};
bool retail_first_person_external_{};
std::unordered_map<std::uint16_t, RetailWorldPlayer> retail_world_players_;
std::vector<RetailVertex> retail_skinned_;
std::vector<RetailVertex> retail_vertices_;
std::vector<RetailSpan> retail_spans_;
std::vector<ViewmodelTriangle> retail_triangles_;
