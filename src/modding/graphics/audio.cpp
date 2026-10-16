// Mod sounds and music: bounded WAV and Ogg Vorbis decoding and a small
// software mixer with 3D panning, loops and music crossfades
// (openminerz/modding/graphics.hpp). The retail sounds keep playing through
// the original XACT banks; this mixer only ever plays what a mod brought.

#include "openminerz/modding/graphics.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <numbers>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push)
// clang-format off
#pragma warning(disable : 4100 4127 4189 4244 4245 4267 4456 4457 4701 4702 4996)
#pragma warning(disable : 6001 6011 6246 6262 6263 6385 6386)
// clang-format on
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis.c>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
// stb_vorbis leaves these one-letter macros behind.
#undef L
#undef C
#undef R

namespace openminerz::modding::graphics
{
namespace
{

std::uint32_t read_u32(const std::uint8_t *bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint16_t read_u16(const std::uint8_t *bytes) noexcept
{
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8U));
}

std::int16_t to_sample(float value) noexcept
{
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
}

bool fail(std::string *error, std::string message)
{
    if (error != nullptr)
        *error = std::move(message);
    return false;
}

std::nullopt_t none(std::string *error, std::string message)
{
    fail(error, std::move(message));
    return std::nullopt;
}

struct VorbisDeleter final
{
    void operator()(stb_vorbis *stream) const noexcept
    {
        stb_vorbis_close(stream);
    }
};
using VorbisPointer = std::unique_ptr<stb_vorbis, VorbisDeleter>;

VorbisPointer open_vorbis(std::span<const std::uint8_t> bytes, std::string *error)
{
    if (bytes.empty() || bytes.size() > 0x7FFFFFFFU)
    {
        fail(error, "empty or oversized Ogg file");
        return nullptr;
    }
    int code{};
    VorbisPointer stream{
        stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &code, nullptr)};
    if (stream == nullptr)
        fail(error, "not an Ogg Vorbis file (stb_vorbis error " + std::to_string(code) + ")");
    return stream;
}

} // namespace

