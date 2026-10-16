// Member functions and state of AvatarViewmodel::Impl: GunFX in both views
// (openminerz/modding/gunfx.hpp). Included inside the class body by
// avatar_viewmodel.cpp, after retail_character_view.inc, which calls in.
//
// PRESENTATION ONLY. A client mod's `gunfx.*` definitions (the graphics
// service's gunfx::Library) and its clip files' part tracks
// (gunfx::installed_tracks) decide:
//   * which retail triangles of the held model are parts, and where each part
//     is this frame (its clip track about its pivot, or riding a hand bone),
//     drawn by build_retail_geometry in first and third person alike;
//   * which clip variant plays (an empty reload, a model's own clip);
//   * the cues a clip passes this frame: sounds (yours 2D, others' at their
//     hands), camera kicks, parts dropped as short-lived world props;
//   * the procedural first-person motion, an offset of the viewmodel camera.
// With no service, or a service whose library is empty, every function here
// returns at once and the views draw exactly what they drew before.

struct GunfxCut final
{
    std::uint64_t revision{};
    std::vector<modding::gunfx::PartSpec> specs;
    modding::gunfx::CutResult cut;
    // The pack's own meshes (`model = ...` parts), by part.
    std::vector<std::shared_ptr<const modding::graphics::Model>> own;
};

// One character's part state for the frame being drawn.
struct GunfxFrame final
{
    const GunfxCut *cut{};
    StaticModelAsset *model{};
    std::uint16_t item{};
    // Per part: visible, its motion in model space, and when riding a bone,
    // the bone and its grip on it (model space -> bone space), eased back
    // onto the track by `detach_weight` (1: on the bone).
    std::vector<bool> visible;
    std::vector<rig::Matrix> motion;
    std::vector<int> bone;
    std::vector<rig::Matrix> grip;
    std::vector<float> carried;
    // Filled by build_retail_geometry: model space -> avatar space as drawn.
    std::vector<rig::Matrix> placement;
};

struct GunfxCharacter final
{
    // The item channel (2) and the use channel (3).
    std::array<modding::gunfx::CueCursor, 2> cursors{};
    std::array<std::vector<modding::Handle>, 2> voices{};
    modding::gunfx::Motion motion;
    std::uint16_t item{0xFFFFU};
    float kick_roll{};
    bool shot{};
};

// A dropped part: a falling, tumbling copy of its triangles in the world.
struct GunfxProp final
{
    StaticModelAsset *model{};
    std::uint16_t item{};
    std::vector<std::vector<std::uint32_t>> triangles;
    std::shared_ptr<const modding::graphics::Model> own;
    // Model space -> world, rotation only, relative to the pivot.
    rig::Matrix shape{rig::identity()};
    world::Vec3 position{};
    world::Vec3 velocity{};
    world::Vec3 spin{};
    rig::Quat turn{};
    float radius{0.05F};
    // How far below `position` its lowest point is, as last drawn, and the
    // top of the block it came to rest on.
    float depth{0.05F};
    float ground{};
    float age{};
    float life{5.0F};
    bool resting{};
};

modding::graphics::GraphicsService *gunfx_service_{};
std::unordered_map<std::string, GunfxCut> gunfx_cuts_;
std::uint64_t gunfx_revision_{};
std::shared_ptr<const modding::gunfx::TrackTable> gunfx_tracks_;
// Registrations a pack added as variants ("PistolReloadEmpty", "X@rpg").
std::unordered_map<std::string, const rig::Registration *> gunfx_variants_;
std::map<std::string, rig::Matrix, std::less<>> gunfx_grips_;
std::unique_ptr<rig::CharacterPose> gunfx_scratch_;
GunfxCharacter gunfx_self_;
std::unordered_map<std::uint16_t, GunfxCharacter> gunfx_others_;
std::vector<GunfxProp> gunfx_props_;
GunfxFrame gunfx_frame_storage_;
const GunfxFrame *gunfx_frame_{};
std::vector<modding::gunfx::Cue> gunfx_pending_drops_;
bool gunfx_reload_empty_{};
bool gunfx_reduced_motion_{};
float gunfx_world_fov_{};
std::uint32_t gunfx_random_{0x2545F491U};
int gunfx_diagnostic_{-1};
std::vector<std::string> gunfx_reported_;

[[nodiscard]] const modding::gunfx::Library *gunfx_library() const noexcept
{
    return gunfx_service_ != nullptr && !gunfx_service_->gunfx().empty() ? &gunfx_service_->gunfx() : nullptr;
}

// OPENMINERZ_DIAGNOSTIC_GUNFX=parts: every part tinted its own colour, and
// (also with =log) every cue that fires is logged.
[[nodiscard]] int gunfx_diagnostic()
{
    if (gunfx_diagnostic_ < 0)
    {
        const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_GUNFX");
        gunfx_diagnostic_ = value == nullptr ? 0 : std::string_view{value} == "parts" ? 1 : 2;
    }
    return gunfx_diagnostic_;
}
[[nodiscard]] bool gunfx_tint_parts()
{
    return gunfx_diagnostic() == 1;
}

void gunfx_report(std::string text)
{
    if (std::find(gunfx_reported_.begin(), gunfx_reported_.end(), text) != gunfx_reported_.end())
        return;
    std::cerr << "GunFX: " << text << '\n';
    gunfx_reported_.push_back(std::move(text));
}

// The pack's registrations changed (the character loaded or the packs did):
// variants, grips and caches start again.
void gunfx_pack_changed()
{
    gunfx_variants_.clear();
    gunfx_grips_.clear();
    gunfx_scratch_.reset();
    gunfx_self_ = {};
    gunfx_others_.clear();
    gunfx_tracks_ = modding::gunfx::installed_tracks();
    if (retail_rig_ == nullptr)
        return;
    for (const rig::Registration &registration : retail_rig_->registrations)
        if (modding::gunfx::split_clip_name(registration.name).base != registration.name)
            gunfx_variants_[registration.name] = &registration;
}

// Follows the library and the installed tracks; caches go when either moves.
void gunfx_follow()
{
    const modding::gunfx::Library *library = gunfx_library();
    const std::uint64_t revision = library != nullptr ? library->revision() : 0U;
    auto tracks = modding::gunfx::installed_tracks();
    if (revision == gunfx_revision_ && tracks == gunfx_tracks_)
        return;
    gunfx_revision_ = revision;
    gunfx_tracks_ = std::move(tracks);
    gunfx_cuts_.clear();
    gunfx_grips_.clear();
}

