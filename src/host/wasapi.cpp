#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "wasapi.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>

namespace stv::host {
namespace {
template <class T> void release(void*& p) {
    if (p) static_cast<T*>(p)->Release();
    p = nullptr;
}
} // namespace

AudioOut::~AudioOut() { close(); }

void AudioOut::close() {
    finish_priming(false);
    if (client_) static_cast<IAudioClient*>(client_)->Stop();
    release<IAudioRenderClient>(render_);
    release<IAudioClient>(client_);
    release<IMMDevice>(device_);
    release<IMMDeviceEnumerator>(enumerator_);
    pending_.clear(); pending_r_.clear();
    servo_.reset();
    newly_queued_ = 0;
    resync_pending_ = false;
    underrun_latched_ = false;
    buffer_frames_ = priming_target_frames_ = 0;
    last_pad_ = UINT32_MAX;
    started_ = false;
    if (com_owned_) { CoUninitialize(); com_owned_ = false; }
}

bool AudioOut::fail(const char* stage, int32_t code) {
    error_stage_ = stage;
    error_code_ = code;
    ++errors_;
    // Samples accepted into software but abandoned by a failed endpoint remain
    // explicit. No old PCM is replayed after the board has continued silently.
    dropped_source_samples_ += pending_.queued();
    close();
    recovery_pending_ = true;
    return false;
}

uint64_t AudioOut::unavailable_ms(uint64_t now_ms) const {
    const uint64_t current = std::max(now_ms, last_service_ms_);
    return unavailable_total_ms_ + (unavailable_observed_ ? current - unavailable_since_ms_ : 0);
}

uint64_t AudioOut::priming_wait_ms(uint64_t now_ms) const {
    const uint64_t current = std::max(now_ms, last_service_ms_);
    return priming_total_ms_ + (priming_ ? current - priming_since_ms_ : 0);
}

void AudioOut::finish_priming(bool completed) {
    if (!priming_) return;
    priming_total_ms_ += last_service_ms_ - priming_since_ms_;
    priming_ = false;
    if (completed) ++priming_completions_;
}

bool AudioOut::service(uint64_t now_ms) {
    // An out-of-order caller timestamp must not wrap elapsed time or turn an
    // unavailable endpoint into a busy retry loop.
    now_ms = last_service_ms_ = std::max(now_ms, last_service_ms_);
    if (!recovery_pending_) return ok();
    if (!unavailable_observed_) {
        unavailable_observed_ = true;
        unavailable_since_ms_ = now_ms;
    }
    if (recovery_attempts_ && now_ms - last_retry_ms_ < kRecoveryRetryMs) return false;
    last_retry_ms_ = now_ms;
    ++recovery_attempts_;
    if (!open_stream(true)) return false;
    recovery_pending_ = false;
    ++recoveries_;
    unavailable_total_ms_ += now_ms - unavailable_since_ms_;
    unavailable_observed_ = false;
    return true;
}

bool AudioOut::open() {
    close();
    error_stage_ = "";
    error_code_ = 0;
    underruns_ = 0;
    min_pad_ = UINT32_MAX;
    pending_target_ = pending_limit_ = maximum_pending_ = 0;
    output_resyncs_ = dropped_source_samples_ = dropped_device_frames_ = 0;
    pending_ = AudioResampler{}; pending_r_ = AudioResampler{};
    servo_.reset();
    rate_ = 48000; // matches the mixer's no-device startup fallback
    endpoint_rate_ = 0;
    recovery_pending_ = unavailable_observed_ = false;
    recovery_attempts_ = recoveries_ = 0;
    last_service_ms_ = last_retry_ms_ = unavailable_since_ms_ = unavailable_total_ms_ = 0;
    startup_primes_ = underrun_reprimes_ = priming_completions_ = 0;
    priming_since_ms_ = priming_total_ms_ = 0;
    return open_stream(false);
}

bool AudioOut::open_stream(bool preserve_rate) {
    // Recovery uses this path rather than cold open(), retaining all lifetime
    // errors, output counters and the sample rate used by the existing mixer.
    close();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return fail("CoInitializeEx", initialized);
    com_owned_ = SUCCEEDED(initialized);
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&en));
    if (FAILED(hr)) return fail("CoCreateInstance", hr);
    enumerator_ = en;
    IMMDevice* dev = nullptr;
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (FAILED(hr)) return fail("GetDefaultAudioEndpoint", hr);
    device_ = dev;
    IAudioClient* ac = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac));
    if (FAILED(hr)) return fail("Activate", hr);
    client_ = ac;
    WAVEFORMATEX* fmt = nullptr;
    hr = ac->GetMixFormat(&fmt);
    if (FAILED(hr)) return fail("GetMixFormat", hr);
    if (!fmt) return fail("GetMixFormat", E_POINTER);
    bool supported = fmt->nSamplesPerSec != 0 && fmt->nChannels != 0;
    is_float_ = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcm = fmt->wFormatTag == WAVE_FORMAT_PCM;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            supported = false;
        } else {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
            is_float_ = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            pcm = ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
            supported = supported && (!ext->Samples.wValidBitsPerSample || ext->Samples.wValidBitsPerSample == fmt->wBitsPerSample);
        }
    }
    supported = supported && ((is_float_ && fmt->wBitsPerSample == 32) || (pcm && fmt->wBitsPerSample == 16));
    const uint32_t block = uint32_t(fmt->nChannels) * (fmt->wBitsPerSample / 8);
    supported = supported && block == fmt->nBlockAlign && uint64_t(fmt->nSamplesPerSec) * block == fmt->nAvgBytesPerSec;
    if (!supported) { CoTaskMemFree(fmt); return fail("MixFormat", AUDCLNT_E_UNSUPPORTED_FORMAT); }
    endpoint_rate_ = fmt->nSamplesPerSec;
    const uint32_t stream_rate = preserve_rate ? rate_ : endpoint_rate_;
    DWORD flags = 0;
    if (stream_rate != endpoint_rate_) {
        // Preserve the existing board's source rate on a replacement endpoint.
        // WASAPI converts this stream to its native mix rate; our bounded drift
        // servo still sees source-rate frames and never retimes the game.
        // https://learn.microsoft.com/windows/win32/coreaudio/audclnt-streamflags-xxx-constants
        if (uint64_t(stream_rate) * block > UINT32_MAX) {
            CoTaskMemFree(fmt);
            return fail("StreamFormat", AUDCLNT_E_UNSUPPORTED_FORMAT);
        }
        fmt->nSamplesPerSec = stream_rate;
        fmt->nAvgBytesPerSec = stream_rate * block;
        flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }
    channels_ = fmt->nChannels;
    bits_ = fmt->wBitsPerSample;
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, fmt, nullptr);
    CoTaskMemFree(fmt);
    if (FAILED(hr)) return fail("Initialize", hr);
    UINT32 frames = 0;
    hr = ac->GetBufferSize(&frames);
    if (FAILED(hr) || !frames) return fail("GetBufferSize", FAILED(hr) ? hr : E_UNEXPECTED);
    buffer_frames_ = frames;
    IAudioRenderClient* rc = nullptr;
    hr = ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc));
    if (FAILED(hr)) return fail("GetService", hr);
    render_ = rc;
    // Prime silence before starting, as required by the rendering contract.
    BYTE* data = nullptr;
    hr = rc->GetBuffer(frames, &data);
    if (FAILED(hr)) return fail("GetBuffer", hr);
    hr = rc->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) return fail("ReleaseBuffer", hr);
    hr = ac->Start();
    if (FAILED(hr)) return fail("Start", hr);
    // Commit the cold-open source rate only after the complete stream opens.
    // A late startup failure must leave the host's 48000 Hz fallback intact.
    rate_ = stream_rate;
    // Keep the existing six-frame software bound and three-frame backlog target.
    pending_target_ = std::max<uint32_t>(2u, uint32_t(
        (uint64_t(rate_) * FrameClock::kRateDenominator * 3u + FrameClock::kRateNumerator - 1) /
        FrameClock::kRateNumerator));
    pending_limit_ = pending_target_ * 2u;
    // Fill the existing device target after starvation, rather than restoring
    // playback one fragile arcade block at a time. The software bound is not
    // enlarged; two source samples cover interpolation lookahead/phase.
    priming_target_frames_ = std::min(buffer_frames_, pending_limit_ - 2u);
    return true;
}

