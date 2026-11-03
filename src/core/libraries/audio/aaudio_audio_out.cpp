// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <vector>

#include "common/android_host.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"

namespace Libraries::AudioOut {

constexpr float INV_VOLUME_0DB = 1.0f / 32768.0f;
constexpr u32 OutputChannels = 2;
constexpr u32 LatencyMs = 60;

class AAudioPortBackend : public PortBackend {
public:
    explicit AAudioPortBackend(const PortOut& port)
        : buffer_frames(port.buffer_frames), num_channels(port.format_info.num_channels),
          is_float(port.format_info.is_float), samples(buffer_frames * OutputChannels) {
        stream = Common::AndroidHost::AAudioOpenOutput(port.sample_rate, OutputChannels, LatencyMs);
        if (!stream) {
            LOG_ERROR(Lib_AudioOut, "Failed to open an AAudio stream for port type {}",
                      static_cast<int>(port.type));
            return;
        }
        LOG_INFO(Lib_AudioOut, "Opened AAudio stream: {} Hz, {} ch {} in, stereo float out",
                 port.sample_rate, num_channels, is_float ? "float" : "s16");
    }

    ~AAudioPortBackend() override {
        Common::AndroidHost::AAudioCloseOutput(stream);
    }

    void Output(void* ptr) override {
        if (!stream || !ptr) [[unlikely]] {
            return;
        }
        const float gain = game_gain.load(std::memory_order_relaxed) *
                           EmulatorSettings.GetVolumeSlider() * 0.01f;
        if (is_float) {
            Downmix(static_cast<const float*>(ptr), gain);
        } else {
            Downmix(static_cast<const s16*>(ptr), INV_VOLUME_0DB * gain);
        }
        Common::AndroidHost::AAudioWrite(stream, samples.data(), buffer_frames, OutputChannels);
    }

    void SetVolume(const std::array<int, 8>& ch_volumes) override {
        float max_channel_gain = 0.0f;
        for (u32 i = 0; i < std::min(num_channels, 8u); i++) {
            max_channel_gain =
                std::max(max_channel_gain, static_cast<float>(ch_volumes[i]) * INV_VOLUME_0DB);
        }
        game_gain.store(max_channel_gain, std::memory_order_relaxed);
    }

private:
    template <typename T>
    void Downmix(const T* src, float scale) {
        float* dst = samples.data();
        switch (num_channels) {
        case 1:
            for (u32 i = 0; i < buffer_frames; i++) {
                dst[i * 2] = dst[i * 2 + 1] = src[i] * scale;
            }
            break;
        case 2:
            for (u32 i = 0; i < buffer_frames * 2; i++) {
                dst[i] = src[i] * scale;
            }
            break;
        default: {
            // Front pair, centre and both surround pairs, as the SDL backend folds 5.1 and 7.1.
            constexpr float Centre = 0.7071f;
            constexpr float Surround = 0.7071f;
            for (u32 i = 0; i < buffer_frames; i++) {
                const T* s = src + i * num_channels;
                const float centre = Centre * s[2];
                dst[i * 2] = (s[0] + centre + Surround * (s[4] + s[6])) * scale;
                dst[i * 2 + 1] = (s[1] + centre + Surround * (s[5] + s[7])) * scale;
            }
            break;
        }
        }
    }

    const u32 buffer_frames;
    const u32 num_channels;
    const bool is_float;
    std::vector<float> samples;
    std::atomic<float> game_gain{1.0f};
    void* stream{};
};

std::unique_ptr<PortBackend> AAudioOut::Open(PortOut& port) {
    return std::make_unique<AAudioPortBackend>(port);
}

} // namespace Libraries::AudioOut