std::optional<PcmBuffer> decode_wav(std::span<const std::uint8_t> bytes, std::string *error,
                                    const AudioLimits &limits)
{
    if (bytes.size() > limits.max_file_bytes)
        return none(error, "WAV file too large");
    if (bytes.size() < 12U || std::memcmp(bytes.data(), "RIFF", 4U) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4U) != 0)
        return none(error, "not a RIFF/WAVE file");
    std::uint16_t format{};
    std::uint16_t channels{};
    std::uint32_t rate{};
    std::uint16_t block_align{};
    std::uint16_t bits{};
    bool have_format{};
    std::span<const std::uint8_t> data;
    std::size_t at = 12U;
    while (at + 8U <= bytes.size())
    {
        const std::uint8_t *header = bytes.data() + at;
        const std::uint32_t size = read_u32(header + 4);
        const std::size_t body = at + 8U;
        if (size > bytes.size() - body)
        {
            // A truncated final data chunk is common; take what is there.
            if (std::memcmp(header, "data", 4U) == 0 && have_format)
            {
                data = bytes.subspan(body);
                break;
            }
            return none(error, "WAV chunk runs past the end of the file");
        }
        if (std::memcmp(header, "fmt ", 4U) == 0)
        {
            if (size < 16U)
                return none(error, "WAV format chunk too short");
            const std::uint8_t *fmt = bytes.data() + body;
            format = read_u16(fmt);
            channels = read_u16(fmt + 2);
            rate = read_u32(fmt + 4);
            block_align = read_u16(fmt + 12);
            bits = read_u16(fmt + 14);
            if (format == 0xFFFEU && size >= 26U)
                format = read_u16(fmt + 24); // WAVE_FORMAT_EXTENSIBLE sub-format
            have_format = true;
        }
        else if (std::memcmp(header, "data", 4U) == 0)
        {
            data = bytes.subspan(body, size);
            if (have_format)
                break;
        }
        at = body + size + (size & 1U);
    }
    if (!have_format)
        return none(error, "WAV file has no format chunk");
    if (data.empty())
        return none(error, "WAV file has no samples");
    const bool pcm = format == 1U && (bits == 8U || bits == 16U || bits == 24U || bits == 32U);
    const bool floating = format == 3U && bits == 32U;
    if (!pcm && !floating)
        return none(error, "unsupported WAV encoding (PCM 8/16/24/32-bit or float 32 only)");
    if (channels == 0U || channels > 8U)
        return none(error, "WAV channel count out of range (1..8)");
    if (rate < static_cast<std::uint32_t>(limits.min_rate) ||
        rate > static_cast<std::uint32_t>(limits.max_rate))
        return none(error, "WAV sample rate out of range");
    const std::size_t sample_bytes = bits / 8U;
    if (block_align != sample_bytes * channels)
        return none(error, "WAV block alignment does not match its format");
    const std::size_t frames = data.size() / block_align;
    if (static_cast<double>(frames) / rate > limits.max_seconds)
        return none(error, "sound longer than " +
                               std::to_string(static_cast<int>(limits.max_seconds)) + " seconds");
    PcmBuffer result;
    result.channels = channels == 1U ? 1 : 2;
    result.sample_rate = static_cast<int>(rate);
    result.samples.resize(frames * static_cast<std::size_t>(result.channels));
    const auto read_sample = [&](const std::uint8_t *sample) -> float {
        if (floating)
        {
            float value{};
            std::memcpy(&value, sample, 4U);
            return std::isfinite(value) ? value : 0.0F;
        }
        switch (bits)
        {
        case 8U:
            return (static_cast<float>(sample[0]) - 128.0F) / 128.0F;
        case 16U:
            return static_cast<float>(static_cast<std::int16_t>(read_u16(sample))) / 32768.0F;
        case 24U: {
            std::int32_t value =
                static_cast<std::int32_t>(sample[0] | (sample[1] << 8U) | (sample[2] << 16U));
            if ((value & 0x800000) != 0)
                value |= ~0xFFFFFF;
            return static_cast<float>(value) / 8388608.0F;
        }
        default:
            return static_cast<float>(static_cast<std::int32_t>(read_u32(sample))) / 2147483648.0F;
        }
    };
    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        const std::uint8_t *base = data.data() + frame * block_align;
        if (result.channels == 1)
            result.samples[frame] = to_sample(read_sample(base));
        else
        {
            result.samples[frame * 2U] = to_sample(read_sample(base));
            result.samples[frame * 2U + 1U] = to_sample(read_sample(base + sample_bytes));
        }
    }
    return result;
}

std::optional<PcmBuffer> decode_ogg(std::span<const std::uint8_t> bytes, std::string *error,
                                    const AudioLimits &limits)
{
    if (bytes.size() > limits.max_file_bytes)
        return none(error, "Ogg file too large");
    VorbisPointer stream = open_vorbis(bytes, error);
    if (stream == nullptr)
        return std::nullopt;
    const stb_vorbis_info info = stb_vorbis_get_info(stream.get());
    if (info.channels <= 0 || info.channels > 8)
        return none(error, "Ogg channel count out of range");
    if (info.sample_rate < static_cast<unsigned>(limits.min_rate) ||
        info.sample_rate > static_cast<unsigned>(limits.max_rate))
        return none(error, "Ogg sample rate out of range");
    const int channels = info.channels == 1 ? 1 : 2;
    // The declared length may lie; decode no further than the limit allows.
    const std::size_t limit_frames =
        static_cast<std::size_t>(limits.max_seconds * static_cast<double>(info.sample_rate)) + 1U;
    PcmBuffer result;
    result.channels = channels;
    result.sample_rate = static_cast<int>(info.sample_rate);
    std::vector<short> chunk(4096U * static_cast<std::size_t>(channels));
    for (;;)
    {
        const int got = stb_vorbis_get_samples_short_interleaved(
            stream.get(), channels, chunk.data(), static_cast<int>(chunk.size()));
        if (got <= 0)
            break;
        if (result.frames() + static_cast<std::size_t>(got) > limit_frames)
            return none(error, "sound longer than " +
                                   std::to_string(static_cast<int>(limits.max_seconds)) +
                                   " seconds");
        result.samples.insert(result.samples.end(), chunk.begin(),
                              chunk.begin() + static_cast<std::ptrdiff_t>(got) * channels);
    }
    if (result.samples.empty())
        return none(error, "Ogg file has no audio");
    return result;
}