bool AudioOut::discard_pending() {
    finish_priming(false);
    pending_.clear(); pending_r_.clear();
    servo_.reset();
    newly_queued_ = 0;
    resync_pending_ = false;
    underrun_latched_ = false;
    started_ = false; // an intentional reset is not an underrun
    last_pad_ = UINT32_MAX;
    if (!client_) return true;
    auto* ac = static_cast<IAudioClient*>(client_);
    auto* rc = static_cast<IAudioRenderClient*>(render_);
    HRESULT hr = ac->Stop();
    if (FAILED(hr)) return fail("DiscardStop", hr);
    hr = ac->Reset();
    if (FAILED(hr)) return fail("DiscardReset", hr);
    // Prime the reset endpoint with silence, as open() does. This only discards
    // host output: board/mixer state and the selected level are untouched.
    BYTE* data = nullptr;
    hr = rc->GetBuffer(buffer_frames_, &data);
    if (FAILED(hr)) return fail("DiscardGetBuffer", hr);
    hr = rc->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) return fail("DiscardReleaseBuffer", hr);
    hr = ac->Start();
    if (FAILED(hr)) return fail("DiscardStart", hr);
    return true;
}

uint32_t AudioOut::queued() {
    if (!client_) return 0;
    UINT32 pad = 0;
    const HRESULT hr = static_cast<IAudioClient*>(client_)->GetCurrentPadding(&pad);
    if (FAILED(hr)) { fail("GetCurrentPadding", hr); return 0; }
    if (pad > buffer_frames_) { fail("GetCurrentPadding", E_UNEXPECTED); return 0; }
    last_pad_ = pad;
    return pad + uint32_t(pending_.queued());
}