[[nodiscard]] const modding::gunfx::ClipTracks *gunfx_clip_tracks(const rig::Registration *registration) const
{
    if (registration == nullptr || gunfx_tracks_ == nullptr)
        return nullptr;
    const auto found = gunfx_tracks_->find(registration->name);
    return found != gunfx_tracks_->end() ? &found->second : nullptr;
}

[[nodiscard]] std::int64_t gunfx_duration(const rig::Registration *registration) const noexcept
{
    if (registration == nullptr || registration->clip < 0 ||
        static_cast<std::size_t>(registration->clip) >= retail_rig_->clips.size())
        return 0;
    return retail_rig_->clips[static_cast<std::size_t>(registration->clip)].duration_ticks;
}

// The cue list a registration plays with `item` in hand: the first of its
// variant names that has one (an Empty reload, the model's own, the clip).
[[nodiscard]] const modding::gunfx::ClipCues *gunfx_cues(const rig::Registration *registration,
                                                         std::uint16_t item, bool empty) const
{
    const modding::gunfx::Library *library = gunfx_library();
    if (library == nullptr || registration == nullptr || !library->any_cues())
        return nullptr;
    const std::string_view model = modding::gunfx::held_model_name(item);
    const modding::gunfx::ClipName name = modding::gunfx::split_clip_name(registration->name);
    for (const std::string &candidate :
         modding::gunfx::variant_names(name.base, model, empty || name.empty, retail_rig_->generation))
        if (const modding::gunfx::ClipCues *list = library->cues(candidate, model))
            return list;
    return nullptr;
}

// Channel 2 and 3 clips by variant: an empty reload (yours only) and the held
// model's own clips. Then, with motion on, the Shoulder clip eased.
void gunfx_adjust(rig::ChannelStates &states, std::uint16_t item, bool local)
{
    if (gunfx_library() == nullptr && gunfx_variants_.empty())
        return;
    // Reproducible pose review, including melee and grenades. This freezes
    // only the rendered pose; it never advances a weapon or gameplay event.
    // Seconds are on the registered clip clock (reloads retain retail timing).
    // OPENMINERZ_DIAGNOSTIC_GUNFX_POSE=GunReload,0.5
    if (const char *value = SDL_getenv("OPENMINERZ_DIAGNOSTIC_GUNFX_POSE"); value != nullptr)
    {
        const std::string_view text{value};
        const auto comma = text.find(',');
        const auto *clip = retail_rig_->find(text.substr(0, comma));
        if (clip != nullptr)
        {
            const double seconds = comma != std::string_view::npos ? std::atof(value + comma + 1U) : 0.0;
            const double duration = rig::seconds_from_ticks(gunfx_duration(clip));
            states[2] = {};
            states[3] = {};
            states[2].current = clip;
            states[2].current_position = rig::ticks_from_seconds(std::clamp(seconds, 0.0, duration));
            states[2].blend = 1.0F;
        }
    }
    if (!gunfx_variants_.empty())
    {
        const std::string_view model = modding::gunfx::held_model_name(item);
        const auto swap = [&](const rig::Registration *&registration, bool empty_reload) {
            if (registration == nullptr)
                return;
            const modding::gunfx::ClipName name = modding::gunfx::split_clip_name(registration->name);
            const bool empty = empty_reload && modding::gunfx::reload_clip(name.base);
            for (const std::string &candidate :
                 modding::gunfx::variant_names(name.base, model, empty, retail_rig_->generation))
            {
                if (candidate == name.base)
                    break;
                if (const auto found = gunfx_variants_.find(candidate); found != gunfx_variants_.end())
                {
                    registration = found->second;
                    return;
                }
            }
        };
        for (const std::size_t channel : {2U, 3U})
        {
            swap(states[channel].current, local && gunfx_reload_empty_);
            swap(states[channel].previous, local && gunfx_reload_empty_);
        }
    }
    // (Motion) the sights come up eased instead of linearly.
    const modding::gunfx::Library *library = gunfx_library();
    const modding::gunfx::MotionSettings *motion = library != nullptr && local ? library->motion() : nullptr;
    rig::ChannelState &upper = states[2];
    if (motion == nullptr || upper.current == nullptr ||
        !modding::gunfx::split_clip_name(upper.current->name).base.ends_with("Shoulder"))
        return;
    const auto family = modding::gunfx::family_of(item_player_animation_mode(static_cast<InventoryItemId>(item)));
    const float amount = motion->tuning(family).aim_ease;
    const std::int64_t duration = gunfx_duration(upper.current);
    if (duration <= 0 || amount <= 0.0F)
        return;
    const float share = static_cast<float>(static_cast<double>(upper.current_position) / static_cast<double>(duration));
    upper.current_position = static_cast<std::int64_t>(
        std::llround(static_cast<double>(modding::gunfx::Motion::ease_aim(share, amount)) * static_cast<double>(duration)));
}

// The parts of a held model, cut once per definition from the model's own
// triangles (model space: after each mesh's transform).
[[nodiscard]] const GunfxCut *gunfx_cut(std::uint16_t item, StaticModelAsset *model)
{
    const modding::gunfx::Library *library = gunfx_library();
    if (library == nullptr || model == nullptr || !model->loaded() || retail_rig_ == nullptr)
        return nullptr;
    const std::string_view stem = modding::gunfx::held_model_name(item);
    const std::vector<modding::gunfx::PartSpec> *specs = library->parts(retail_rig_->generation, stem);
    if (specs == nullptr || specs->empty())
        return nullptr;
    const std::string key = retail_rig_->generation + "/" + std::string{stem};
    auto found = gunfx_cuts_.find(key);
    if (found != gunfx_cuts_.end() && found->second.revision == library->revision())
        return &found->second;
    GunfxCut cut;
    cut.revision = library->revision();
    cut.specs = *specs;
    std::vector<std::vector<std::array<float, 3>>> positions;
    std::vector<std::vector<std::uint32_t>> indices;
    for (const StaticModelPart &part : model->parts())
    {
        std::vector<std::array<float, 3>> moved;
        moved.reserve(part.vertices.size());
        for (const StaticModelVertex &vertex : part.vertices)
            moved.push_back(rig::transform_point(vertex.position.data(), part.transform));
        positions.push_back(std::move(moved));
        indices.push_back(part.indices);
    }
    std::vector<modding::gunfx::MeshTriangles> meshes;
    for (std::size_t mesh = 0U; mesh < positions.size(); ++mesh)
        meshes.push_back({positions[mesh], indices[mesh]});
    cut.cut = modding::gunfx::cut_parts(cut.specs, meshes);
    cut.own.resize(cut.specs.size());
    for (std::size_t part = 0U; part < cut.specs.size(); ++part)
    {
        const modding::gunfx::PartSpec &spec = cut.specs[part];
        if (!spec.model.empty())
        {
            std::string error;
            cut.own[part] = gunfx_service_->model_cache().get(gunfx_service_->assets(), spec.model, &error);
            if (cut.own[part] == nullptr)
                gunfx_report(std::format("{} part \"{}\": {}", key, spec.name, error));
        }
        else if (cut.cut.parts[part].count == 0U)
            gunfx_report(std::format("{} part \"{}\" selects no triangle", key, spec.name));
    }
    std::cerr << "GunFX: " << key << " cut into " << cut.specs.size() << " part(s)\n";
    return &(gunfx_cuts_[key] = std::move(cut));
}