std::optional<PcmBuffer> decode_audio(std::span<const std::uint8_t> bytes, std::string *error,
                                      const AudioLimits &limits)
{
    if (bytes.size() >= 4U && std::memcmp(bytes.data(), "OggS", 4U) == 0)
        return decode_ogg(bytes, error, limits);
    if (bytes.size() >= 4U && std::memcmp(bytes.data(), "RIFF", 4U) == 0)
        return decode_wav(bytes, error, limits);
    fail(error, "not a WAV or Ogg Vorbis file");
    return std::nullopt;
}

std::shared_ptr<SoundAsset> SoundAsset::from_pcm(PcmBuffer pcm)
{
    if (pcm.channels <= 0 || pcm.sample_rate <= 0 || pcm.samples.empty())
        return nullptr;
    auto asset = std::make_shared<SoundAsset>();
    asset->channels_ = pcm.channels;
    asset->sample_rate_ = pcm.sample_rate;
    asset->pcm_ = std::move(pcm);
    return asset;
}

std::shared_ptr<SoundAsset> SoundAsset::from_ogg_stream(std::vector<std::uint8_t> bytes,
                                                        std::string *error)
{
    VorbisPointer stream = open_vorbis(bytes, error);
    if (stream == nullptr)
        return nullptr;
    const stb_vorbis_info info = stb_vorbis_get_info(stream.get());
    if (info.channels <= 0 || info.channels > 8 || info.sample_rate < 8000U ||
        info.sample_rate > 192000U)
    {
        fail(error, "Ogg stream format out of range");
        return nullptr;
    }
    auto asset = std::make_shared<SoundAsset>();
    asset->channels_ = info.channels == 1 ? 1 : 2;
    asset->sample_rate_ = static_cast<int>(info.sample_rate);
    asset->ogg_ = std::move(bytes);
    return asset;
}

SpatialGain spatial_gain(Vec3 listener, float listener_yaw, Vec3 source, float min_distance,
                         float max_distance) noexcept
{
    min_distance = std::clamp(std::isfinite(min_distance) ? min_distance : 2.0F, 0.1F, 256.0F);
    max_distance =
        std::clamp(std::isfinite(max_distance) ? max_distance : 32.0F, min_distance + 0.1F, 512.0F);
    const Vec3 offset = source - listener;
    const float distance = vfx::length(offset);
    if (!std::isfinite(distance) || distance >= max_distance)
        return {0.0F, 0.0F};
    float gain = std::clamp(min_distance / std::max(distance, min_distance), 0.0F, 1.0F);
    // Fade the last fifth of the range to silence instead of cutting off.
    gain *= std::clamp((max_distance - distance) / (0.2F * max_distance), 0.0F, 1.0F);
    const Vec3 right{std::cos(listener_yaw), 0.0F, std::sin(listener_yaw)};
    float pan = distance > 1.0e-4F ? vfx::dot(offset, right) / distance : 0.0F;
    pan *= std::clamp(distance / min_distance, 0.0F, 1.0F);
    const float angle = (std::clamp(pan, -1.0F, 1.0F) + 1.0F) * std::numbers::pi_v<float> * 0.25F;
    constexpr float root_two = std::numbers::sqrt2_v<float>;
    return {std::min(1.0F, std::cos(angle) * root_two) * gain,
            std::min(1.0F, std::sin(angle) * root_two) * gain};
}

// --- The mixer -------------------------------------------------------------