void AudioOut::push(const float* samples, std::size_t count) { push_frames(samples, nullptr, count); }

void AudioOut::push_stereo(const float* lr, std::size_t frames) {
    stereo_left_.resize(frames);
    stereo_right_.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) { stereo_left_[i] = lr[2 * i]; stereo_right_[i] = lr[2 * i + 1]; }
    push_frames(stereo_left_.data(), stereo_right_.data(), frames);
}

// The right channel always mirrors the left queue's length, ratio and phase;
// mono sources feed the same samples to both.
void AudioOut::push_frames(const float* left, const float* right, std::size_t count) {
    if (!client_ || !count) return;
    if (!right) right = left;
    if (count > pending_limit_ - std::min<std::size_t>(pending_.queued(), pending_limit_)) {
        // Trim before appending, so even a very large caller block cannot
        // temporarily allocate an unbounded output deque. Source data stays const.
        if (count >= pending_target_) {
            dropped_source_samples_ += pending_.retain_latest(0) + count - pending_target_;
            pending_r_.retain_latest(0);
            pending_.append(left + count - pending_target_, pending_target_);
            pending_r_.append(right + count - pending_target_, pending_target_);
        } else {
            dropped_source_samples_ += pending_.retain_latest(pending_target_ - count);
            pending_r_.retain_latest(pending_target_ - count);
            pending_.append(left, count);
            pending_r_.append(right, count);
        }
        servo_.reset();
        newly_queued_ = 0;
        resync_pending_ = true;
    } else {
        pending_.append(left, count);
        pending_r_.append(right, count);
        newly_queued_ += count;
    }
    maximum_pending_ = std::max(maximum_pending_, uint32_t(pending_.queued()));
}