// Where a part was on the gun (model space -> avatar) at another moment of
// the clip on `channel`, against the bone it grabs then: its grip on the bone.
[[nodiscard]] rig::Matrix gunfx_grip(const rig::ChannelStates &states, std::size_t channel, std::int64_t at,
                                     const modding::gunfx::PartTrack *track, modding::gunfx::Vec3 pivot,
                                     const rig::Matrix &hand, int bone, const std::string &key)
{
    if (const auto found = gunfx_grips_.find(key); found != gunfx_grips_.end())
        return found->second;
    if (gunfx_scratch_ == nullptr)
        gunfx_scratch_ = std::make_unique<rig::CharacterPose>(*retail_rig_);
    rig::ChannelStates then = states;
    then[channel].current_position = at;
    then[channel].previous = nullptr;
    then[channel].blend = 1.0F;
    gunfx_scratch_->apply(then);
    const rig::Matrix placed = rig::multiply(rig::multiply(modding::gunfx::part_motion(track, at, pivot), hand),
                                             gunfx_scratch_->right_hand());
    const rig::Matrix grip =
        rig::multiply(placed, rig::invert(gunfx_scratch_->attachment()[static_cast<std::size_t>(bone)]));
    gunfx_grips_.emplace(key, grip);
    return grip;
}

// Channels 2 and 3 as the pose shows them: the caller's states, or the
// built-in driver's players.
[[nodiscard]] static rig::ChannelStates gunfx_states_of(rig::CharacterPose &pose)
{
    rig::ChannelStates states{};
    for (const int channel : {2, 3})
        if (const rig::ClipPlayer *player = pose.animations.active(channel))
        {
            states[static_cast<std::size_t>(channel)].current = player->registration();
            states[static_cast<std::size_t>(channel)].current_position = player->position_ticks();
        }
    return states;
}

// This frame's part state for one character (nullptr: draw the held model as
// it always is).
[[nodiscard]] const GunfxFrame *gunfx_prepare(const rig::ChannelStates &states, std::uint16_t item, bool local)
{
    gunfx_follow();
    StaticModelAsset *model = retail_held_model(item);
    const GunfxCut *cut = gunfx_cut(item, model);
    if (cut == nullptr)
        return nullptr;
    GunfxFrame &frame = gunfx_frame_storage_;
    const std::size_t count = cut->specs.size();
    frame.cut = cut;
    frame.model = model;
    frame.item = item;
    frame.visible.assign(count, true);
    frame.motion.assign(count, rig::identity());
    frame.bone.assign(count, -1);
    frame.grip.assign(count, rig::identity());
    frame.carried.assign(count, 0.0F);
    frame.placement.assign(count, rig::identity());
    // A grip is taken as the first-person view draws the gun (the hand
    // transform the pack was authored against), in either view: another
    // player's carried part sits in their palm as it does in yours, though
    // retail holds their gun a little further out.
    const rig::HeldItem *entry = retail_rig_->item(item);
    const rig::Matrix hand = entry != nullptr ? entry->hand : rig::identity();
    const auto bone_index = [&](std::string_view name) {
        return modding::graphics::find_rig_bone(*retail_rig_, name);
    };
    std::vector<modding::gunfx::PartState> item_states;
    std::vector<modding::gunfx::PartState> use_states;
    std::array<const modding::gunfx::ClipCues *, 2> lists{};
    std::array<const modding::gunfx::ClipTracks *, 2> tracks{};
    std::array<std::int64_t, 2> durations{};
    for (std::size_t slot = 0U; slot < 2U; ++slot)
    {
        const rig::ChannelState &channel = states[2U + slot];
        lists[slot] = gunfx_cues(channel.current, item, local && gunfx_reload_empty_ && slot == 0U);
        tracks[slot] = gunfx_clip_tracks(channel.current);
        durations[slot] = gunfx_duration(channel.current);
        modding::gunfx::part_states(cut->specs, lists[slot], tracks[slot], durations[slot], channel.current_position,
                                    bone_index, slot == 0U ? item_states : use_states);
    }
    for (std::size_t part = 0U; part < count; ++part)
    {
        const modding::gunfx::PartSpec &spec = cut->specs[part];
        // The use channel (a shot) owns a part it moves or cues.
        std::size_t slot = 0U;
        if (tracks[1] != nullptr && tracks[1]->find(spec.name) != nullptr)
            slot = 1U;
        if (lists[1] != nullptr && std::any_of(lists[1]->cues.begin(), lists[1]->cues.end(),
                                               [&](const modding::gunfx::Cue &cue) { return cue.part == spec.name; }))
            slot = 1U;
        const modding::gunfx::PartState &state = slot == 0U ? item_states[part] : use_states[part];
        const rig::ChannelState &channel = states[2U + slot];
        const modding::gunfx::PartTrack *track = tracks[slot] != nullptr ? tracks[slot]->find(spec.name) : nullptr;
        const modding::gunfx::Vec3 pivot = cut->cut.parts[part].pivot;
        frame.visible[part] = state.visible;
        frame.motion[part] = modding::gunfx::part_motion(track, channel.current_position, pivot);
        if (state.bone >= 0 && state.attach_cue >= 0 && lists[slot] != nullptr)
        {
            const modding::gunfx::Cue &cue = lists[slot]->cues[static_cast<std::size_t>(state.attach_cue)];
            const std::int64_t at = modding::gunfx::cue_ticks(cue, *lists[slot], tracks[slot], durations[slot]);
            const std::string key = std::format("{}|{}|{}|{}", channel.current->name, state.attach_cue, part, item);
            frame.bone[part] = state.bone;
            frame.grip[part] = gunfx_grip(states, 2U + slot, at, track, pivot, hand, state.bone, key);
            frame.carried[part] = state.detach_cue >= 0 ? 1.0F - state.detach_weight : state.attach_weight;
        }
    }
    if (gunfx_diagnostic() != 0 && ++gunfx_prepare_logged_ % 30U == 0U)
        for (std::size_t part = 0U; part < count; ++part)
            std::cerr << std::format("GunFX: {} {} part {} visible={} bone={} carried={:.2f} moved={:.3f},{:.3f},{:.3f}\n",
                                     local ? "self" : "other",
                                     states[2].current != nullptr ? states[2].current->name : std::string{"-"},
                                     cut->specs[part].name, frame.visible[part] ? 1 : 0, frame.bone[part],
                                     frame.carried[part], frame.motion[part][12], frame.motion[part][13],
                                     frame.motion[part][14]);
    return &frame;
}