struct Mixer::Voice final
{
    std::uint64_t id{};
    std::shared_ptr<const SoundAsset> sound;
    VoiceParams params;
    bool music{};
    // Source position in frames (absolute for streams).
    double cursor{};
    float envelope{};
    float envelope_target{1.0F};
    float envelope_rate{};
    bool stopping{};
    bool finished{};
    // Streams: decoded frames from `window_start`.
    VorbisPointer stream;
    std::vector<std::int16_t> window;
    std::size_t window_start{};
    bool stream_ended{};

    [[nodiscard]] std::size_t window_frames() const noexcept
    {
        return window.size() / static_cast<std::size_t>(sound->channels());
    }

    // Frame `index` (absolute) as stereo floats, or false past the end.
    bool frame(std::size_t index, float &left, float &right)
    {
        const int channels = sound->channels();
        if (!sound->streamed())
        {
            const PcmBuffer &pcm = sound->pcm();
            const std::size_t frames = pcm.frames();
            if (frames == 0U)
                return false;
            if (index >= frames)
            {
                if (!params.loop)
                    return false;
                index %= frames;
            }
            const std::int16_t *sample =
                pcm.samples.data() + index * static_cast<std::size_t>(channels);
            left = static_cast<float>(sample[0]) / 32768.0F;
            right = channels > 1 ? static_cast<float>(sample[1]) / 32768.0F : left;
            return true;
        }
        while (index >= window_start + window_frames())
        {
            if (!decode_more())
                return false;
        }
        if (index < window_start)
            return false;
        const std::int16_t *sample =
            window.data() + (index - window_start) * static_cast<std::size_t>(channels);
        left = static_cast<float>(sample[0]) / 32768.0F;
        right = channels > 1 ? static_cast<float>(sample[1]) / 32768.0F : left;
        return true;
    }

    bool decode_more()
    {
        if (stream == nullptr || stream_ended)
            return false;
        const int channels = sound->channels();
        // Drop frames the cursor has passed, keeping one for interpolation.
        const auto keep_from = static_cast<std::size_t>(std::max(0.0, std::floor(cursor) - 1.0));
        if (keep_from > window_start + 8192U)
        {
            const std::size_t drop = std::min(keep_from - window_start, window_frames());
            window.erase(window.begin(),
                         window.begin() + static_cast<std::ptrdiff_t>(drop * channels));
            window_start += drop;
        }
        std::array<short, 4096> chunk{};
        int got = stb_vorbis_get_samples_short_interleaved(stream.get(), channels, chunk.data(),
                                                           static_cast<int>(chunk.size()));
        if (got <= 0 && params.loop)
        {
            stb_vorbis_seek_start(stream.get());
            got = stb_vorbis_get_samples_short_interleaved(stream.get(), channels, chunk.data(),
                                                           static_cast<int>(chunk.size()));
        }
        if (got <= 0)
        {
            stream_ended = true;
            return false;
        }
        window.insert(window.end(), chunk.begin(),
                      chunk.begin() + static_cast<std::ptrdiff_t>(got) * channels);
        return true;
    }
};

Mixer::Mixer(int output_rate) : output_rate_(std::clamp(output_rate, 8000, 192000))
{
}

Mixer::~Mixer() = default;

Mixer::Voice *Mixer::find_voice(std::uint64_t id)
{
    for (const auto &voice : voices_)
        if (voice->id == id)
            return voice.get();
    return nullptr;
}

const Mixer::Voice *Mixer::find_voice(std::uint64_t id) const
{
    for (const auto &voice : voices_)
        if (voice->id == id)
            return voice.get();
    return nullptr;
}