void AudioOut::pump() {
    if (!client_) { pending_.clear(); pending_r_.clear(); return; }
    auto* ac = static_cast<IAudioClient*>(client_);
    auto* rc = static_cast<IAudioRenderClient*>(render_);
    UINT32 pad = 0;
    HRESULT hr = ac->GetCurrentPadding(&pad);
    if (FAILED(hr)) { fail("GetCurrentPadding", hr); return; }
    if (pad > buffer_frames_) { fail("GetCurrentPadding", E_UNEXPECTED); return; }
    last_pad_ = pad;
    if (started_) {
        if (pad == 0 && !underrun_latched_) ++underruns_;
        underrun_latched_ = pad == 0;
        min_pad_ = std::min(min_pad_, pad);
    }
    const bool restarting = resync_pending_;
    if (restarting) {
        finish_priming(false); // explicit backlog recovery has its own counters
        hr = ac->Stop();
        if (FAILED(hr)) { fail("BacklogStop", hr); return; }
        hr = ac->Reset();
        if (FAILED(hr)) { fail("BacklogReset", hr); return; }
        dropped_device_frames_ += pad;
        last_pad_ = UINT32_MAX;
        pad = 0;
        servo_.reset();
        set_ratio_both(1.0);
        newly_queued_ = 0;
        started_ = false;
        underrun_latched_ = false;
    } else {
        if (!pad && !priming_) {
            priming_ = true;
            priming_since_ms_ = last_service_ms_;
            if (started_) ++underrun_reprimes_;
            else ++startup_primes_;
            servo_.reset();
            set_ratio_both(1.0); // keep phase and every source sample
        }
        if (priming_) {
            // The real gap was already counted above. While its confirmed PCM
            // is unavailable, do not replay it, manufacture it or trickle out
            // single blocks that immediately empty the endpoint again.
            if (pending_.available(priming_target_frames_) < priming_target_frames_) return;
            newly_queued_ = 0;
        } else if (newly_queued_) {
            set_ratio_both(servo_.update(std::uint64_t(pad) + pending_.queued(),
                                            buffer_frames_, rate_, newly_queued_));
            newly_queued_ = 0;
        }
    }
    const uint32_t n = uint32_t(pending_.available(buffer_frames_ - pad));
    if (!n) {
        if (restarting) fail("BacklogPrime", E_UNEXPECTED);
        return;
    }
    BYTE* data = nullptr;
    hr = rc->GetBuffer(n, &data);
    if (FAILED(hr) || !data) { fail("GetBuffer", FAILED(hr) ? hr : E_POINTER); return; }
    const std::size_t source_before = pending_.queued();
    for (uint32_t i = 0; i < n; ++i) {
        float v = 0.0f, w = 0.0f;
        pending_.take(v); // n was bounded by source availability, including interpolation history
        pending_r_.take(w);
        v = level_.apply(v);   // Device copy only; the pending/raw PCM stays unscaled.
        w = level_.apply(w);
        for (uint32_t c = 0; c < channels_; ++c) {
            // Stereo endpoints: left/right. Surround layouts: front pair, rest mono.
            const float out = channels_ >= 2 && c < 2 ? (c ? w : v) : 0.5f * (v + w);
            if (is_float_) reinterpret_cast<float*>(data)[i * channels_ + c] = out;
            else reinterpret_cast<int16_t*>(data)[i * channels_ + c] = audio_pcm16(out);
        }
    }
    hr = rc->ReleaseBuffer(n, 0);
    if (FAILED(hr)) {
        // Conversion consumed these samples, but the failed release did not
        // submit them. fail() accounts for the still-pending remainder.
        dropped_source_samples_ += source_before - pending_.queued();
        fail("ReleaseBuffer", hr);
        return;
    }
    if (restarting) {
        // Start with retained PCM, not a new buffer of silence in front of it.
        // Errors and all discarded frames remain visible in the host report.
        hr = ac->Start();
        if (FAILED(hr)) { dropped_device_frames_ += n; fail("BacklogStart", hr); return; }
        resync_pending_ = false;
        ++output_resyncs_;
    }
    if (priming_) {
        finish_priming(true);
        // Successful full priming ends this gap even if the owner next runs
        // only after the newly submitted buffer has genuinely drained again.
        underrun_latched_ = false;
    }
    started_ = true;
}
} // namespace stv::host