// Blends two placements: translation linearly, rotation and scale through
// the rows (good for the short ease of a detach).
[[nodiscard]] static rig::Matrix gunfx_blend(const rig::Matrix &from, const rig::Matrix &to, float weight) noexcept
{
    rig::Matrix result{};
    for (std::size_t index = 0U; index < 16U; ++index)
        result[index] = from[index] + (to[index] - from[index]) * weight;
    // Keep the rotation rows orthonormal-ish: renormalise each to the length
    // the blend of their lengths has.
    for (std::size_t row = 0U; row < 3U; ++row)
    {
        const float target = std::sqrt(from[row * 4U] * from[row * 4U] + from[row * 4U + 1U] * from[row * 4U + 1U] +
                                       from[row * 4U + 2U] * from[row * 4U + 2U]) *
                                 (1.0F - weight) +
                             std::sqrt(to[row * 4U] * to[row * 4U] + to[row * 4U + 1U] * to[row * 4U + 1U] +
                                       to[row * 4U + 2U] * to[row * 4U + 2U]) *
                                 weight;
        const float length = std::sqrt(result[row * 4U] * result[row * 4U] + result[row * 4U + 1U] * result[row * 4U + 1U] +
                                       result[row * 4U + 2U] * result[row * 4U + 2U]);
        if (length > 1.0e-6F)
            for (std::size_t column = 0U; column < 3U; ++column)
                result[row * 4U + column] *= target / length;
    }
    return result;
}

// build_retail_geometry's held model with parts: the body's triangles under
// the item's own transform, each visible part's under its placement.
void gunfx_append_model(const GunfxFrame &frame, const rig::CharacterPose &pose, const rig::Matrix &item_parent,
                        const RetailHeldItem &held)
{
    GunfxFrame &placed = const_cast<GunfxFrame &>(frame);
    const std::size_t count = frame.cut->specs.size();
    for (std::size_t part = 0U; part < count; ++part)
    {
        const rig::Matrix on_gun = rig::multiply(frame.motion[part], item_parent);
        if (frame.bone[part] >= 0)
        {
            const rig::Matrix carried =
                rig::multiply(frame.grip[part], pose.attachment()[static_cast<std::size_t>(frame.bone[part])]);
            placed.placement[part] = frame.carried[part] >= 1.0F ? carried : gunfx_blend(on_gun, carried, frame.carried[part]);
        }
        else
            placed.placement[part] = on_gun;
    }
    std::array<float, 3> primary{};
    std::array<float, 3> secondary{};
    retail_item_tints(held.item, primary, secondary);
    const bool gray_launcher = held.item == 104U || held.item == 113U;
    const bool tint_parts = gunfx_tint_parts();
    static constexpr std::array<std::array<float, 3>, 6> diagnostic{
        {{1.0F, 0.25F, 0.2F}, {0.25F, 1.0F, 0.3F}, {0.3F, 0.5F, 1.0F}, {1.0F, 0.9F, 0.2F}, {1.0F, 0.3F, 1.0F}, {0.2F, 1.0F, 1.0F}}};
    const auto &parts = frame.model->parts();
    for (std::size_t mesh = 0U; mesh < parts.size(); ++mesh)
    {
        const StaticModelPart &part = parts[mesh];
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
        constexpr float color_scale = 1.0F / 255.0F;
        const auto emit = [&](std::size_t triangle, const rig::Matrix &placement, const std::array<float, 3> *paint) {
            const rig::Matrix world = rig::multiply(part.transform, placement);
            for (std::size_t corner = 0U; corner < 3U; ++corner)
            {
                const std::uint32_t index = part.indices[triangle * 3U + corner];
                if (index >= part.vertices.size())
                    return;
            }
            for (std::size_t corner = 0U; corner < 3U; ++corner)
            {
                const StaticModelVertex &source = part.vertices[part.indices[triangle * 3U + corner]];
                RetailVertex vertex;
                vertex.position = rig::transform_point(source.position.data(), world);
                vertex.normal = rig::transform_vector(source.normal.data(), world);
                vertex.uv = source.uv;
                std::array<float, 3> colour = tint;
                if (paint != nullptr)
                    colour = {tint[0] * (*paint)[0], tint[1] * (*paint)[1], tint[2] * (*paint)[2]};
                vertex.color = {part.diffuse[0] * source.color[0] * color_scale * colour[0],
                                part.diffuse[1] * source.color[1] * color_scale * colour[1],
                                part.diffuse[2] * source.color[2] * color_scale * colour[2], source.color[3] * color_scale};
                retail_vertices_.push_back(vertex);
            }
        };
        const std::vector<int> &owner = frame.cut->cut.owner[mesh];
        for (std::size_t triangle = 0U; triangle * 3U + 2U < part.indices.size(); ++triangle)
            if (triangle >= owner.size() || owner[triangle] < 0)
                emit(triangle, item_parent, nullptr);
        for (std::size_t index = 0U; index < count; ++index)
        {
            if (!frame.visible[index] || mesh >= frame.cut->cut.parts[index].triangles.size())
                continue;
            const auto *paint = tint_parts ? &diagnostic[index % diagnostic.size()] : nullptr;
            for (const std::uint32_t triangle : frame.cut->cut.parts[index].triangles[mesh])
                emit(triangle, frame.placement[index], paint);
        }
        span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
        if (span.count > 0U)
            retail_spans_.push_back(span);
    }
    // The pack's own meshes.
    for (std::size_t index = 0U; index < count; ++index)
    {
        if (!frame.visible[index] || frame.cut->own[index] == nullptr)
            continue;
        append_gunfx_own(frame.cut->own[index], frame.cut->specs[index], frame.placement[index]);
    }
}

