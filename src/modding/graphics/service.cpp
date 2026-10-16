// The graphics and assets service and its script libraries
// (openminerz/modding/graphics.hpp).

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <cmath>

namespace openminerz::modding::graphics
{
namespace
{

const Value &arg(std::span<const Value> args, std::size_t index)
{
    static const Value nil;
    return index < args.size() ? args[index] : nil;
}

Handle expect_handle(const Value &value, HandleKind kind, const char *what)
{
    if (value.type() != ValueType::handle || value.as_handle().kind != kind)
        throw ScriptError{std::string{"expected "} + what + " handle"};
    return value.as_handle();
}

Vec3 to_vec3(const Value &value, const char *what)
{
    if (value.type() == ValueType::vec3)
    {
        const modding::Vec3 v = value.as_vec3();
        const Vec3 result{static_cast<float>(v.x), static_cast<float>(v.y),
                          static_cast<float>(v.z)};
        if (std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z))
            return result;
    }
    throw ScriptError{std::string{what} + ": expected a finite vector"};
}

float optional_number(const Value &table, std::string_view field, float fallback, float low,
                      float high)
{
    const Value *value = table.field(field);
    if (value == nullptr || value->is_nil())
        return fallback;
    if (!value->is_number() || !std::isfinite(value->as_number()))
        throw ScriptError{std::string{field} + ": expected a number"};
    return static_cast<float>(
        std::clamp(value->as_number(), static_cast<double>(low), static_cast<double>(high)));
}

bool optional_bool(const Value &table, std::string_view field, bool fallback)
{
    const Value *value = table.field(field);
    if (value == nullptr || value->is_nil())
        return fallback;
    if (value->type() != ValueType::boolean)
        throw ScriptError{std::string{field} + ": expected true or false"};
    return value->as_bool();
}

std::optional<Attachment> attachment_from(const Value &options)
{
    const Value *target = options.field("attach");
    if (target == nullptr || target->is_nil())
        return std::nullopt;
    if (target->type() != ValueType::handle || (target->as_handle().kind != HandleKind::entity &&
                                                target->as_handle().kind != HandleKind::player))
        throw ScriptError{"attach: expected an entity or player handle"};
    Attachment attachment;
    attachment.target = target->as_handle();
    if (const Value *offset = options.field("offset"); offset != nullptr && !offset->is_nil())
        attachment.offset = to_vec3(*offset, "offset");
    attachment.rotate = optional_bool(options, "rotate", true);
    return attachment;
}

void check_name(const Value &value, const char *what)
{
    if (value.type() != ValueType::string || !valid_asset_name(value.as_string()))
        throw ScriptError{std::string{what} + ": expected an asset name such as \"sounds/bell\""};
}

template <typename Entry>
void check_owner(const Entry *entry, const CallContext &context, const char *what)
{
    if (entry == nullptr)
        throw ScriptError{std::string{"no such "} + what};
    if (entry->mod_id != context.mod_id)
        throw ScriptError{std::string{"that "} + what + " belongs to another mod"};
}

} // namespace

// --- Model instances -------------------------------------------------------

std::optional<std::uint64_t> ModelStore::create(std::string_view mod_id,
                                                const ModelInstanceDesc &desc)
{
    if (entries_.size() >= max_instances || count_for(mod_id) >= max_instances_per_mod)
        return std::nullopt;
    Entry entry;
    entry.id = next_id_++;
    entry.mod_id = mod_id;
    entry.desc = desc;
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

ModelStore::Entry *ModelStore::find(std::uint64_t id) noexcept
{
    const auto found =
        std::lower_bound(entries_.begin(), entries_.end(), id,
                         [](const Entry &entry, std::uint64_t key) { return entry.id < key; });
    return found != entries_.end() && found->id == id ? &*found : nullptr;
}

const ModelStore::Entry *ModelStore::find(std::uint64_t id) const noexcept
{
    return const_cast<ModelStore *>(this)->find(id);
}

bool ModelStore::remove(std::uint64_t id) noexcept
{
    const auto before = entries_.size();
    std::erase_if(entries_, [&](const Entry &entry) { return entry.id == id; });
    return entries_.size() != before;
}

void ModelStore::remove_mod(std::string_view mod_id) noexcept
{
    std::erase_if(entries_, [&](const Entry &entry) { return entry.mod_id == mod_id; });
}

void ModelStore::clear() noexcept
{
    entries_.clear();
}

void ModelStore::step(float seconds) noexcept
{
    if (!std::isfinite(seconds) || seconds <= 0.0F)
        return;
    for (Entry &entry : entries_)
    {
        entry.time += seconds * entry.desc.speed;
        // Keep the clock small; clips loop well inside an hour.
        if (entry.time > 3600.0F)
            entry.time = std::fmod(entry.time, 3600.0F);
    }
}

std::size_t ModelStore::count_for(std::string_view mod_id) const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(),
                      [&](const Entry &entry) { return entry.mod_id == mod_id; }));
}

// --- The service -----------------------------------------------------------

GraphicsService::GraphicsService(GraphicsBackend &backend, ServiceLimits limits)
    : backend_(backend), limits_(limits), assets_(std::make_shared<PresentationAssets>())
{
}

GraphicsService::~GraphicsService()
{
    // No backend call here: the game that implements it may already be going.
    if (presentation_assets() == assets_)
        install_presentation_assets(nullptr);
}