std::uint64_t Mixer::start_voice(std::shared_ptr<const SoundAsset> sound, const VoiceParams &params,
                                 bool music)
{
    if (sound == nullptr || voices_.size() >= max_voices)
        return 0U;
    auto voice = std::make_unique<Voice>();
    voice->id = next_id_++;
    voice->params = params;
    voice->params.volume =
        std::clamp(std::isfinite(params.volume) ? params.volume : 1.0F, 0.0F, 4.0F);
    voice->params.pitch =
        std::clamp(std::isfinite(params.pitch) ? params.pitch : 1.0F, 0.25F, 4.0F);
    voice->music = music;
    if (params.fade_in > 0.0F && std::isfinite(params.fade_in))
    {
        voice->envelope = 0.0F;
        voice->envelope_rate = 1.0F / std::min(params.fade_in, 30.0F);
    }
    else
        voice->envelope = 1.0F;
    if (sound->streamed())
    {
        voice->stream = open_vorbis(sound->ogg(), nullptr);
        if (voice->stream == nullptr)
            return 0U;
    }
    voice->sound = std::move(sound);
    voices_.push_back(std::move(voice));
    return voices_.back()->id;
}

std::uint64_t Mixer::play(std::shared_ptr<const SoundAsset> sound, const VoiceParams &params)
{
    const std::lock_guard lock{mutex_};
    const std::size_t frames = sound != nullptr && !sound->streamed() ? sound->pcm().frames() : 0U;
    const std::uint64_t voice = start_voice(std::move(sound), params, false);
    if (recorder_ != nullptr)
        record(std::format("play voice={} bus={} volume={:.2f} positioned={} loop={} frames={}",
                           voice,
                           params.bus == AudioBus::ambience ? "ambience"
                           : params.bus == AudioBus::music  ? "music"
                                                            : "effects",
                           params.volume, params.position.has_value() ? 1 : 0,
                           params.loop ? 1 : 0, frames));
    return voice;
}

void Mixer::stop(std::uint64_t voice, float fade_seconds)
{
    const std::lock_guard lock{mutex_};
    if (Voice *found = find_voice(voice))
    {
        found->stopping = true;
        found->envelope_target = 0.0F;
        if (fade_seconds > 0.0F && std::isfinite(fade_seconds))
            found->envelope_rate = 1.0F / std::min(fade_seconds, 30.0F);
        else
            found->finished = true;
        if (recorder_ != nullptr)
            record(std::format("stop voice={} fade={:.2f}", voice, fade_seconds));
    }
}

void Mixer::set_position(std::uint64_t voice, Vec3 position)
{
    const std::lock_guard lock{mutex_};
    if (Voice *found = find_voice(voice))
        found->params.position = position;
}

void Mixer::set_volume(std::uint64_t voice, float volume)
{
    const std::lock_guard lock{mutex_};
    if (Voice *found = find_voice(voice))
        found->params.volume = std::clamp(std::isfinite(volume) ? volume : 1.0F, 0.0F, 4.0F);
}

bool Mixer::playing(std::uint64_t voice) const
{
    const std::lock_guard lock{mutex_};
    const Voice *found = find_voice(voice);
    return found != nullptr && !found->finished;
}

bool Mixer::music_playing() const
{
    const std::lock_guard lock{mutex_};
    const Voice *found = find_voice(music_voice_);
    return found != nullptr && !found->finished && !found->stopping;
}

void Mixer::play_music(std::shared_ptr<const SoundAsset> track, float crossfade_seconds,
                       float volume, bool loop)
{
    const std::lock_guard lock{mutex_};
    const float fade =
        std::isfinite(crossfade_seconds) ? std::clamp(crossfade_seconds, 0.0F, 30.0F) : 0.0F;
    if (Voice *current = find_voice(music_voice_))
    {
        current->stopping = true;
        current->envelope_target = 0.0F;
        if (fade > 0.0F)
            current->envelope_rate = 1.0F / fade;
        else
            current->finished = true;
    }
    const std::uint64_t previous = std::exchange(music_voice_, 0U);
    if (track == nullptr)
    {
        if (recorder_ != nullptr)
            record(std::format("music stop voice={} crossfade={:.2f}", previous, fade));
        return;
    }
    VoiceParams params;
    params.volume = volume;
    params.loop = loop;
    params.bus = AudioBus::music;
    params.fade_in = fade;
    music_voice_ = start_voice(std::move(track), params, true);
    if (recorder_ != nullptr)
        record(std::format("music voice={} replaces={} crossfade={:.2f} volume={:.2f}",
                           music_voice_, previous, fade, volume));
}