// A pack mesh part, in avatar space: its origin at the part's pivot, turned
// and scaled as declared, then placed like any part.
void append_gunfx_own(const std::shared_ptr<const modding::graphics::Model> &shared,
                      const modding::gunfx::PartSpec &spec, const rig::Matrix &placement)
{
    namespace graphics = modding::graphics;
    const graphics::Model &model = *shared;
    graphics::pose_item_model(model, {}, held_item_parts_, held_item_scratch_);
    constexpr float degrees = std::numbers::pi_v<float> / 180.0F;
    const modding::gunfx::Vec3 pivot = spec.pivot.value_or(modding::gunfx::Vec3{});
    // XNA CreateFromYawPitchRoll order is not what an author means by XYZ
    // degrees: turn about X, then Y, then Z.
    const auto turn = [](float angle, int axis) {
        rig::Matrix m = rig::identity();
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const int a = (axis + 1) % 3;
        const int b = (axis + 2) % 3;
        m[static_cast<std::size_t>(a * 4 + a)] = c;
        m[static_cast<std::size_t>(a * 4 + b)] = s;
        m[static_cast<std::size_t>(b * 4 + a)] = -s;
        m[static_cast<std::size_t>(b * 4 + b)] = c;
        return m;
    };
    rig::Matrix local = rig::multiply(rig::multiply(turn(spec.rotation.x * degrees, 0), turn(spec.rotation.y * degrees, 1)),
                                      turn(spec.rotation.z * degrees, 2));
    for (std::size_t row = 0U; row < 3U; ++row)
        for (std::size_t column = 0U; column < 3U; ++column)
            local[row * 4U + column] *= spec.scale;
    local[12] = pivot.x;
    local[13] = pivot.y;
    local[14] = pivot.z;
    const rig::Matrix world = rig::multiply(local, placement);
    for (const graphics::PosedItemPart &part : held_item_parts_)
    {
        if (part.vertices.empty())
            continue;
        SDL_Texture *texture = white_texture_;
        if (part.material >= 0 && static_cast<std::size_t>(part.material) < model.materials.size() &&
            mod_material_textures_)
            if (SDL_Texture *own = mod_material_textures_(shared, model.materials[static_cast<std::size_t>(part.material)]))
                texture = own;
        const std::array<float, 4> color = graphics::item_material_color(model, part.material);
        RetailSpan span;
        span.texture = texture;
        span.first = static_cast<std::uint32_t>(retail_vertices_.size());
        span.material = 0.12F;
        span.item = true;
        span.vertex_albedo = true;
        for (const graphics::PosedItemVertex &source : part.vertices)
        {
            const std::array<float, 3> position{source.position.x, source.position.y, source.position.z};
            const std::array<float, 3> normal{source.normal.x, source.normal.y, source.normal.z};
            RetailVertex vertex;
            vertex.position = rig::transform_point(position.data(), world);
            vertex.normal = rig::transform_vector(normal.data(), world);
            vertex.uv = source.uv;
            vertex.color = color;
            retail_vertices_.push_back(vertex);
        }
        span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
        retail_spans_.push_back(span);
    }
}

[[nodiscard]] float gunfx_random() noexcept
{
    gunfx_random_ ^= gunfx_random_ << 13U;
    gunfx_random_ ^= gunfx_random_ >> 17U;
    gunfx_random_ ^= gunfx_random_ << 5U;
    return static_cast<float>(gunfx_random_ & 0xFFFFU) / 32767.5F - 1.0F;
}

// The cues this frame passes on channels 2 and 3, for one character. `self`:
// your own first person (sounds 2D, kicks); otherwise another player (sounds
// at `hands`, quieter). Drops wait in gunfx_pending_drops_ until the frame's
// view is known (gunfx_flush_drops).
void gunfx_fire(GunfxCharacter &character, const rig::ChannelStates &states, std::uint16_t item, bool self,
                world::Vec3 hands)
{
    const modding::gunfx::Library *library = gunfx_library();
    character.shot = false;
    if (library == nullptr)
        return;
    if (character.item != item)
    {
        // Another item: whatever the last one's clips were doing is over.
        for (std::size_t slot = 0U; slot < 2U; ++slot)
        {
            for (const modding::Handle voice : character.voices[slot])
                gunfx_service_->stop_sound(voice, 0.06F);
            character.voices[slot].clear();
            character.cursors[slot].reset();
        }
    }
    for (std::size_t slot = 0U; slot < 2U; ++slot)
    {
        const rig::ChannelState &channel = states[2U + slot];
        const rig::Registration *registration = channel.current;
        const modding::gunfx::ClipCues *list = gunfx_cues(registration, item, self && gunfx_reload_empty_ && slot == 0U);
        const modding::gunfx::ClipTracks *tracks = gunfx_clip_tracks(registration);
        const std::int64_t duration = gunfx_duration(registration);
        const bool looping = registration != nullptr && registration->looping && slot == 0U;
        const modding::gunfx::CueCursor::Step step =
            character.cursors[slot].advance(registration, channel.current_position, duration, looping, list, tracks);
        if (step.cut)
            for (const modding::Handle voice : character.voices[slot])
                gunfx_service_->stop_sound(voice, 0.06F);
        if (step.cut || step.ended || step.restarted)
            character.voices[slot].clear();
        if (slot == 1U && step.restarted && registration != nullptr &&
            modding::gunfx::split_clip_name(registration->name).base.ends_with("Shoot"))
            character.shot = true;
        if (gunfx_diagnostic() != 0 && (step.cut || (step.restarted && !looping)))
            std::cerr << "GunFX: " << (self ? "self" : "other") << " channel " << slot + 2U << ' '
                      << (step.cut ? "cut short" : "starts") << ' '
                      << (registration != nullptr ? registration->name : std::string{"-"}) << '\n';
        for (const int index : step.fired)
        {
            const modding::gunfx::Cue &cue = list->cues[static_cast<std::size_t>(index)];
            if (gunfx_diagnostic() != 0)
                std::cerr << "GunFX: " << (self ? "self" : "other") << ' ' << registration->name << " cue " << index
                          << " at " << rig::seconds_from_ticks(channel.current_position) << " s: "
                          << (cue.sound.empty() ? cue.part : cue.sound) << '\n';
            if ((cue.who == modding::gunfx::CueAudience::self && !self) ||
                (cue.who == modding::gunfx::CueAudience::others && self))
                continue;
            switch (cue.action)
            {
            case modding::gunfx::CueAction::sound:
            {
                modding::graphics::VoiceParams params;
                params.volume = std::max(0.0F, cue.volume * (1.0F + cue.volume_jitter * gunfx_random()));
                params.pitch = std::clamp(cue.pitch * (1.0F + cue.pitch_jitter * gunfx_random()), 0.25F, 4.0F);
                if (!self)
                {
                    params.volume *= list->remote_volume;
                    params.position = modding::graphics::Vec3{hands.x, hands.y, hands.z};
                    params.min_distance = 1.5F;
                    params.max_distance = list->remote_distance;
                }
                try
                {
                    const modding::Handle voice =
                        gunfx_service_->play_sound(list->mod_id, cue.sound, params, std::nullopt);
                    if (voice.valid())
                        character.voices[slot].push_back(voice);
                }
                catch (const modding::ScriptError &error)
                {
                    // A sound the pack does not have: said once, then quiet.
                    gunfx_report(std::format("{} cue sound \"{}\": {}", list->clip, cue.sound, error.what()));
                }
                break;
            }
            case modding::gunfx::CueAction::kick:
                if (self)
                {
                    gunfx_service_->camera().kick(cue.kick.x, cue.kick.y, cue.kick_seconds);
                    character.kick_roll += cue.kick.z;
                }
                break;
            case modding::gunfx::CueAction::drop:
                gunfx_pending_drops_.push_back(cue);
                break;
            default:
                break; // part state is a function of the clip time
            }
        }
    }
    character.item = item;
}