void GraphicsService::set_sources(std::vector<AssetSource> sources)
{
    // A new object, so a loader holding the previous one finishes with it.
    auto next = std::make_shared<PresentationAssets>();
    next->stack = assets_->stack;
    next->stack.set_sources(std::move(sources));
    next->block_tiles = assets_->block_tiles;
    assets_ = std::move(next);
    model_cache_.clear();
    sounds_.clear();
    luts_.clear();
    install_presentation_assets(assets_->stack.empty() ? nullptr : assets_);
}

void GraphicsService::set_source_refresh(std::function<void()> refresh)
{
    refresh_ = std::move(refresh);
    refreshed_at_ = ~0ULL;
}

void GraphicsService::refresh_sources()
{
    if (!refresh_ || refreshed_at_ == steps_)
        return;
    refreshed_at_ = steps_;
    refresh_();
}

void GraphicsService::release_mod(std::string_view mod_id)
{
    lights_.remove_mod(mod_id);
    models_.remove_mod(mod_id);
    atmosphere_.remove_mod(mod_id);
    for (const EffectRecord &record : effect_records_)
        if (record.mod_id == mod_id)
            backend_.kill_effect(record.handle, true);
    std::erase_if(effect_records_,
                  [&](const EffectRecord &record) { return record.mod_id == mod_id; });
    std::erase_if(pending_effects_,
                  [&](const PendingEffect &pending) { return pending.mod_id == mod_id; });
    Mixer *mixer = backend_.mixer(false);
    for (const SoundRecord &record : sound_records_)
        if (record.mod_id == mod_id && mixer != nullptr)
            mixer->stop(record.voice, 0.0F);
    std::erase_if(sound_records_,
                  [&](const SoundRecord &record) { return record.mod_id == mod_id; });
    gunfx_.release_mod(mod_id); // (GunFX)
}

void GraphicsService::reset()
{
    lights_.clear();
    models_.clear();
    atmosphere_.clear();
    camera_.clear();
    gunfx_.clear(); // (GunFX)
    for (const EffectRecord &record : effect_records_)
        backend_.kill_effect(record.handle, true);
    effect_records_.clear();
    pending_effects_.clear();
    creatures_.clear();
    if (Mixer *mixer = backend_.mixer(false))
        mixer->stop_all();
    sound_records_.clear();
}

bool GraphicsService::idle() const noexcept
{
    return lights_.size() == 0U && models_.entries().empty() && atmosphere_.size() == 0U &&
           !camera_.active() && effect_records_.empty() && pending_effects_.empty() &&
           sound_records_.empty() && creatures_.empty() && gunfx_.empty();
}

const CreatureView *GraphicsService::creature(std::uint64_t id) const noexcept
{
    for (const CreatureView &view : creatures_)
        if (view.id == id)
            return &view;
    return nullptr;
}

TargetLocator GraphicsService::locator() const
{
    return [this](Handle target, Vec3 &position, float &yaw) {
        return backend_.locate(target, position, yaw);
    };
}

void GraphicsService::step(float seconds, Vec3 eye)
{
    if (!std::isfinite(seconds) || seconds < 0.0F)
        seconds = 0.0F;
    clock_ += seconds;
    ++steps_;
    models_.step(seconds);
    atmosphere_.step(seconds, eye);
    camera_.step(seconds);
    if (!pending_effects_.empty())
        spawn_pending_effects(seconds);
    // Effects that follow something; drop the ones that have ended.
    for (EffectRecord &record : effect_records_)
    {
        if (!record.attachment)
            continue;
        Vec3 position{};
        float yaw{};
        if (!backend_.locate(record.attachment->target, position, yaw))
        {
            backend_.kill_effect(record.handle, false);
            record.attachment.reset();
            continue;
        }
        Vec3 offset = record.attachment->offset;
        if (record.attachment->rotate)
            offset = {offset.x * std::cos(yaw) - offset.z * std::sin(yaw), offset.y,
                      offset.x * std::sin(yaw) + offset.z * std::cos(yaw)};
        position += offset;
        const Vec3 velocity =
            seconds > 0.0F ? (position - record.last_position) * (1.0F / seconds) : Vec3{};
        backend_.move_effect(record.handle, position, velocity);
        record.last_position = position;
    }
    std::erase_if(effect_records_, [&](const EffectRecord &record) {
        return !backend_.effect_alive(record.handle);
    });
    Mixer *mixer = backend_.mixer(false);
    for (SoundRecord &record : sound_records_)
    {
        if (mixer == nullptr || !record.attachment)
            continue;
        Vec3 position{};
        float yaw{};
        if (backend_.locate(record.attachment->target, position, yaw))
            mixer->set_position(record.voice, position + record.attachment->offset);
    }
    std::erase_if(sound_records_, [&](const SoundRecord &record) {
        return mixer == nullptr || !mixer->playing(record.voice);
    });
}

