// XAudio render driver: the game's audio library mixes 256-sample frames
// (6 channels, big-endian float, one channel after another) and hands each to
// XAudioSubmitRenderDriverFrame; we play them through XAudio2 (Windows).
//
// Pacing: the game's callback is called once per frame of free space in the
// output queue, so the game produces audio exactly as fast as it plays.
// Without an audio device it is driven by a timer at the console rate and the
// frames are dropped.
#include "kernel.h"
#include "memory.h"
#include "thread.h"
#include "xma.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")
#endif

namespace
{
    constexpr uint32_t DRIVER_HANDLE = 0x41550000;
    constexpr uint32_t kChannels = 6, kFrameSamples = 256, kSampleRate = 48000;
    constexpr uint32_t kQueuedFrames = 8; // ~43 ms of buffering

    std::atomic<uint32_t> g_callback{ 0 }, g_callbackContext{ 0 };
    std::atomic<bool> g_running{ false };
    std::atomic<uint64_t> g_framesSubmitted{ 0 };

    // Free output slots (frames that have finished playing).
    std::mutex g_slotMutex;
    std::condition_variable g_slotCv;
    uint32_t g_freeSlots = kQueuedFrames;

#ifdef _WIN32
    struct VoiceCallback : IXAudio2VoiceCallback
    {
        void STDMETHODCALLTYPE OnBufferEnd(void*) override
        {
            {
                std::lock_guard lock(g_slotMutex);
                g_freeSlots++;
            }
            g_slotCv.notify_one();
        }
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
    };

    IXAudio2* g_xaudio = nullptr;
    IXAudio2MasteringVoice* g_master = nullptr;
    IXAudio2SourceVoice* g_voice = nullptr;
    VoiceCallback g_voiceCallback;
    std::vector<float> g_buffers(size_t(kQueuedFrames) * kChannels * kFrameSamples);
    uint32_t g_nextBuffer = 0;

    bool InitOutput()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        {
            LOG("[audio] XAudio2 unavailable - no sound");
            return false;
        }
        if (FAILED(g_xaudio->CreateMasteringVoice(&g_master)))
        {
            LOG("[audio] no audio output device - no sound");
            return false;
        }
        WAVEFORMATEXTENSIBLE fmt = {};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = kChannels;
        fmt.Format.nSamplesPerSec = kSampleRate;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = kChannels * 4;
        fmt.Format.nAvgBytesPerSec = kSampleRate * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(fmt) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                            SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (FAILED(g_xaudio->CreateSourceVoice(&g_voice, &fmt.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &g_voiceCallback)))
        {
            LOG("[audio] couldn't create the output voice - no sound");
            return false;
        }
        g_voice->Start();
        LOG("[audio] XAudio2 output: 48 kHz, 5.1");
        return true;
    }

    void Submit(uint32_t guestSamples)
    {
        if (!g_voice)
            return;
        float* out = &g_buffers[size_t(g_nextBuffer) * kChannels * kFrameSamples];
        g_nextBuffer = (g_nextBuffer + 1) % kQueuedFrames;
        const uint8_t* in = g_base + guestSamples;
        // Planar big-endian -> interleaved little-endian.
        for (uint32_t s = 0; s < kFrameSamples; s++)
            for (uint32_t c = 0; c < kChannels; c++)
            {
                uint32_t v;
                memcpy(&v, in + (size_t(c) * kFrameSamples + s) * 4, 4);
                v = __builtin_bswap32(v);
                memcpy(&out[s * kChannels + c], &v, 4);
            }
        XAUDIO2_BUFFER b = {};
        b.AudioBytes = kChannels * kFrameSamples * 4;
        b.pAudioData = reinterpret_cast<const BYTE*>(out);
        if (FAILED(g_voice->SubmitSourceBuffer(&b)))
        {
            std::lock_guard lock(g_slotMutex);
            g_freeSlots++; // never queued, so no OnBufferEnd for it
        }
    }
#else
    bool InitOutput() { return false; }
    void Submit(uint32_t) {}
#endif

    bool g_haveOutput = false;

    void AudioThread()
    {
        ProfileRegisterCurrentThread("audio");
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        while (g_running)
        {
            if (g_haveOutput)
            {
                // Wait for room, then let the game mix the next frame.
                std::unique_lock lock(g_slotMutex);
                if (!g_slotCv.wait_for(lock, std::chrono::milliseconds(100), [] { return g_freeSlots > 0; }))
                    continue;
                g_freeSlots--;
            }
            else
            {
                next += std::chrono::nanoseconds(5333333); // 256 / 48000 s
                std::this_thread::sleep_until(next);
            }
            if (uint32_t cb = g_callback.load())
                CallGuest(cb, g_callbackContext.load());
            else if (g_haveOutput)
            {
                std::lock_guard lock(g_slotMutex);
                g_freeSlots++;
            }
        }
    }
}

GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    // (callback_info* {be32 callback, be32 context}, driver*)
    uint32_t info = ARG32(0);
    g_callback = Load32(info);
    g_callbackContext = Load32(info + 4);
    if (ARG32(1))
        Store32(ARG32(1), DRIVER_HANDLE);
    LOG("[audio] render driver client: callback %08X context %08X", g_callback.load(), g_callbackContext.load());
    if (!g_running.exchange(true))
    {
        g_haveOutput = InitOutput();
        StartHostThread(AudioThread);
    }
    RETURN(0);
}

GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    g_callback = 0;
    RETURN(0);
}

GUEST_IMPORT(XAudioSubmitRenderDriverFrame)
{
    // (driver, samples*)
    g_framesSubmitted++;
    if (g_haveOutput && ARG32(1))
        Submit(ARG32(1));
    RETURN(0);
}

GUEST_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask)
{
    if (ARG32(1))
        Store32(ARG32(1), 0);
    RETURN(0);
}

GUEST_IMPORT(XAudioGetVoiceCategoryVolume)
{
    if (ARG32(1))
        StoreF32(ARG32(1), 1.0f);
    RETURN(0);
}

GUEST_IMPORT(XMACreateContext)
{
    uint32_t context = xma::AllocateContext();
    if (ARG32(0))
        Store32(ARG32(0), context);
    RETURN(context ? 0 : STATUS_NO_MEMORY);
}

GUEST_IMPORT(XMAReleaseContext)
{
    xma::ReleaseContext(ARG32(0));
    RETURN(0);
}