// The parts this frame dropped leave the gun: props where they are drawn
// now, falling. `to_world` maps an avatar-space placement to the world
// (rotation only, and where its origin lands); `carry` is the character's
// own velocity.
void gunfx_flush_drops(std::uint16_t item, world::Vec3 carry,
                       const std::function<void(const rig::Matrix &, rig::Matrix &, world::Vec3 &)> &to_world)
{
    for (const modding::gunfx::Cue &cue : gunfx_pending_drops_)
        gunfx_drop(cue, item, carry, to_world);
    gunfx_pending_drops_.clear();
}

void gunfx_drop(const modding::gunfx::Cue &cue, std::uint16_t item, world::Vec3 carry,
                const std::function<void(const rig::Matrix &, rig::Matrix &, world::Vec3 &)> &to_world)
{
    const GunfxFrame &frame = gunfx_frame_storage_;
    if (frame.cut == nullptr || frame.item != item || !to_world)
        return;
    std::size_t part = 0U;
    while (part < frame.cut->specs.size() && frame.cut->specs[part].name != cue.part)
        ++part;
    if (part >= frame.cut->specs.size() || part >= frame.placement.size())
        return;
    const modding::gunfx::CutPart &cut = frame.cut->cut.parts[part];
    if (cut.count == 0U && frame.cut->own[part] == nullptr)
        return;
    GunfxProp prop;
    prop.model = frame.model;
    prop.item = item;
    prop.triangles = cut.triangles;
    prop.own = frame.cut->own[part];
    const modding::gunfx::Vec3 pivot = cut.pivot;
    // Model space about the pivot -> avatar -> world (rotation only).
    rig::Matrix linear{};
    world::Vec3 origin{};
    const std::array<float, 3> pivot_avatar =
        rig::transform_point(std::array<float, 3>{pivot.x, pivot.y, pivot.z}.data(), frame.placement[part]);
    rig::Matrix about = frame.placement[part];
    about[12] = about[13] = about[14] = 0.0F;
    rig::Matrix avatar_pivot = rig::identity();
    avatar_pivot[12] = pivot_avatar[0];
    avatar_pivot[13] = pivot_avatar[1];
    avatar_pivot[14] = pivot_avatar[2];
    to_world(avatar_pivot, linear, origin);
    rig::Matrix shape = rig::multiply(about, linear);
    // Undo the pivot first: vertex (model space) - pivot, then turned.
    rig::Matrix shift = rig::identity();
    shift[12] = -pivot.x;
    shift[13] = -pivot.y;
    shift[14] = -pivot.z;
    prop.shape = rig::multiply(shift, shape);
    prop.position = origin;
    // The cue's velocity is in the player's frame: x right, y up, -z ahead.
    const std::array<float, 3> push = rig::transform_vector(
        std::array<float, 3>{cue.velocity.x, cue.velocity.y, cue.velocity.z}.data(), linear);
    prop.velocity = {carry.x + push[0], carry.y + push[1], carry.z + push[2]};
    const std::array<float, 3> spin =
        rig::transform_vector(std::array<float, 3>{cue.spin.x, cue.spin.y, cue.spin.z}.data(), linear);
    prop.spin = {spin[0], spin[1], spin[2]};
    prop.life = cue.life;
    const modding::gunfx::Vec3 extent = (cut.bounds_max - cut.bounds_min) * 0.5F;
    prop.radius = std::clamp(std::max({extent.x, extent.y, extent.z}) * 0.6F, 0.01F, 0.3F);
    prop.depth = prop.radius;
    if (gunfx_props_.size() >= 32U)
        gunfx_props_.erase(gunfx_props_.begin());
    if (gunfx_diagnostic() != 0)
        std::cerr << std::format("GunFX: dropped {} at {:.2f} {:.2f} {:.2f} moving {:.2f} {:.2f} {:.2f}\n", cue.part,
                                 prop.position.x, prop.position.y, prop.position.z, prop.velocity.x, prop.velocity.y,
                                 prop.velocity.z);
    gunfx_props_.push_back(std::move(prop));
}