const Image *GraphicsService::lut(std::string_view name)
{
    if (name.empty())
        return nullptr;
    refresh_sources();
    if (const auto found = luts_.find(name); found != luts_.end())
        return found->second.empty() ? nullptr : &found->second;
    static constexpr std::array<std::string_view, 1> extensions{".png"};
    Image image;
    if (const auto resolved = assets_->stack.resolve(name, extensions))
    {
        std::string error;
        if (auto loaded = load_image(resolved->path, &error, {4096, 16U * 1024U * 1024U});
            loaded && valid_lut(*loaded))
            image = std::move(*loaded);
    }
    if (luts_.size() >= 32U)
        luts_.clear();
    const auto [entry, inserted] = luts_.emplace(std::string{name}, std::move(image));
    (void)inserted;
    return entry->second.empty() ? nullptr : &entry->second;
}

std::shared_ptr<const SoundAsset> GraphicsService::sound(std::string_view name, std::string *error)
{
    refresh_sources();
    if (const auto found = sounds_.find(name); found != sounds_.end())
        return found->second;
    static constexpr std::array<std::string_view, 2> extensions{".ogg", ".wav"};
    const auto resolved = assets_->stack.resolve(name, extensions);
    if (!resolved)
    {
        if (error != nullptr)
            *error = "no sound \"" + std::string{name} + "\" in any enabled mod";
        return nullptr;
    }
    const bool music = name.starts_with("music/") || name.find("/music/") != std::string_view::npos;
    AudioLimits limits;
    if (music)
        limits.max_seconds = 900.0;
    auto bytes = read_file_bounded(resolved->path, limits.max_file_bytes, error);
    if (!bytes)
        return nullptr;
    std::shared_ptr<const SoundAsset> asset;
    const bool ogg = bytes->size() >= 4U && std::equal(bytes->begin(), bytes->begin() + 4, "OggS");
    if (music && ogg)
        asset = SoundAsset::from_ogg_stream(std::move(*bytes), error);
    else if (auto pcm = decode_audio(*bytes, error, limits))
        asset = SoundAsset::from_pcm(std::move(*pcm));
    if (asset == nullptr)
        return nullptr;
    if (sounds_.size() >= limits_.max_sound_assets)
        sounds_.clear(); // voices keep what they are playing
    sounds_.emplace(std::string{name}, asset);
    return asset;
}

Handle GraphicsService::spawn_effect(std::string_view mod_id, std::string_view recipe,
                                     const vfx::SpawnParams &params,
                                     std::optional<Attachment> attachment)
{
    const vfx::EffectDesc *desc = effects_.find(mod_id, recipe);
    if (desc == nullptr)
        throw ScriptError{"no effect \"" + std::string{recipe} + "\""};
    const bool available = backend_.effects_available();
    if (!available && !backend_.effects_pending())
        return {};
    const auto own = static_cast<std::size_t>(
        std::count_if(effect_records_.begin(), effect_records_.end(),
                      [&](const EffectRecord &record) { return record.mod_id == mod_id; }) +
        std::count_if(pending_effects_.begin(), pending_effects_.end(),
                      [&](const PendingEffect &pending) { return pending.mod_id == mod_id; }));
    if (own >= limits_.max_effects_per_mod)
        throw ScriptError{"too many live effects in this mod"};
    if (!available)
    {
        // Scripts run at session start, before the world's first frame: the
        // effect waits for it, and its handle is good from now on.
        PendingEffect pending;
        pending.id = next_effect_id_++;
        pending.mod_id = mod_id;
        pending.desc = desc;
        pending.params = params;
        pending.attachment = std::move(attachment);
        pending_effects_.push_back(std::move(pending));
        return {HandleKind::effect, pending_effects_.back().id};
    }
    vfx::SpawnParams spawn = params;
    if (attachment)
    {
        Vec3 position{};
        float yaw{};
        if (!backend_.locate(attachment->target, position, yaw))
            return {};
        spawn.position = position + attachment->offset;
    }
    const vfx::EffectHandle handle = backend_.spawn_effect(*desc, spawn);
    if (!handle.valid())
        return {};
    EffectRecord record;
    record.id = next_effect_id_++;
    record.mod_id = mod_id;
    record.handle = handle;
    record.attachment = std::move(attachment);
    record.last_position = spawn.position;
    effect_records_.push_back(std::move(record));
    return {HandleKind::effect, effect_records_.back().id};
}

void GraphicsService::spawn_pending_effects(float seconds)
{
    if (!backend_.effects_available())
    {
        // Effects that never come (the Original presentation) or that waited
        // too long are dropped, as a spawn then would have been refused.
        const bool waiting = backend_.effects_pending();
        std::erase_if(pending_effects_, [&](PendingEffect &pending) {
            pending.age += seconds;
            return !waiting || pending.age > pending_effect_seconds;
        });
        return;
    }
    std::vector<PendingEffect> pending = std::move(pending_effects_);
    pending_effects_.clear();
    for (PendingEffect &effect : pending)
    {
        vfx::SpawnParams spawn = effect.params;
        if (effect.attachment)
        {
            Vec3 position{};
            float yaw{};
            if (!backend_.locate(effect.attachment->target, position, yaw))
                continue;
            spawn.position = position + effect.attachment->offset;
        }
        const vfx::EffectHandle handle = backend_.spawn_effect(*effect.desc, spawn);
        if (!handle.valid())
            continue;
        EffectRecord record;
        record.id = effect.id;
        record.mod_id = std::move(effect.mod_id);
        record.handle = handle;
        record.attachment = std::move(effect.attachment);
        record.last_position = spawn.position;
        effect_records_.push_back(std::move(record));
    }
}

