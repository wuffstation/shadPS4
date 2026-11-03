// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "common/android_host.h"
#include "common/logging/log.h"

#if defined(__linux__)
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#define ANDROID_HOST_REACHABLE 1
#else
#define ANDROID_HOST_REACHABLE 0
#endif

namespace Common::AndroidHost {

static bool IsSelected(const char* variable) {
    const char* value = std::getenv(variable);
    if (value == nullptr) {
        return false;
    }
    const char* expected = "android";
    for (; *value != '\0' && *expected != '\0'; value++, expected++) {
        if ((*value | 0x20) != *expected) {
            return false;
        }
    }
    return *value == '\0' && *expected == '\0';
}

bool WindowSelected() {
    static const bool selected = IsSelected("SHADPS4_HOST_WINDOW");
    return selected;
}

bool AudioSelected() {
    static const bool selected = IsSelected("SHADPS4_HOST_AUDIO");
    return selected;
}

bool InputSelected() {
    static const bool selected = IsSelected("SHADPS4_HOST_INPUT");
    return selected;
}

bool WindowSize(s32* width, s32* height) {
    const char* value = std::getenv("SHADPS4_HOST_WINDOW_SIZE");
    int w = 0;
    int h = 0;
    if (value == nullptr || std::sscanf(value, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

// The pad bridge is a syscall with a magic number, answered by the host rather than a kernel. Its
// wire format is versioned and size-checked by every read, since the two sides live apart.
constexpr long PadBridgeMagic = 0x50440000;
constexpr long PadWireVersion = 1;

s64 PadRead(PadWireState* state) {
#if ANDROID_HOST_REACHABLE
    return ::syscall(PadBridgeMagic | 0, PadWireVersion, state, static_cast<long>(sizeof(*state)));
#else
    return -1;
#endif
}

void PadRumble(u8 large_motor, u8 small_motor) {
#if ANDROID_HOST_REACHABLE
    ::syscall(PadBridgeMagic | 1, static_cast<long>(large_motor), static_cast<long>(small_motor));
#endif
}

// The guest-side libaaudio.so the host stages forwards each export to the host's AAudio. The data
// callback is never used: nothing on the host side may call into this process.
constexpr s32 AAUDIO_OK = 0;
constexpr s32 AAUDIO_FORMAT_PCM_FLOAT = 2;
constexpr s32 AAUDIO_DIRECTION_OUTPUT = 0;
constexpr s32 AAUDIO_SHARING_MODE_SHARED = 1;
constexpr s32 AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12;
constexpr s32 AAUDIO_USAGE_GAME = 14;

struct AAudioApi {
    s32 (*create_stream_builder)(void** builder);
    void (*set_sample_rate)(void* builder, s32 rate);
    void (*set_channel_count)(void* builder, s32 channels);
    void (*set_format)(void* builder, s32 format);
    void (*set_direction)(void* builder, s32 direction);
    void (*set_sharing_mode)(void* builder, s32 mode);
    void (*set_performance_mode)(void* builder, s32 mode);
    void (*set_usage)(void* builder, s32 usage);
    s32 (*open_stream)(void* builder, void** stream);
    s32 (*delete_builder)(void* builder);
    s32 (*request_start)(void* stream);
    s32 (*request_stop)(void* stream);
    s32 (*close)(void* stream);
    s32 (*write)(void* stream, const void* data, s32 frames, s64 timeout_ns);
    s32 (*get_buffer_capacity)(void* stream);
    s32 (*set_buffer_size)(void* stream, s32 frames);
    s32 (*get_sample_rate)(void* stream);
    s32 (*get_channel_count)(void* stream);
    s32 (*get_format)(void* stream);
};

static const AAudioApi* GetAAudio() {
#if ANDROID_HOST_REACHABLE
    static const AAudioApi* api = []() -> const AAudioApi* {
        void* library = dlopen("libaaudio.so", RTLD_NOW);
        if (library == nullptr) {
            LOG_ERROR(Lib_AudioOut, "libaaudio.so: {}", dlerror());
            return nullptr;
        }
        static AAudioApi resolved{};
        bool complete = true;
        const auto bind = [&](auto& slot, const char* name) {
            slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(dlsym(library, name));
            if (slot == nullptr) {
                LOG_ERROR(Lib_AudioOut, "AAudio is missing {}", name);
                complete = false;
            }
        };
        bind(resolved.create_stream_builder, "AAudio_createStreamBuilder");
        bind(resolved.set_sample_rate, "AAudioStreamBuilder_setSampleRate");
        bind(resolved.set_channel_count, "AAudioStreamBuilder_setChannelCount");
        bind(resolved.set_format, "AAudioStreamBuilder_setFormat");
        bind(resolved.set_direction, "AAudioStreamBuilder_setDirection");
        bind(resolved.set_sharing_mode, "AAudioStreamBuilder_setSharingMode");
        bind(resolved.set_performance_mode, "AAudioStreamBuilder_setPerformanceMode");
        bind(resolved.set_usage, "AAudioStreamBuilder_setUsage");
        bind(resolved.open_stream, "AAudioStreamBuilder_openStream");
        bind(resolved.delete_builder, "AAudioStreamBuilder_delete");
        bind(resolved.request_start, "AAudioStream_requestStart");
        bind(resolved.request_stop, "AAudioStream_requestStop");
        bind(resolved.close, "AAudioStream_close");
        bind(resolved.write, "AAudioStream_write");
        bind(resolved.get_buffer_capacity, "AAudioStream_getBufferCapacityInFrames");
        bind(resolved.set_buffer_size, "AAudioStream_setBufferSizeInFrames");
        bind(resolved.get_sample_rate, "AAudioStream_getSampleRate");
        bind(resolved.get_channel_count, "AAudioStream_getChannelCount");
        bind(resolved.get_format, "AAudioStream_getFormat");
        return complete ? &resolved : nullptr;
    }();
    return api;
#else
    return nullptr;
#endif
}

void* AAudioOpenOutput(u32 sample_rate, u32 channels, u32 latency_ms) {
    const auto* api = GetAAudio();
    if (api == nullptr) {
        return nullptr;
    }
    void* builder = nullptr;
    if (api->create_stream_builder(&builder) != AAUDIO_OK || builder == nullptr) {
        return nullptr;
    }
    api->set_sample_rate(builder, static_cast<s32>(sample_rate));
    api->set_channel_count(builder, static_cast<s32>(channels));
    api->set_format(builder, AAUDIO_FORMAT_PCM_FLOAT);
    api->set_direction(builder, AAUDIO_DIRECTION_OUTPUT);
    api->set_sharing_mode(builder, AAUDIO_SHARING_MODE_SHARED);
    api->set_performance_mode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    api->set_usage(builder, AAUDIO_USAGE_GAME);
    void* stream = nullptr;
    const s32 status = api->open_stream(builder, &stream);
    api->delete_builder(builder);
    if (status != AAUDIO_OK || stream == nullptr) {
        LOG_ERROR(Lib_AudioOut, "AAudio open failed: {}", status);
        return nullptr;
    }
    if (api->get_sample_rate(stream) != static_cast<s32>(sample_rate) ||
        api->get_channel_count(stream) != static_cast<s32>(channels) ||
        api->get_format(stream) != AAUDIO_FORMAT_PCM_FLOAT) {
        LOG_ERROR(Lib_AudioOut, "AAudio negotiated {} Hz {} ch format {} for {} Hz {} ch float",
                  api->get_sample_rate(stream), api->get_channel_count(stream),
                  api->get_format(stream), sample_rate, channels);
        api->close(stream);
        return nullptr;
    }
    const s32 wanted = static_cast<s32>(sample_rate * latency_ms / 1000);
    const s32 capacity = api->get_buffer_capacity(stream);
    if (wanted > 0 && capacity > 0) {
        api->set_buffer_size(stream, wanted < capacity ? wanted : capacity);
    }
    if (api->request_start(stream) != AAUDIO_OK) {
        api->close(stream);
        return nullptr;
    }
    return stream;
}

void AAudioCloseOutput(void* stream) {
    const auto* api = GetAAudio();
    if (api != nullptr && stream != nullptr) {
        api->request_stop(stream);
        api->close(stream);
    }
}

bool AAudioWrite(void* stream, const float* samples, u32 frames, u32 channels) {
#if ANDROID_HOST_REACHABLE
    const auto* api = GetAAudio();
    if (api == nullptr || stream == nullptr) {
        return false;
    }
    // The write itself never waits: the host delivers signals to a guest thread only at syscall
    // exits, and one parked inside a host call cannot be reached. So the waiting is ours.
    constexpr s64 DeadlineNs = 250'000'000;
    const auto now_ns = [] {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<s64>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
    };
    const s64 start = now_ns();
    u32 done = 0;
    while (done < frames) {
        const s32 written = api->write(stream, samples + static_cast<u64>(done) * channels,
                                       static_cast<s32>(frames - done), 0);
        if (written < 0) {
            return false;
        }
        done += static_cast<u32>(written);
        if (done >= frames || now_ns() - start > DeadlineNs) {
            break;
        }
        timespec pause{0, 1'000'000};
        nanosleep(&pause, nullptr);
    }
    return true;
#else
    return false;
#endif
}

} // namespace Common::AndroidHost