// Props fall, bounce on solid blocks, settle and fade.
void gunfx_step_props(float seconds, const std::function<bool(int, int, int)> &solid)
{
    constexpr float gravity = 16.0F;
    for (GunfxProp &prop : gunfx_props_)
    {
        prop.age += seconds;
        if (prop.resting)
            continue;
        prop.velocity.y -= gravity * seconds;
        // Air drag, a little.
        const float drag = std::exp(-0.4F * seconds);
        prop.velocity = {prop.velocity.x * drag, prop.velocity.y, prop.velocity.z * drag};
        prop.position = {prop.position.x + prop.velocity.x * seconds, prop.position.y + prop.velocity.y * seconds,
                         prop.position.z + prop.velocity.z * seconds};
        const float spin = std::sqrt(prop.spin.x * prop.spin.x + prop.spin.y * prop.spin.y + prop.spin.z * prop.spin.z);
        if (spin > 1.0e-4F)
        {
            const float angle = spin * seconds * 0.5F;
            const float s = std::sin(angle) / spin;
            const rig::Quat step{prop.spin.x * s, prop.spin.y * s, prop.spin.z * s, std::cos(angle)};
            const rig::Quat &q = prop.turn;
            prop.turn = {step.w * q.x + step.x * q.w + step.y * q.z - step.z * q.y,
                         step.w * q.y - step.x * q.z + step.y * q.w + step.z * q.x,
                         step.w * q.z + step.x * q.y - step.y * q.x + step.z * q.w,
                         step.w * q.w - step.x * q.x - step.y * q.y - step.z * q.z};
            const float length =
                std::sqrt(prop.turn.x * prop.turn.x + prop.turn.y * prop.turn.y + prop.turn.z * prop.turn.z + prop.turn.w * prop.turn.w);
            if (length > 1.0e-6F)
                prop.turn = {prop.turn.x / length, prop.turn.y / length, prop.turn.z / length, prop.turn.w / length};
        }
        if (!solid)
            continue;
        const int x = static_cast<int>(std::floor(prop.position.x));
        const int z = static_cast<int>(std::floor(prop.position.z));
        const int below = static_cast<int>(std::floor(prop.position.y - prop.depth));
        if (prop.velocity.y < 0.0F && solid(x, below, z))
        {
            prop.ground = static_cast<float>(below + 1);
            prop.position.y = prop.ground + prop.depth;
            prop.velocity = {prop.velocity.x * 0.55F, -prop.velocity.y * 0.28F, prop.velocity.z * 0.55F};
            prop.spin = {prop.spin.x * 0.45F, prop.spin.y * 0.45F, prop.spin.z * 0.45F};
            if (std::abs(prop.velocity.y) < 0.6F)
                prop.resting = true;
        }
    }
    std::erase_if(gunfx_props_, [](const GunfxProp &prop) { return prop.age >= prop.life; });
}

// (Motion) The viewmodel camera's offset this frame (identity without
// `gunfx.motion`): the look sway, bob, roll, sprint pose, jump and landing,
// breathing, the recoil of a shot and the lowered start of a new item.
[[nodiscard]] rig::Matrix gunfx_motion_offset(const world::PlayerState &player, float seconds)
{
    const modding::gunfx::Library *library = gunfx_library();
    const modding::gunfx::MotionSettings *settings = library != nullptr ? library->motion() : nullptr;
    GunfxCharacter &self = gunfx_self_;
    if (settings == nullptr)
    {
        self.motion.reset();
        self.kick_roll = 0.0F;
        gunfx_motion_item_ = 0xFFFFU;
        return rig::identity();
    }
    modding::gunfx::MotionInput input;
    input.seconds = seconds;
    input.yaw = player.yaw;
    input.pitch = player.pitch;
    input.velocity = {player.velocity.x, player.velocity.y, player.velocity.z};
    input.grounded = player.grounded;
    input.sprinting = player.sprinting;
    input.flying = player.fly_mode;
    switch (player.viewmodel_cue)
    {
    case world::ViewmodelCue::shoulder_idle:
    case world::ViewmodelCue::shoulder_use:
        input.aim = 1.0F;
        break;
    case world::ViewmodelCue::shoulder:
        input.aim = player.viewmodel_cue_progress;
        break;
    case world::ViewmodelCue::unshoulder:
        input.aim = 1.0F - player.viewmodel_cue_progress;
        break;
    default:
        break;
    }
    input.shot = self.shot;
    input.switched = gunfx_motion_item_ != 0xFFFFU && gunfx_motion_item_ != player.selected_item_id;
    gunfx_motion_item_ = player.selected_item_id;
    input.reduced_motion = gunfx_reduced_motion_;
    input.family = held_mod_item_ != nullptr && !held_mod_item_->look.empty()
                       ? modding::gunfx::Family::generic
                       : modding::gunfx::family_of(
                             item_player_animation_mode(static_cast<InventoryItemId>(player.selected_item_id)));
    input.kick_roll = std::exchange(self.kick_roll, 0.0F);
    input.seed = 0x51EDU;
    const rig::Matrix offset = self.motion.step(*settings, input).offset;
    // OPENMINERZ_DIAGNOSTIC_GUNFX_MOTION=<file.csv>: every frame's offset of a
    // point at the grip (eye space, metres), for frame-rate comparisons.
    if (gunfx_motion_log_ == nullptr && !gunfx_motion_log_tried_)
    {
        gunfx_motion_log_tried_ = true;
        if (const char *path = SDL_getenv("OPENMINERZ_DIAGNOSTIC_GUNFX_MOTION"))
        {
            gunfx_motion_log_ = std::make_unique<std::ofstream>(path);
            *gunfx_motion_log_ << "seconds,frame,x,y,z,yaw,pitch,vx,vy,vz,grounded\n";
        }
    }
    if (gunfx_motion_log_ != nullptr)
    {
        gunfx_motion_clock_ += seconds;
        const std::array<float, 3> grip{0.1F, -0.15F, -0.5F};
        const std::array<float, 3> moved = rig::transform_point(grip.data(), offset);
        *gunfx_motion_log_ << std::format("{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.3f},{:.3f},{:.3f},{:d}\n", gunfx_motion_clock_,
                                          seconds, moved[0] - grip[0], moved[1] - grip[1], moved[2] - grip[2],
                                          player.yaw, player.pitch, player.velocity.x, player.velocity.y,
                                          player.velocity.z, player.grounded ? 1 : 0);
    }
    return offset;
}