bool GraphicsService::stop_effect(Handle handle, bool immediate)
{
    for (auto pending = pending_effects_.begin(); pending != pending_effects_.end(); ++pending)
        if (pending->id == handle.id)
        {
            pending_effects_.erase(pending);
            return true;
        }
    for (auto record = effect_records_.begin(); record != effect_records_.end(); ++record)
        if (record->id == handle.id)
        {
            backend_.kill_effect(record->handle, immediate);
            effect_records_.erase(record);
            return true;
        }
    return false;
}

bool GraphicsService::move_effect(Handle handle, Vec3 position, Vec3 velocity)
{
    for (PendingEffect &pending : pending_effects_)
        if (pending.id == handle.id)
        {
            pending.params.position = position;
            pending.params.velocity = velocity;
            return true;
        }
    for (EffectRecord &record : effect_records_)
        if (record.id == handle.id)
        {
            backend_.move_effect(record.handle, position, velocity);
            record.last_position = position;
            return true;
        }
    return false;
}

Handle GraphicsService::play_sound(std::string_view mod_id, std::string_view name,
                                   const VoiceParams &params, std::optional<Attachment> attachment)
{
    Mixer *mixer = backend_.mixer(true);
    if (mixer == nullptr)
        return {};
    const auto own = static_cast<std::size_t>(
        std::count_if(sound_records_.begin(), sound_records_.end(),
                      [&](const SoundRecord &record) { return record.mod_id == mod_id; }));
    if (own >= limits_.max_voices_per_mod)
        return {};
    std::string error;
    auto asset = sound(name, &error);
    if (asset == nullptr)
        throw ScriptError{error};
    VoiceParams voice = params;
    if (attachment)
    {
        Vec3 position{};
        float yaw{};
        if (backend_.locate(attachment->target, position, yaw))
            voice.position = position + attachment->offset;
    }
    mixer->mark(name); // (GunFX) names the sound in an audio dump
    const std::uint64_t id = mixer->play(std::move(asset), voice);
    if (id == 0U)
        return {};
    SoundRecord record;
    record.id = next_sound_id_++;
    record.mod_id = mod_id;
    record.voice = id;
    record.attachment = std::move(attachment);
    sound_records_.push_back(std::move(record));
    return {HandleKind::sound, sound_records_.back().id};
}

bool GraphicsService::stop_sound(Handle handle, float fade_seconds)
{
    for (auto record = sound_records_.begin(); record != sound_records_.end(); ++record)
        if (record->id == handle.id)
        {
            if (Mixer *mixer = backend_.mixer(false))
                mixer->stop(record->voice, fade_seconds);
            sound_records_.erase(record);
            return true;
        }
    return false;
}

bool GraphicsService::play_music(std::string_view mod_id, std::string_view name,
                                 float crossfade_seconds, float volume, bool loop)
{
    (void)mod_id;
    Mixer *mixer = backend_.mixer(true);
    if (mixer == nullptr)
        return false;
    std::string error;
    auto track = sound(name, &error);
    if (track == nullptr)
        throw ScriptError{error};
    mixer->play_music(std::move(track), crossfade_seconds, volume, loop);
    return true;
}

void GraphicsService::stop_music(float fade_seconds)
{
    if (Mixer *mixer = backend_.mixer(false))
        mixer->play_music(nullptr, fade_seconds, 0.0F, false);
}

// --- Script libraries ------------------------------------------------------