void Mixer::set_listener(Vec3 position, float yaw)
{
    const std::lock_guard lock{mutex_};
    listener_ = position;
    listener_yaw_ = yaw;
}

void Mixer::set_levels(const MixLevels &levels)
{
    const std::lock_guard lock{mutex_};
    levels_ = levels;
}

void Mixer::stop_all()
{
    const std::lock_guard lock{mutex_};
    voices_.clear();
    music_voice_ = 0U;
}

std::size_t Mixer::voice_count() const
{
    const std::lock_guard lock{mutex_};
    return static_cast<std::size_t>(std::count_if(
        voices_.begin(), voices_.end(), [](const auto &voice) { return !voice->finished; }));
}

void Mixer::mix(float *output, std::size_t frames)
{
    if (output == nullptr || frames == 0U)
        return;
    std::fill(output, output + frames * 2U, 0.0F);
    const std::lock_guard lock{mutex_};
    const float master = levels_.muted ? 0.0F : std::clamp(levels_.master, 0.0F, 1.0F);
    const float out_rate = static_cast<float>(output_rate_);
    for (const auto &pointer : voices_)
    {
        Voice &voice = *pointer;
        if (voice.finished)
            continue;
        const float bus = voice.params.bus == AudioBus::music      ? levels_.music
                          : voice.params.bus == AudioBus::ambience ? levels_.ambience
                                                                   : levels_.effects;
        float gain_left = voice.params.volume * std::clamp(bus, 0.0F, 1.0F) * master;
        float gain_right = gain_left;
        const bool positioned = voice.params.position.has_value();
        if (positioned)
        {
            const SpatialGain spatial =
                spatial_gain(listener_, listener_yaw_, *voice.params.position,
                             voice.params.min_distance, voice.params.max_distance);
            gain_left *= spatial.left;
            gain_right *= spatial.right;
        }
        const double step =
            static_cast<double>(voice.sound->sample_rate()) / out_rate * voice.params.pitch;
        const float envelope_step = voice.envelope_rate / out_rate;
        for (std::size_t frame = 0U; frame < frames; ++frame)
        {
            const auto index = static_cast<std::size_t>(voice.cursor);
            const float t = static_cast<float>(voice.cursor - static_cast<double>(index));
            float l0{}, r0{}, l1{}, r1{};
            if (!voice.frame(index, l0, r0))
            {
                voice.finished = true;
                break;
            }
            if (!voice.frame(index + 1U, l1, r1))
            {
                l1 = l0;
                r1 = r0;
            }
            float left = l0 + (l1 - l0) * t;
            float right = r0 + (r1 - r0) * t;
            if (positioned)
                left = right = (left + right) * 0.5F;
            // Fades.
            if (voice.envelope < voice.envelope_target)
                voice.envelope = std::min(voice.envelope_target, voice.envelope + envelope_step);
            else if (voice.envelope > voice.envelope_target)
                voice.envelope = std::max(voice.envelope_target, voice.envelope - envelope_step);
            output[frame * 2U] += left * gain_left * voice.envelope;
            output[frame * 2U + 1U] += right * gain_right * voice.envelope;
            voice.cursor += step;
            if (!voice.sound->streamed() && voice.params.loop)
            {
                const auto total = static_cast<double>(voice.sound->pcm().frames());
                if (voice.cursor >= total)
                    voice.cursor = std::fmod(voice.cursor, total);
            }
            if (voice.stopping && voice.envelope <= 0.0F)
            {
                voice.finished = true;
                break;
            }
        }
    }
    std::erase_if(voices_, [](const auto &voice) { return voice->finished; });
    for (std::size_t sample = 0U; sample < frames * 2U; ++sample)
        output[sample] = std::clamp(output[sample], -1.0F, 1.0F);
    if (recorder_ != nullptr)
        recorder_->capture(output, frames);
    mixed_ += frames;
}