// The world draws what your own hands dropped and what others' did, with the
// entities, in both presentations: lit where each lies, falling, settling and
// shrinking away at the end of its life.
void append_gunfx_props(EntityBatch &output, float elapsed_seconds, const AvatarViewmodel::GunfxSolid &solid,
                        const AvatarViewmodel::GunfxLight &light, const world::SkyState &sky, bool rebirth_presentation)
{
    const float seconds = gunfx_props_clock_ < 0.0F || elapsed_seconds < gunfx_props_clock_
                              ? 0.0F
                              : std::min(elapsed_seconds - gunfx_props_clock_, 0.1F);
    gunfx_props_clock_ = elapsed_seconds;
    if (gunfx_props_.empty())
        return;
    gunfx_step_props(seconds, solid);
    const std::size_t first_vertex = output.vertices.size();
    for (GunfxProp &prop : gunfx_props_)
    {
        // Shrinks away over its last half second.
        const float scale = std::clamp((prop.life - prop.age) / 0.5F, 0.0F, 1.0F);
        if (scale <= 0.0F)
            continue;
        rig::BoneTransform turn;
        turn.rotation = prop.turn;
        turn.scale = {scale, scale, scale};
        turn.translation = {prop.position.x, prop.position.y, prop.position.z};
        const rig::Matrix world_matrix = rig::multiply(prop.shape, rig::compose(turn));
        const auto [sky_light, torch_light] = light ? light(prop.position) : std::pair<float, float>{1.0F, 0.0F};
        retail_vertices_.clear();
        retail_spans_.clear();
        if (prop.own != nullptr)
        {
            modding::gunfx::PartSpec spec;
            append_gunfx_own(prop.own, spec, world_matrix);
        }
        else if (prop.model != nullptr && prop.model->loaded())
        {
            std::array<float, 3> primary{};
            std::array<float, 3> secondary{};
            retail_item_tints(prop.item, primary, secondary);
            const auto &parts = prop.model->parts();
            for (std::size_t mesh = 0U; mesh < parts.size() && mesh < prop.triangles.size(); ++mesh)
            {
                const StaticModelPart &part = parts[mesh];
                if (prop.triangles[mesh].empty())
                    continue;
                const std::array<float, 3> tint =
                    (part.material_flags & 8U) != 0U ? projectile_tint(static_cast<InventoryItemId>(prop.item))
                    : (part.material_flags & 2U) != 0U ? secondary
                    : (part.material_flags & 1U) != 0U ? primary
                                                       : std::array<float, 3>{1.0F, 1.0F, 1.0F};
                RetailSpan span;
                span.texture = part.texture != nullptr ? part.texture : white_texture_;
                span.first = static_cast<std::uint32_t>(retail_vertices_.size());
                span.material = retail_item_material(prop.item, part.material_flags);
                span.item = true;
                const rig::Matrix placed = rig::multiply(part.transform, world_matrix);
                constexpr float color_scale = 1.0F / 255.0F;
                for (const std::uint32_t triangle : prop.triangles[mesh])
                {
                    if (triangle * 3U + 2U >= part.indices.size())
                        continue;
                    for (std::size_t corner = 0U; corner < 3U; ++corner)
                    {
                        const std::uint32_t index = part.indices[triangle * 3U + corner];
                        const StaticModelVertex &source =
                            part.vertices[std::min<std::size_t>(index, part.vertices.size() - 1U)];
                        RetailVertex vertex;
                        vertex.position = rig::transform_point(source.position.data(), placed);
                        vertex.normal = rig::transform_vector(source.normal.data(), placed);
                        vertex.uv = source.uv;
                        vertex.color = {part.diffuse[0] * source.color[0] * color_scale * tint[0],
                                        part.diffuse[1] * source.color[1] * color_scale * tint[1],
                                        part.diffuse[2] * source.color[2] * color_scale * tint[2],
                                        source.color[3] * color_scale};
                        retail_vertices_.push_back(vertex);
                    }
                }
                span.count = static_cast<std::uint32_t>(retail_vertices_.size()) - span.first;
                if (span.count > 0U)
                    retail_spans_.push_back(span);
            }
        }
        // Its lowest point, for the next landing; a prop at rest lies on it.
        float lowest = prop.position.y;
        for (const RetailVertex &vertex : retail_vertices_)
            lowest = std::min(lowest, vertex.position[1]);
        prop.depth = std::clamp(prop.position.y - lowest, 0.0F, 0.5F);
        if (prop.resting && lowest < prop.ground - 0.001F)
        {
            const float lift = prop.ground - lowest;
            prop.position.y += lift;
            for (RetailVertex &vertex : retail_vertices_)
                vertex.position[1] += lift;
        }
        // As append_retail_world_player hands a body to the entity pass, the
        // positions already in the world.
        for (const RetailSpan &source_span : retail_spans_)
        {
            EntitySpan span;
            span.texture = source_span.texture;
            span.repeat_texture = true;
            span.first = static_cast<std::uint32_t>(output.vertices.size());
            for (std::uint32_t index = 0; index < source_span.count; ++index)
            {
                const RetailVertex &source = retail_vertices_[source_span.first + index];
                std::array<float, 3> normal = source.normal;
                const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                if (length > 0.000001F)
                    for (float &component : normal)
                        component /= length;
                else
                    normal = {0.0F, 1.0F, 0.0F};
                EntityVertex vertex;
                vertex.x = source.position[0];
                vertex.y = source.position[1];
                vertex.z = source.position[2];
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
                if (rebirth_presentation)
                {
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
                output.vertices.push_back(vertex);
            }
            span.count = static_cast<std::uint32_t>(output.vertices.size()) - span.first;
            if (span.count > 0U)
                output.spans.push_back(span);
        }
    }
    if (gunfx_diagnostic() != 0 && static_cast<int>(elapsed_seconds * 2.0F) != gunfx_props_logged_)
    {
        gunfx_props_logged_ = static_cast<int>(elapsed_seconds * 2.0F);
        const GunfxProp &prop = gunfx_props_.front();
        std::cerr << std::format("GunFX: {} prop(s), the first at {:.2f} {:.2f} {:.2f}{}, {} vertices\n",
                                 gunfx_props_.size(), prop.position.x, prop.position.y, prop.position.z,
                                 prop.resting ? " resting" : "", output.vertices.size() - first_vertex);
    }
}

int gunfx_props_logged_{-1};
std::uint16_t gunfx_motion_item_{0xFFFFU};
float gunfx_props_clock_{-1.0F};
std::unique_ptr<std::ofstream> gunfx_motion_log_;
bool gunfx_motion_log_tried_{};
float gunfx_motion_clock_{};
std::uint32_t gunfx_prepare_logged_{};