void register_graphics_api(ApiRegistry &registry, ScriptHost &host, GraphicsService &service)
{
    // Everything the client VM's scripts made goes when that VM does.
    host.add_teardown_listener([&service](Side side) {
        if (side == Side::client)
            service.reset();
    });

    const auto add = [&](FunctionSpec spec, NativeFunction function) {
        spec.sides = side_client;
        spec.requires_caps = capability::presentation;
        registry.add_function(std::move(spec), std::move(function));
    };
    using P = ParamSpec;
    using T = ValueType;

    // ---- light ------------------------------------------------------------
    add({"light.create",
         "Places a light: {position=, color=, intensity=1, radius=8, flicker=0, "
         "flicker_speed=9, pulse=0, pulse_speed=1, shape=\"point\"|\"spot\", direction=, "
         "inner_angle=25, outer_angle=40, enabled=true}. Lights terrain, creatures, models and "
         "the first-person body in both presentations; a world can hold 4096 (1024 per mod) and "
         "the 256 most important in view are drawn.",
         {P{"desc", T::map, false, "the light"}},
         T::handle,
         "the light, or nil when the limit is reached"},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const LightDesc desc = light_from_value(arg(args, 0));
            const auto id = service.lights().create(context.mod_id, desc);
            if (!id)
                return {};
            return Handle{HandleKind::light, *id};
        });
    add({"light.set",
         "Changes some fields of a light (the same fields as light.create).",
         {P{"light", T::handle, false, ""}, P{"desc", T::map, false, "fields to change"}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), HandleKind::light, "a light");
            auto *entry = service.lights().find(handle.id);
            check_owner(entry, context, "light");
            entry->desc = light_from_value(arg(args, 1), entry->desc);
            return {};
        });
    add({"light.remove", "Removes a light.", {P{"light", T::handle, false, ""}}, T::boolean},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), HandleKind::light, "a light");
            const auto *entry = service.lights().find(handle.id);
            if (entry == nullptr)
                return false;
            check_owner(entry, context, "light");
            return service.lights().remove(handle.id);
        });
    add({"light.attach",
         "Makes a light follow an entity or player: offset in its frame, turned with its yaw "
         "unless rotate=false. The light goes dark while the target is gone.",
         {P{"light", T::handle, false, ""}, P{"target", T::handle, false, "entity or player"},
          P{"offset", T::vec3, true, ""}, P{"rotate", T::boolean, true, ""}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), HandleKind::light, "a light");
            auto *entry = service.lights().find(handle.id);
            check_owner(entry, context, "light");
            Map options{{"attach", arg(args, 1)}};
            if (!arg(args, 2).is_nil())
                options.emplace_back("offset", arg(args, 2));
            if (!arg(args, 3).is_nil())
                options.emplace_back("rotate", arg(args, 3));
            entry->attachment = attachment_from(Value{options});
            return {};
        });
    add({"light.detach",
         "Stops following; the light stays where it is.",
         {P{"light", T::handle, false, ""}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), HandleKind::light, "a light");
            auto *entry = service.lights().find(handle.id);
            check_owner(entry, context, "light");
            if (entry->attachment)
            {
                Vec3 position{};
                float yaw{};
                if (service.locator()(entry->attachment->target, position, yaw))
                    entry->desc.position = position + entry->attachment->offset;
                entry->attachment.reset();
            }
            return {};
        });
    add({"light.enable",
         "Switches a light on or off.",
         {P{"light", T::handle, false, ""}, P{"on", T::boolean, false, ""}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), HandleKind::light, "a light");
            auto *entry = service.lights().find(handle.id);
            check_owner(entry, context, "light");
            entry->desc.enabled = arg(args, 1).as_bool();
            return {};
        });
    add({"light.count", "Lights this mod has placed.", {}, T::integer},
        [&service](CallContext &context, std::span<const Value>) -> Value {
            return static_cast<std::int64_t>(service.lights().count_for(context.mod_id));
        });

    // ---- fx ---------------------------------------------------------------
    add({"fx.define",
         "Defines an effect recipe from a table (emitters, lights, shake, flash; the fields of "
         "vfx::EffectDesc). Returns its full name \"<mod>:<name>\".",
         {P{"name", T::string, false, ""}, P{"recipe", T::map, false, ""}},
         T::string},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const std::string &name = arg(args, 0).as_string();
            const vfx::EffectDesc &desc =
                service.effect_library().define(context.mod_id, name, arg(args, 1));
            return std::string{desc.name};
        });
    add({"fx.spawn",
         "Plays an effect at a position: one of this mod's recipes, \"mod:name\", or a built-in "
         "(fx.builtins()). Options: direction, velocity, scale, intensity, tint, attach, offset. "
         "Returns nil in the Original presentation, which has no effects layer. Spawned before "
         "a world's first frame, the effect waits for it (up to ten seconds; dropped then if "
         "the presentation has no effects layer).",
         {P{"recipe", T::string, false, ""}, P{"position", T::vec3, false, ""},
          P{"options", T::map, true, ""}},
         T::handle},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const std::string &recipe = arg(args, 0).as_string();
            vfx::SpawnParams params;
            params.position = to_vec3(arg(args, 1), "position");
            const Value &options = arg(args, 2);
            std::optional<Attachment> attachment;
            if (!options.is_nil())
            {
                if (const Value *direction = options.field("direction"))
                    params.direction = vfx::normalized(to_vec3(*direction, "direction"));
                if (const Value *velocity = options.field("velocity"))
                    params.velocity = to_vec3(*velocity, "velocity");
                params.scale = optional_number(options, "scale", 1.0F, 0.05F, 16.0F);
                params.intensity = optional_number(options, "intensity", 1.0F, 0.0F, 8.0F);
                if (const Value *tint = options.field("tint"); tint != nullptr && !tint->is_nil())
                    params.tint = color_from_value(*tint);
                attachment = attachment_from(options);
            }
            const Handle handle =
                service.spawn_effect(context.mod_id, recipe, params, std::move(attachment));
            return handle.valid() ? Value{handle} : Value{};
        });
    add({"fx.move",
         "Moves a playing effect (streams are laid along the path).",
         {P{"effect", T::handle, false, ""}, P{"position", T::vec3, false, ""},
          P{"velocity", T::vec3, true, ""}},
         T::boolean},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), HandleKind::effect, "an effect");
            const Vec3 velocity =
                arg(args, 2).is_nil() ? Vec3{} : to_vec3(arg(args, 2), "velocity");
            return service.move_effect(handle, to_vec3(arg(args, 1), "position"), velocity);
        });
    add({"fx.stop",
         "Stops an effect emitting; immediate=true also removes what it already emitted.",
         {P{"effect", T::handle, false, ""}, P{"immediate", T::boolean, true, ""}},
         T::boolean},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), HandleKind::effect, "an effect");
            return service.stop_effect(handle, !arg(args, 1).is_nil() && arg(args, 1).as_bool());
        });
    add({"fx.builtins", "Names of the built-in effect recipes.", {}, T::array},
        [](CallContext &, std::span<const Value>) -> Value {
            Array names;
            for (const vfx::EffectDesc *desc : vfx::all_effects())
                names.emplace_back(std::string{desc->name});
            return names;
        });

    // ---- model ------------------------------------------------------------
    add({"model.spawn",
         "Places a glTF model (\"models/lantern\") at a position. Options: yaw, pitch, roll "
         "(degrees), scale, tint, animation, loop, speed, visible, attach, offset.",
         {P{"model", T::string, false, ""}, P{"position", T::vec3, false, ""},
          P{"options", T::map, true, ""}},
         T::handle},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            check_name(arg(args, 0), "model");
            service.refresh_sources();
            ModelInstanceDesc desc;
            desc.model = arg(args, 0).as_string();
            desc.position = to_vec3(arg(args, 1), "position");
            std::optional<Attachment> attachment;
            if (!arg(args, 2).is_nil())
            {
                desc = model_instance_from_value(arg(args, 2), desc);
                attachment = attachment_from(arg(args, 2));
            }
            std::string error;
            if (service.model_cache().get(service.assets(), desc.model, &error) == nullptr)
                throw ScriptError{error};
            const auto id = service.models().create(context.mod_id, desc);
            if (!id)
                return {};
            service.models().find(*id)->attachment = std::move(attachment);
            return Handle{model_handle_kind, *id};
        });
    add({"model.set",
         "Changes a placed model (the options of model.spawn, and position).",
         {P{"model", T::handle, false, ""}, P{"options", T::map, false, ""}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), model_handle_kind, "a model");
            auto *entry = service.models().find(handle.id);
            check_owner(entry, context, "model");
            const std::string previous_animation = entry->desc.animation;
            entry->desc = model_instance_from_value(arg(args, 1), entry->desc);
            if (entry->desc.animation != previous_animation)
                entry->time = 0.0F;
            if (arg(args, 1).field("attach") != nullptr)
                entry->attachment = attachment_from(arg(args, 1));
            return {};
        });
    add({"model.play",
         "Plays one of the model's animation clips from its start.",
         {P{"model", T::handle, false, ""}, P{"clip", T::string, false, ""},
          P{"options", T::map, true, "loop, speed"}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), model_handle_kind, "a model");
            auto *entry = service.models().find(handle.id);
            check_owner(entry, context, "model");
            entry->desc.animation = arg(args, 1).as_string();
            entry->time = 0.0F;
            if (!arg(args, 2).is_nil())
            {
                entry->desc.loop = optional_bool(arg(args, 2), "loop", entry->desc.loop);
                entry->desc.speed =
                    optional_number(arg(args, 2), "speed", entry->desc.speed, 0.0F, 16.0F);
            }
            return {};
        });
    add({"model.remove", "Removes a placed model.", {P{"model", T::handle, false, ""}}, T::boolean},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), model_handle_kind, "a model");
            const auto *entry = service.models().find(handle.id);
            if (entry == nullptr)
                return false;
            check_owner(entry, context, "model");
            return service.models().remove(handle.id);
        });
    add({"model.clips",
         "The animation clips of a model file.",
         {P{"model", T::string, false, ""}},
         T::array},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            check_name(arg(args, 0), "model");
            service.refresh_sources();
            std::string error;
            const auto model =
                service.model_cache().get(service.assets(), arg(args, 0).as_string(), &error);
            if (model == nullptr)
                throw ScriptError{error};
            Array names;
            for (const ModelAnimation &animation : model->animations)
                names.emplace_back(animation.name);
            return names;
        });

    // ---- atmosphere -------------------------------------------------------
    add({"atmosphere.push",
         "Adds an atmosphere layer that blends over the game's own: sky_zenith, sky_horizon, "
         "fog_color, ambient, sun_color, ambient_intensity, sun_intensity, fog_start, fog_end, "
         "exposure (EV), bloom, saturation, contrast, temperature, vignette, lut "
         "(\"luts/name\"), lut_strength; region {min=, max=} or {center=, radius=} with "
         "feather; priority, fade_in, fade_out, strength.",
         {P{"layer", T::map, false, ""}},
         T::handle},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const auto id =
                service.atmosphere().create(context.mod_id, atmosphere_from_value(arg(args, 0)));
            if (!id)
                return {};
            return Handle{atmosphere_handle_kind, *id};
        });
    add({"atmosphere.set",
         "Replaces a layer's values (it keeps its fade).",
         {P{"layer", T::handle, false, ""}, P{"values", T::map, false, ""}}},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            const Handle handle =
                expect_handle(arg(args, 0), atmosphere_handle_kind, "an atmosphere layer");
            if (!service.atmosphere().update(handle.id, atmosphere_from_value(arg(args, 1))))
                throw ScriptError{"no such atmosphere layer"};
            return {};
        });
    add({"atmosphere.remove",
         "Fades a layer out and removes it (immediate=true: now).",
         {P{"layer", T::handle, false, ""}, P{"immediate", T::boolean, true, ""}},
         T::boolean},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            const Handle handle =
                expect_handle(arg(args, 0), atmosphere_handle_kind, "an atmosphere layer");
            return service.atmosphere().remove(handle.id,
                                               !arg(args, 1).is_nil() && arg(args, 1).as_bool());
        });
    add({"atmosphere.weight",
         "How strongly a layer applies now (fade times region, 0..1).",
         {P{"layer", T::handle, false, ""}},
         T::number},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            const Handle handle =
                expect_handle(arg(args, 0), atmosphere_handle_kind, "an atmosphere layer");
            return static_cast<double>(service.atmosphere().weight(handle.id));
        });

    // ---- sound / music ----------------------------------------------------
    add({"sound.play",
         "Plays a mod sound (\"sounds/bell\", .ogg or .wav). Options: volume, pitch, loop, "
         "position (3D), min_distance=2, max_distance=32, bus=\"effects\"|\"ambience\", "
         "fade_in, attach, offset. Follows the game's volume settings.",
         {P{"sound", T::string, false, ""}, P{"options", T::map, true, ""}},
         T::handle},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            check_name(arg(args, 0), "sound");
            VoiceParams params;
            std::optional<Attachment> attachment;
            const Value &options = arg(args, 1);
            if (!options.is_nil())
            {
                params.volume = optional_number(options, "volume", 1.0F, 0.0F, 4.0F);
                params.pitch = optional_number(options, "pitch", 1.0F, 0.25F, 4.0F);
                params.loop = optional_bool(options, "loop", false);
                if (const Value *position = options.field("position");
                    position != nullptr && !position->is_nil())
                    params.position = to_vec3(*position, "position");
                params.min_distance = optional_number(options, "min_distance", 2.0F, 0.1F, 256.0F);
                params.max_distance = optional_number(options, "max_distance", 32.0F, 0.5F, 512.0F);
                params.fade_in = optional_number(options, "fade_in", 0.0F, 0.0F, 30.0F);
                if (const Value *bus = options.field("bus"); bus != nullptr && !bus->is_nil())
                {
                    if (bus->type() != ValueType::string ||
                        (bus->as_string() != "effects" && bus->as_string() != "ambience"))
                        throw ScriptError{"bus: expected \"effects\" or \"ambience\""};
                    params.bus =
                        bus->as_string() == "ambience" ? AudioBus::ambience : AudioBus::effects;
                }
                attachment = attachment_from(options);
            }
            const Handle handle = service.play_sound(context.mod_id, arg(args, 0).as_string(),
                                                     params, std::move(attachment));
            return handle.valid() ? Value{handle} : Value{};
        });
    add({"sound.stop",
         "Stops a sound, fading over `fade` seconds.",
         {P{"sound", T::handle, false, ""}, P{"fade", T::number, true, ""}},
         T::boolean},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            const Handle handle = expect_handle(arg(args, 0), HandleKind::sound, "a sound");
            const float fade =
                arg(args, 1).is_nil() ? 0.0F : static_cast<float>(arg(args, 1).as_number());
            return service.stop_sound(handle, fade);
        });
    add({"music.play",
         "Plays a music track (\"music/theme\", streamed .ogg or .wav), crossfading from what "
         "plays. Options: crossfade=2, volume=1, loop=true. Follows the Music volume.",
         {P{"track", T::string, false, ""}, P{"options", T::map, true, ""}},
         T::boolean},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            check_name(arg(args, 0), "track");
            const Value &options = arg(args, 1);
            const float crossfade =
                options.is_nil() ? 2.0F : optional_number(options, "crossfade", 2.0F, 0.0F, 30.0F);
            const float volume =
                options.is_nil() ? 1.0F : optional_number(options, "volume", 1.0F, 0.0F, 4.0F);
            const bool loop = options.is_nil() ? true : optional_bool(options, "loop", true);
            return service.play_music(context.mod_id, arg(args, 0).as_string(), crossfade, volume,
                                      loop);
        });
    add({"music.stop", "Fades the mod music out.", {P{"fade", T::number, true, ""}}},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            service.stop_music(
                arg(args, 0).is_nil() ? 2.0F : static_cast<float>(arg(args, 0).as_number()));
            return {};
        });

    // ---- camera -----------------------------------------------------------
    add({"camera.shake",
         "Shakes the view: strength 0..1, seconds, frequency (Hz). Scaled by the player's "
         "Explosion camera shake setting; none with Reduced motion.",
         {P{"strength", T::number, false, ""}, P{"seconds", T::number, true, ""},
          P{"frequency", T::number, true, ""}}},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            service.camera().shake(
                static_cast<float>(arg(args, 0).as_number()),
                arg(args, 1).is_nil() ? 0.5F : static_cast<float>(arg(args, 1).as_number()),
                arg(args, 2).is_nil() ? 14.0F : static_cast<float>(arg(args, 2).as_number()));
            return {};
        });
    add({"camera.fade",
         "Fades a full-screen colour over the world (under the HUD) to `alpha` over `seconds`.",
         {P{"color", T::any, false, ""}, P{"alpha", T::number, false, ""},
          P{"seconds", T::number, true, ""}}},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            service.camera().fade(
                color_from_value(arg(args, 0)), static_cast<float>(arg(args, 1).as_number()),
                arg(args, 2).is_nil() ? 0.5F : static_cast<float>(arg(args, 2).as_number()));
            return {};
        });
    add({"camera.fov_pulse",
         "Widens (or with a negative value narrows) the view by `degrees` and back over "
         "`seconds`. None with Reduced motion.",
         {P{"degrees", T::number, false, ""}, P{"seconds", T::number, true, ""}}},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            service.camera().fov_pulse(
                static_cast<float>(arg(args, 0).as_number()),
                arg(args, 1).is_nil() ? 0.4F : static_cast<float>(arg(args, 1).as_number()));
            return {};
        });
    add({"camera.clear", "Ends every shake, pulse and fade at once.", {}},
        [&service](CallContext &, std::span<const Value>) -> Value {
            service.camera().clear();
            return {};
        });

    // ---- gunfx (GunFX: weapon parts, clip cues, viewmodel motion) ----------
    add({"gunfx.parts",
         "Cuts named parts out of a retail held-item model (\"ak\", \"pistol\", ...; "
         "\"classic/ak\" or \"modern/ak\" for one generation), from the player's own installed "
         "content, so clips can move them: a list of {name=, box={min=,max=} or boxes={...}, "
         "exclude={...}, mesh=, islands=, copy=, model=, pivot=, rotation=, scale=, hidden=}, "
         "boxes in the model's own space (barrel along -X, up +Z, right +Y; 1 unit = 1 block). "
         "Earlier parts claim triangles first. Nodes named after a part in a clip file animate it. "
         "Presentation only.",
         {P{"model", T::string, false, "a retail held model name"},
          P{"parts", T::any, false, "a list (or map) of part tables"}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            const std::string &model = arg(args, 0).as_string();
            const std::string_view stem =
                model.starts_with("classic/") ? std::string_view{model}.substr(8U)
                : model.starts_with("modern/") ? std::string_view{model}.substr(7U)
                                                : std::string_view{model};
            if (stem.empty() || stem.size() > 40U ||
                !std::all_of(stem.begin(), stem.end(), [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                }))
                throw ScriptError{"gunfx.parts: expected a model name such as \"ak\" or \"modern/ak\""};
            service.gunfx().define_parts(context.mod_id, model, gunfx::parts_from_value(arg(args, 1)));
            return {};
        });
    add({"gunfx.events",
         "Timed cues for a player clip (\"GunReload\", a variant \"PistolReloadEmpty\" or "
         "\"PumpShotgunReload@rpg\"): a list of {time= (seconds of your clip, stretched with a "
         "reload) or at= (0..1), and one of sound=, show=, hide=, attach= (with bone=), detach= "
         "(blend=), drop= (velocity=, spin=, life=), kick= {pitch=, yaw=, roll=, seconds=}; "
         "volume=, pitch=, volume_jitter=, pitch_jitter=, who=\"all\"|\"self\"|\"others\"}. "
         "Options: model= (only with that model in hand), mute= (retail cues not played for it), "
         "length=, remote_volume=0.6, remote_distance=24. Your own cues play in first person, "
         "other players' at their hands; a clip cut short stops its sounds.",
         {P{"clip", T::string, false, "the clip's registered name"},
          P{"cues", T::array, false, "the cues"},
          P{"options", T::map, true, "model, mute, length, remote_volume, remote_distance"}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            gunfx::ClipCues list =
                gunfx::cues_from_value(arg(args, 0).as_string(), arg(args, 1), arg(args, 2));
            list.mod_id = std::string{context.mod_id};
            service.gunfx().define_cues(std::move(list));
            return {};
        });
    add({"gunfx.motion",
         "Turns on MW-style procedural first-person weapon motion: multipliers (1 = default, "
         "0 = off) sway=, bob=, roll=, sprint=, jump=, breathe=, recoil=, switch=, aim_ease= "
         "(0..1), aim_damping= (0..1, kept while aimed, 0.3), weight= (heavier = slower), and "
         "families={Pistol={...}, LMG={...}, ...} per weapon family. The camera and the aim are "
         "untouched; Reduced motion turns it down. nil turns it off.",
         {P{"settings", T::map, true, "the multipliers"}}},
        [&service](CallContext &context, std::span<const Value> args) -> Value {
            service.gunfx().define_motion(context.mod_id, gunfx::motion_from_value(arg(args, 0)));
            return {};
        });
    add({"gunfx.clear", "Drops this mod's gunfx parts, cues and motion.", {}},
        [&service](CallContext &context, std::span<const Value>) -> Value {
            service.gunfx().release_mod(context.mod_id);
            return {};
        });

    // ---- assets -----------------------------------------------------------
    add({"assets.exists",
         "Whether an asset is present in any enabled mod (\"@mod/name\" pins one).",
         {P{"name", T::string, false, ""}},
         T::boolean},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            service.refresh_sources();
            const std::string &name = arg(args, 0).as_string();
            static constexpr std::array<std::string_view, 6> extensions{".png", ".gltf", ".glb",
                                                                        ".ogg", ".wav",  ".bmp"};
            return service.assets().resolve(name, extensions).has_value();
        });
    add({"assets.list",
         "Files directly in an asset folder, across every enabled mod.",
         {P{"folder", T::string, false, ""}},
         T::array},
        [&service](CallContext &, std::span<const Value> args) -> Value {
            service.refresh_sources();
            static constexpr std::array<std::string_view, 6> extensions{".png", ".gltf", ".glb",
                                                                        ".ogg", ".wav",  ".bmp"};
            Array names;
            for (const AssetStack::Resolved &file :
                 service.assets().list(arg(args, 0).as_string(), extensions))
                names.emplace_back(file.name);
            return names;
        });
}

} // namespace openminerz::modding::graphics