void Mixer::set_recorder(MixRecorder *recorder)
{
    const std::lock_guard lock{mutex_};
    recorder_ = recorder;
    recorder_start_ = mixed_;
}

void Mixer::mark(std::string_view text)
{
    const std::lock_guard lock{mutex_};
    if (recorder_ != nullptr)
        record(std::string{text});
}

// What a call changed is heard from the next mixed block on: the frame the
// recorder captures next.
void Mixer::record(std::string text)
{
    recorder_->mark(mixed_ - recorder_start_, std::move(text));
}

// --- MixRecorder (Polish) ----------------------------------------------------

MixRecorder::MixRecorder(int sample_rate, double seconds)
    : rate_(std::clamp(sample_rate, 8000, 192000)), made_(std::chrono::steady_clock::now())
{
    const double length = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 3600.0) : 0.0;
    samples_.resize(static_cast<std::size_t>(length * rate_) * 2U);
}

void MixRecorder::capture(const float *interleaved, std::size_t count) noexcept
{
    const std::size_t captured = captured_.load(std::memory_order_relaxed);
    if (interleaved == nullptr || captured * 2U >= samples_.size())
        return;
    if (first_block_seconds_ < 0.0)
        first_block_seconds_ =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - made_).count();
    const std::size_t kept = std::min(count, samples_.size() / 2U - captured);
    std::int16_t *out = samples_.data() + captured * 2U;
    for (std::size_t sample = 0U; sample < kept * 2U; ++sample)
        out[sample] = static_cast<std::int16_t>(
            std::lround(std::clamp(interleaved[sample], -1.0F, 1.0F) * 32767.0F));
    // The game thread polls full() without the mixer's lock.
    captured_.store(captured + kept, std::memory_order_release);
}

void MixRecorder::mark(std::uint64_t frame, std::string text)
{
    marks_.push_back(
        {frame, std::chrono::duration<double>(std::chrono::steady_clock::now() - made_).count(),
         std::move(text)});
}

bool MixRecorder::write(const std::filesystem::path &wav, std::string &error) const
{
    std::ofstream out{wav, std::ios::binary | std::ios::trunc};
    const auto put = [&out](std::uint32_t value, int bytes) {
        for (int index{}; index < bytes; ++index)
            out.put(static_cast<char>((value >> (8 * index)) & 0xFFU));
    };
    const std::size_t captured = frames();
    const auto data_bytes = static_cast<std::uint32_t>(captured * 4U);
    out.write("RIFF", 4);
    put(36U + data_bytes, 4);
    out.write("WAVEfmt ", 8);
    put(16U, 4);
    put(1U, 2); // PCM
    put(2U, 2);
    put(static_cast<std::uint32_t>(rate_), 4);
    put(static_cast<std::uint32_t>(rate_) * 4U, 4);
    put(4U, 2);
    put(16U, 2);
    out.write("data", 4);
    put(data_bytes, 4);
    static_assert(std::endian::native == std::endian::little, "WAV samples are little-endian");
    out.write(reinterpret_cast<const char *>(samples_.data()),
              static_cast<std::streamsize>(captured * 4U));
    if (!out)
    {
        error = "cannot write " + wav.string();
        return false;
    }
    std::filesystem::path notes = wav;
    notes += ".marks.txt";
    std::ofstream marks{notes, std::ios::trunc};
    marks << std::format("# {} Hz stereo, {} frames ({:.3f} s); first block mixed {:.3f} s "
                         "after the recorder was made\n# frame audio_s wall_s what\n",
                         rate_, captured, static_cast<double>(captured) / rate_,
                         first_block_seconds_);
    for (const Mark &mark : marks_)
        marks << std::format("{} {:.4f} {:.4f} {}\n", mark.frame,
                             static_cast<double>(mark.frame) / rate_, mark.seconds, mark.text);
    if (!marks)
    {
        error = "cannot write " + notes.string();
        return false;
    }
    return true;
}

} // namespace openminerz::modding::graphics
