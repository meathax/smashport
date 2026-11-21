// Exercise the actual adapter with controlled COM endpoints; no device is changed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
enum class Fault { None, Com, Factory, Endpoint, Activate, Mix, Initialize, Size, ZeroSize,
                   Service, Start, Padding, OversizedPadding, Buffer, NullBuffer, Release, Stop, Reset };
enum class Format { Float, Pcm, ExtFloat, ExtPcm, ChannelsZero, RateZero, Align,
                    Average, Float64, UnsupportedTag, ExtShort, ExtUnknown, ExtBits };
Fault fault = Fault::None;
Format format = Format::Float;
HRESULT com_result = S_OK;
int refs = 0, initialized = 0, uninitialized = 0, checks = 0, failures = 0;
int buffer_calls = 0, silence_calls = 0, start_calls = 0, stop_calls = 0, reset_calls = 0;
UINT32 written_frames = 0, reported_padding = 0, endpoint_mix_rate = 48000;
int endpoint_queries = 0, initialize_calls = 0;
DWORD negotiated_flags = 0;
AUDCLNT_SHAREMODE negotiated_mode = AUDCLNT_SHAREMODE_EXCLUSIVE;
WAVEFORMATEX negotiated_format{};
std::vector<BYTE> sample_data(200000);
void check(bool condition, const char* name) {
    ++checks; if (!condition) { ++failures; std::printf("FAIL %s\n", name); }
}
#define MOCK_UNKNOWN \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; } \
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs); } \
    ULONG STDMETHODCALLTYPE Release() override { return ULONG(--refs); }

struct Render : IAudioRenderClient {
    MOCK_UNKNOWN
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 n, BYTE** data) override {
        ++buffer_calls;
        if (fault == Fault::Buffer) return AUDCLNT_E_DEVICE_INVALIDATED;
        *data = fault == Fault::NullBuffer ? nullptr : sample_data.data();
        written_frames = n; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32, DWORD flags) override {
        if (fault == Fault::Release) return AUDCLNT_E_DEVICE_INVALIDATED;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) ++silence_calls;
        return S_OK;
    }
} render;
struct Client : IAudioClient {
    MOCK_UNKNOWN
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME, REFERENCE_TIME,
                                         const WAVEFORMATEX* f, LPCGUID) override {
        ++initialize_calls;
        negotiated_mode = mode;
        negotiated_flags = flags;
        negotiated_format = *f;
        return fault == Fault::Initialize ? E_FAIL : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* n) override {
        if (fault == Fault::Size) return AUDCLNT_E_DEVICE_INVALIDATED;
        *n = fault == Fault::ZeroSize ? 0 : negotiated_format.nSamplesPerSec / 10; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* n) override {
        if (fault == Fault::Padding) return AUDCLNT_E_DEVICE_INVALIDATED;
        *n = fault == Fault::OversizedPadding ? negotiated_format.nSamplesPerSec / 10 + 1 : reported_padding; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** out) override {
        if (fault == Fault::Mix) return E_FAIL;
        auto* ext = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
        std::memset(ext, 0, sizeof *ext); auto& f = ext->Format;
        f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; f.nChannels = 2; f.nSamplesPerSec = endpoint_mix_rate; f.wBitsPerSample = 32;
        if (format == Format::Pcm || format == Format::ExtPcm) { f.wFormatTag = WAVE_FORMAT_PCM; f.wBitsPerSample = 16; }
        if (format == Format::ChannelsZero) f.nChannels = 0;
        if (format == Format::RateZero) f.nSamplesPerSec = 0;
        if (format == Format::Float64) f.wBitsPerSample = 64;
        if (format == Format::UnsupportedTag) { f.wFormatTag = WAVE_FORMAT_ADPCM; f.wBitsPerSample = 16; }
        if (format == Format::ExtFloat || format == Format::ExtPcm || format == Format::ExtShort ||
            format == Format::ExtUnknown || format == Format::ExtBits) {
            f.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            f.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            ext->Samples.wValidBitsPerSample = f.wBitsPerSample;
            ext->SubFormat = format == Format::ExtPcm ? KSDATAFORMAT_SUBTYPE_PCM : KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            if (format == Format::ExtShort) f.cbSize = 0;
            if (format == Format::ExtUnknown) ext->SubFormat = GUID_NULL;
            if (format == Format::ExtBits) ext->Samples.wValidBitsPerSample = 24;
        }
        f.nBlockAlign = WORD(f.nChannels * f.wBitsPerSample / 8);
        f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
        if (format == Format::Align) ++f.nBlockAlign;
        if (format == Format::Average) ++f.nAvgBytesPerSec;
        *out = &f; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Start() override {
        ++start_calls; return fault == Fault::Start ? AUDCLNT_E_DEVICE_INVALIDATED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        ++stop_calls; return fault == Fault::Stop ? AUDCLNT_E_DEVICE_INVALIDATED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        ++reset_calls; return fault == Fault::Reset ? AUDCLNT_E_DEVICE_INVALIDATED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetService(REFIID, void** out) override {
        if (fault == Fault::Service) return E_FAIL;
        render.AddRef(); *out = &render; return S_OK;
    }
} client;
struct Device : IMMDevice {
    MOCK_UNKNOWN
    HRESULT STDMETHODCALLTYPE Activate(REFIID, DWORD, PROPVARIANT*, void** out) override {
        if (fault == Fault::Activate) return E_FAIL;
        client.AddRef(); *out = &client; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD*) override { return E_NOTIMPL; }
} device;
struct Enumerator : IMMDeviceEnumerator {
    MOCK_UNKNOWN
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** out) override {
        ++endpoint_queries;
        check(flow == eRender && role == eConsole, "open/recovery selects the current default console render endpoint");
        if (fault == Fault::Endpoint) return E_FAIL;
        device.AddRef(); *out = &device; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR, IMMDevice**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient*) override { return E_NOTIMPL; }
} enumerator;
#undef MOCK_UNKNOWN
HRESULT fake_initialize(LPVOID, DWORD) {
    if (fault == Fault::Com) return E_FAIL;
    if (SUCCEEDED(com_result)) ++initialized;
    return com_result;
}
void fake_uninitialize() { ++uninitialized; }
HRESULT fake_create(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID* out) {
    if (fault == Fault::Factory) return E_FAIL;
    enumerator.AddRef(); *out = &enumerator; return S_OK;
}
} // namespace

// The seam substitutes only process/device creation. COM calls below use the
// actual adapter body and normal vtable dispatch to the controlled endpoints.
#define CoInitializeEx fake_initialize
#define CoUninitialize fake_uninitialize
#define CoCreateInstance fake_create
#include "../src/host/wasapi.cpp"
#undef CoInitializeEx
#undef CoUninitialize
#undef CoCreateInstance

namespace {
void reset() {
    check(refs == 0, "prior case released every acquired COM interface");
    refs = initialized = uninitialized = buffer_calls = silence_calls = start_calls = stop_calls = reset_calls = 0;
    fault = Fault::None; format = Format::Float; com_result = S_OK;
    reported_padding = written_frames = 0;
    endpoint_mix_rate = 48000;
    endpoint_queries = initialize_calls = 0;
    negotiated_flags = 0;
    negotiated_mode = AUDCLNT_SHAREMODE_EXCLUSIVE;
    negotiated_format = {};
}
void closed(stv::host::AudioOut& out, const char* stage) {
    check(!out.ok() && out.queued() == 0, "failed stream is closed and has no pending/device queue");
#ifndef STV_PREP_BASELINE
    check(std::strcmp(out.error_stage(), stage) == 0 && FAILED(out.error_code()) && out.errors() == 1,
          "failure retains its named boundary and HRESULT");
#endif
    const float more[] = {0.1f, -0.1f}; out.push(more, 2); out.pump();
    check(out.queued() == 0, "closed stream drops later samples instead of growing a queue");
}
void backlog_recovery() {
    reset();
    {
        stv::host::AudioOut out;
        check(out.open(), "four-step catch-up fixture opened");
        const std::vector<float> block(880, .25f);
        for (int i = 0; i < 4; ++i) out.push(block.data(), block.size());
        check(out.pending_samples() == 3520 && out.pending_limit_samples() >= 3520 &&
              out.pending_limit_samples() <= 5280, "normal four-frame burst fits a bounded six-frame queue");
        out.pump();
        check(out.ok() && !out.errors() && !out.output_resyncs() && !out.dropped_source_samples() &&
              !out.dropped_device_frames(), "normal four-step catch-up keeps every output sample");
    }
    check(refs == 0 && initialized == uninitialized, "four-step catch-up balances COM ownership");

    // A device that repeatedly reports its buffer full must not cause an
    // unbounded software queue, even though all its calls return S_OK.
    reset();
    {
        stv::host::AudioOut out;
        check(out.open(), "full-padding fixture opened");
        const std::vector<float> block(880, -.25f);
        reported_padding = 4800;
        for (int i = 0; i < 200; ++i) {
            out.push(block.data(), block.size());
            out.pump();
            check(out.pending_samples() <= out.pending_limit_samples(),
                  "full-padding endpoint cannot grow the source queue beyond its bound");
        }
        check(out.ok() && !out.errors() && out.output_resyncs() > 0 &&
              out.dropped_source_samples() > 0 && out.dropped_device_frames() > 0,
              "full-padding output discontinuities are reported instead of hidden");
        check(uint64_t(reset_calls) == out.output_resyncs() &&
              uint64_t(start_calls) == out.output_resyncs() + 1,
              "each output resync resets and restarts the device exactly once");
        check(out.maximum_pending_samples() <= out.pending_limit_samples() && out.underruns() == 0,
              "bounded output resync is distinguished from an actual device underrun");
    }
    check(refs == 0 && initialized == uninitialized, "full-padding recovery balances COM ownership");

    reset();
    {
        stv::host::AudioOut out;
        check(out.open(), "owner-stall fixture opened");
        reported_padding = 4797;
        const float warmup[] = {.25f, -.5f, .75f};
        out.push(warmup, 3); out.pump();
        std::vector<float> source(48000);
        for (std::size_t i = 0; i < source.size(); ++i) source[i] = float(i) / 65536.0f;
        const auto original = source;
        reported_padding = 0; // the old device queue drained during the owner stall
        out.push(source.data(), source.size());
        const std::size_t retained = out.pending_samples();
        check(retained >= 2632 && retained <= 2635 && retained <= out.pending_limit_samples(),
              "one-second backlog retains only the newest three arcade frames");
        check(out.dropped_source_samples() == 1 + source.size() - retained,
              "old lookahead and dropped source samples are counted exactly");
        out.pump();
        const auto* data = reinterpret_cast<const float*>(sample_data.data());
        check(written_frames == retained - 1 && out.pending_samples() == 1 &&
              data[0] == source[source.size() - retained] &&
              data[(written_frames - 1) * 2] == source[source.size() - 2],
              "restart begins with newest retained PCM and keeps its last lookahead");
        check(out.output_resyncs() == 1 && out.dropped_device_frames() == 0 &&
              reset_calls == 1 && start_calls == 2 && silence_calls == 1,
              "stall recovery primes actual PCM without another latency-sized silence buffer");
        check(out.underruns() == 1 && out.min_padding() == 0 && out.errors() == 0,
              "real stall underrun remains observable after successful recovery");
        check(source == original, "output backlog recovery leaves raw source PCM immutable");
    }
    check(refs == 0 && initialized == uninitialized, "owner-stall recovery balances COM ownership");

    struct RecoveryFault { Fault fault; const char* stage; };
    for (const auto& row : {RecoveryFault{Fault::Stop, "BacklogStop"}, {Fault::Reset, "BacklogReset"},
                           {Fault::Buffer, "GetBuffer"}, {Fault::Release, "ReleaseBuffer"},
                           {Fault::Start, "BacklogStart"}}) {
        reset();
        {
            stv::host::AudioOut out;
            check(out.open(), "recovery-failure fixture opened");
            const std::vector<float> source(48000, .25f);
            out.push(source.data(), source.size());
            fault = row.fault;
            out.pump();
            closed(out, row.stage);
            check(out.output_resyncs() == 0 && out.dropped_source_samples() > 0,
                  "failed recovery retains its discontinuity without counting a successful restart");
        }
        check(refs == 0 && initialized == uninitialized, "recovery failure releases every COM interface");
    }
}

using stv::host::AudioOut;
constexpr DWORD conversion_flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

void missing_endpoint_recovery() {
    reset();
    {
        AudioOut out;
        out.set_volume(37); out.set_muted(true);
        fault = Fault::Endpoint;
        check(!out.open() && out.rate() == 48000 && out.endpoint_rate() == 0,
              "missing startup endpoint preserves the 48000 Hz source fallback");
        check(!out.service(100) && out.recovery_attempts() == 1 && !out.recoveries() && out.errors() == 2,
              "first unavailable observation makes one recorded retry");
        check(out.unavailable_ms(350) == 250 && out.buffer_frames() == 0 &&
              out.last_device_padding() == UINT32_MAX, "missing endpoint exposes measured downtime and unknown padding");
        const int queries = endpoint_queries;
        for (uint64_t now : {101ULL, 999ULL, 1099ULL, 50ULL})
            check(!out.service(now), "retry interval also holds for an out-of-order timestamp");
        check(out.recovery_attempts() == 1 && out.errors() == 2 && endpoint_queries == queries &&
              refs == 0 && initialized == uninitialized, "failed endpoint is not polled or leaked at render frequency");
        fault = Fault::None; endpoint_mix_rate = 44100; format = Format::ExtPcm;
        check(out.service(1100) && out.ok() && out.rate() == 48000 && out.endpoint_rate() == 44100,
              "late endpoint availability recovers without changing the existing source rate");
        check(negotiated_mode == AUDCLNT_SHAREMODE_SHARED && negotiated_flags == conversion_flags &&
              negotiated_format.nSamplesPerSec == 48000 && negotiated_format.nAvgBytesPerSec == 48000 * 4 &&
              negotiated_format.nBlockAlign == 4 && negotiated_format.wFormatTag == WAVE_FORMAT_EXTENSIBLE,
              "replacement rate conversion uses valid unchanged-source shared PCM format");
        check(out.recovery_attempts() == 2 && out.recoveries() == 1 && out.errors() == 2 &&
              std::strcmp(out.error_stage(), "GetDefaultAudioEndpoint") == 0 && out.unavailable_ms(9000) == 1000,
              "successful recovery retains failed attempts, last error and completed downtime");
        check(out.volume() == 37 && out.muted() && out.buffer_frames() == 4800 &&
              out.last_device_padding() == UINT32_MAX, "recovery preserves level and primes a source-rate stream");
        const int opens = initialize_calls;
        check(out.service(20000) && out.service(20001) && initialize_calls == opens && out.recovery_attempts() == 2,
              "healthy service never reopens the endpoint or retimes output");
    }
    check(refs == 0 && initialized == uninitialized, "missing-startup recovery balances COM ownership");

    // A format learned before a late startup failure must not replace the rate
    // used by the fallback mixer constructed by the host after open() fails.
    for (Fault late : {Fault::Initialize, Fault::Size, Fault::Service, Fault::Start}) {
        reset(); endpoint_mix_rate = 44100; fault = late;
        {
            AudioOut out;
            check(!out.open() && out.rate() == 48000 && out.endpoint_rate() == 44100,
                  "late startup failure does not commit a rate the host cannot use");
            fault = Fault::None;
            check(out.service(0) && out.rate() == 48000 && negotiated_flags == conversion_flags &&
                  out.errors() == 1 && out.recovery_attempts() == 1 && out.recoveries() == 1,
                  "late startup recovery converts to the endpoint without resetting source or errors");
        }
        check(refs == 0 && initialized == uninitialized, "late startup recovery releases partial and final clients");
    }
}

void endpoint_rate_recovery() {
    struct Rates { uint32_t source, replacement; Format format; };
    for (const Rates row : {Rates{48000, 48000, Format::Float}, {48000, 44100, Format::ExtFloat},
                            {44100, 48000, Format::Pcm}, {44100, 44100, Format::ExtPcm}}) {
        reset(); endpoint_mix_rate = row.source;
        {
            AudioOut out;
            check(out.open() && out.rate() == row.source && !negotiated_flags,
                  "healthy cold open uses the first native mix rate without conversion");
            const uint32_t limit = out.pending_limit_samples();
            reported_padding = out.buffer_frames() - 3;
            const float first[] = {.25f, -.5f, .75f};
            out.push(first, 3); out.pump();
            check(out.output_ratio() == 1.0 && out.output_samples() == 2 &&
                  out.last_device_padding() == reported_padding, "first stream renders immutable source at unity");
            reported_padding = 0; out.pump();
            check(out.underruns() == 1 && out.min_padding() == 0 && out.last_device_padding() == 0,
                  "real pre-loss starvation establishes lifetime underrun telemetry");
            const std::vector<float> source(48000, .25f);
            out.push(source.data(), source.size());
            reported_padding = 200; out.pump();
            check(out.output_resyncs() == 1 && out.dropped_source_samples() > 0 &&
                  out.dropped_device_frames() == 200 && out.last_device_padding() == UINT32_MAX,
                  "pre-loss backlog recovery establishes retained drop telemetry");
            const uint64_t outputs = out.output_samples(), drops = out.dropped_source_samples();
            const uint32_t maximum = out.maximum_pending_samples();
            const float abandoned[] = {.1f, .2f, .3f};
            out.push(abandoned, 3);
            const uint32_t pending = out.pending_samples();
            fault = Fault::Padding; (void)out.queued();
            check(!out.ok() && out.dropped_source_samples() == drops + pending &&
                  out.last_device_padding() == UINT32_MAX && out.buffer_frames() == 0,
                  "endpoint loss counts abandoned pending PCM and invalidates stale padding");
            endpoint_mix_rate = row.replacement; format = row.format; fault = Fault::Initialize;
            check(!out.service(2000) && out.rate() == row.source && out.errors() == 2 && refs == 0 &&
                  initialized == uninitialized, "failed reopen preserves source rate and releases partial endpoint");
            fault = Fault::None;
            check(!out.service(2999) && out.service(3000), "recovery respects the exact retry deadline");
            check(out.rate() == row.source && out.endpoint_rate() == row.replacement &&
                  negotiated_format.nSamplesPerSec == row.source &&
                  negotiated_flags == (row.source == row.replacement ? 0u : conversion_flags),
                  "same and different endpoint rates use the original stream rate with conversion only when needed");
            check(out.pending_limit_samples() == limit && out.maximum_pending_samples() == maximum &&
                  out.output_samples() == outputs && out.output_resyncs() == 1 &&
                  out.dropped_source_samples() == drops + pending && out.dropped_device_frames() == 200 &&
                  out.underruns() == 1 && out.min_padding() == 0 && out.errors() == 2,
                  "successful reopen does not reset lifetime output, queue, error or underrun evidence");
            check(out.recovery_attempts() == 2 && out.recoveries() == 1 && out.unavailable_ms(4000) == 1000 &&
                  std::strcmp(out.error_stage(), "Initialize") == 0 && out.pending_samples() == 0,
                  "recovery records failures and downtime without replaying stale PCM");
            reported_padding = out.buffer_frames() - 3;
            const float resumed[] = {-.75f, .5f, .125f};
            out.push(resumed, 3); out.pump();
            const bool pcm = row.format == Format::Pcm || row.format == Format::ExtPcm;
            const bool begins_new = pcm ? reinterpret_cast<const int16_t*>(sample_data.data())[0] == -24575
                                       : reinterpret_cast<const float*>(sample_data.data())[0] == -.75f;
            check(out.output_samples() == outputs + 2 && out.output_ratio() == 1.0 && begins_new &&
                  first[0] == .25f && abandoned[0] == .1f && resumed[0] == -.75f,
                  "new endpoint resumes new immutable PCM with the existing bounded drift converter");
        }
        check(refs == 0 && initialized == uninitialized, "rate-changing endpoint recovery balances COM ownership");
    }
}

void endpoint_recovery_failures() {
    reset();
    {
        AudioOut out; check(out.open(), "repeated-loss fixture opened");
        fault = Fault::Padding; (void)out.queued(); fault = Fault::None;
        check(out.service(1000), "first runtime loss recovers");
        fault = Fault::Padding; (void)out.queued(); fault = Fault::None;
        check(!out.service(1001) && !out.service(1999) && out.recovery_attempts() == 1,
              "rapid repeated loss cannot bypass retry pacing after an apparently successful reopen");
        check(out.service(2000) && out.recovery_attempts() == 2 && out.recoveries() == 2 &&
              out.errors() == 2 && out.unavailable_ms(3000) == 999,
              "separate outages accumulate recovery counts and only observed unavailable time");
    }
    check(refs == 0 && initialized == uninitialized, "repeated recovery balances COM ownership");

    struct ResetFault { Fault fault; const char* stage; };
    for (const auto row : {ResetFault{Fault::Stop, "DiscardStop"}, {Fault::Reset, "DiscardReset"},
                           {Fault::Buffer, "DiscardGetBuffer"}, {Fault::Release, "DiscardReleaseBuffer"},
                           {Fault::Start, "DiscardStart"}}) {
        reset(); endpoint_mix_rate = 44100;
        {
            AudioOut out; check(out.open(), "explicit reset failure fixture opened");
            fault = row.fault;
            check(!out.discard_pending() && !out.ok() && out.errors() == 1 &&
                  std::strcmp(out.error_stage(), row.stage) == 0 && out.last_device_padding() == UINT32_MAX,
                  "explicit reset failure closes endpoint and retains its exact boundary");
            fault = Fault::None;
            check(out.service(10) && out.rate() == 44100 && out.errors() == 1 && out.recoveries() == 1 &&
                  out.underruns() == 0 && out.output_resyncs() == 0,
                  "failed explicit reset recovers output without rewriting its error or inventing an underrun");
        }
        check(refs == 0 && initialized == uninitialized, "reset-failure recovery balances COM ownership");
    }

    reset();
    {
        AudioOut out; check(out.open(), "release failure accounting fixture opened");
        reported_padding = 4795;
        const float source[] = {.1f, .2f, .3f, .4f, .5f};
        out.push(source, 5); fault = Fault::Release; out.pump();
        check(!out.ok() && out.dropped_source_samples() == 5 && out.output_samples() == 4 && out.errors() == 1,
              "failed ReleaseBuffer counts both converted and still-pending abandoned source samples");
        fault = Fault::None;
        check(out.service(100) && out.dropped_source_samples() == 5 && out.output_samples() == 4 &&
              out.errors() == 1 && source[0] == .1f && source[4] == .5f,
              "reopen retains failed-submission accounting and never mutates original samples");
    }
    check(refs == 0 && initialized == uninitialized, "failed submission recovery balances COM ownership");

    for (HRESULT mode : {S_FALSE, RPC_E_CHANGED_MODE}) {
        reset(); com_result = mode; fault = Fault::Endpoint;
        {
            AudioOut out; check(!out.open() && !out.service(0), "missing endpoint can retry in an existing COM apartment");
            fault = Fault::None;
            check(out.service(1000) && out.recoveries() == 1 && out.errors() == 2,
                  "recovery respects existing apartment ownership and cumulative errors");
        }
        check(refs == 0 && initialized == uninitialized, "retry balances only COM references it actually owns");
    }
}

void empty_buffer_priming() {
    // Both initially drained silence and a real later underrun must collect real
    // PCM before resuming. A healthy buffer's three-sample conversion fixtures
    // above still submit immediately; this policy never raises steady latency.
    for (bool running : {false, true}) {
        reset();
        {
            AudioOut out; check(out.open() && out.service(0), "empty-buffer priming fixture opened");
            std::vector<float> source, rendered;
            auto pump_and_capture = [&] {
                const uint64_t before = out.output_samples();
                out.pump();
                const uint64_t count = out.output_samples() - before;
                if (!count) return;
                check(count == written_frames, "priming fixture captures exactly the submitted device frames");
                const auto* data = reinterpret_cast<const float*>(sample_data.data());
                for (uint64_t i = 0; i < count; ++i) rendered.push_back(data[i * 2]);
            };
            if (running) {
                source = {.25f, -.5f, .75f};
                reported_padding = out.buffer_frames() - 3;
                out.push(source.data(), source.size()); pump_and_capture();
                check(rendered.size() == 2 && !out.priming() && out.startup_primes() == 0,
                      "positive-padding playback starts without a priming delay");
            }
            const uint64_t output_before = out.output_samples();
            reported_padding = 0;
            out.service(100); pump_and_capture();
            check(out.priming() && out.priming_target_frames() == 4800 &&
                  out.startup_primes() == (running ? 0u : 1u) &&
                  out.underrun_reprimes() == (running ? 1u : 0u) &&
                  out.underruns() == (running ? 1u : 0u),
                  "drained startup and actual starvation have separate honest priming counters");
            // Six actual-size arcade blocks fit the unchanged six-frame source
            // bound. The first five must not be trickled out and starved again.
            const std::size_t sizes[] = {877, 878, 877, 878, 877, 877};
            for (std::size_t block = 0; block < 6; ++block) {
                std::vector<float> pcm(sizes[block]);
                for (std::size_t i = 0; i < pcm.size(); ++i)
                    pcm[i] = float(int((source.size() + i) % 1024) - 512) / 1024.0f;
                const auto original = pcm;
                source.insert(source.end(), pcm.begin(), pcm.end());
                out.service(120 + block * 20);
                out.push(pcm.data(), pcm.size()); pump_and_capture();
                check(pcm == original && out.pending_samples() <= out.pending_limit_samples() &&
                      !out.dropped_source_samples() && !out.dropped_device_frames(),
                      "single-block confirmed input stays immutable and bounded during priming");
                if (block < 5) {
                    pump_and_capture();
                    check(out.priming() && out.output_samples() == output_before &&
                          out.underruns() == (running ? 1u : 0u) &&
                          out.underrun_reprimes() == (running ? 1u : 0u),
                          "one-block restart churn cannot recur while the real reserve is incomplete");
                }
            }
            check(!out.priming() && out.priming_completions() == 1 &&
                  out.output_samples() == output_before + 4800 && out.priming_wait_ms(300) == 120,
                  "ready reserve fills the existing 100ms buffer and records its actual owner-clock wait");
            check(out.underruns() == (running ? 1u : 0u) && out.output_resyncs() == 0 &&
                  out.errors() == 0 && reset_calls == 0 && start_calls == 1 && silence_calls == 1,
                  "priming preserves the real gap without device resets or inserted silence");
            // Drain the remaining source behind positive device padding. The
            // existing final lookahead is retained, never dropped or duplicated.
            out.service(230); reported_padding = 1; pump_and_capture();
            check(rendered.size() + 1 == source.size() && out.pending_samples() == 1 &&
                  std::memcmp(rendered.data(), source.data(), rendered.size() * sizeof(float)) == 0 &&
                  out.output_ratio() == 1.0 && !out.dropped_source_samples(),
                  "all original PCM emerges in exact order across the gap with no loss or replay");
            // A genuinely drained newly primed buffer remains a new underrun.
            out.service(400); reported_padding = 0; pump_and_capture();
            check(out.underruns() == (running ? 2u : 1u) && out.priming() &&
                  out.underrun_reprimes() == (running ? 2u : 1u),
                  "successful priming does not suppress a later genuine underrun");
            out.service(450);
            check(out.discard_pending() && !out.priming() && out.priming_completions() == 1 &&
                  out.priming_wait_ms(900) == 170,
                  "explicit restart cancels a pending prime while retaining its wait and gap evidence");
        }
        check(refs == 0 && initialized == uninitialized, "empty-buffer priming balances COM ownership");
    }
}
} // namespace

int main() {
    for (Format f : {Format::Float, Format::Pcm, Format::ExtFloat, Format::ExtPcm}) {
        reset(); format = f;
        {
            stv::host::AudioOut out; check(out.open() && out.ok() && out.rate() == 48000, "supported device format opens");
            check(silence_calls == 1 && start_calls == 1, "silence is primed before playback starts");
            // Balance source + device queue at the target so this conversion
            // fixture has unity clock ratio. One source sample is retained as
            // interpolation lookahead and must lead the next submitted block.
            reported_padding = 4797;
            const float pcm[] = {0.25f, -0.5f, 0.75f}; out.push(pcm, 3); out.pump();
            check(out.ok() && out.output_ratio() == 1.0 && written_frames == 2 &&
                  out.output_samples() == 2 && out.queued() == reported_padding + 1,
                  "supported format renders two mono samples and retains one lookahead");
            if (f == Format::Float || f == Format::ExtFloat) {
                auto* data = reinterpret_cast<float*>(sample_data.data());
                check(data[0] == .25f && data[1] == .25f && data[2] == -.5f && data[3] == -.5f, "float stereo channel expansion exact");
            } else {
                auto* data = reinterpret_cast<int16_t*>(sample_data.data());
                check(data[0] == 8192 && data[1] == 8192 && data[2] == -16384 && data[3] == -16384, "PCM16 stereo channel expansion exact");
            }
            const float next[] = {-0.25f, 0.0f}; out.push(next, 2); out.pump();
            check(out.ok() && out.output_ratio() == 1.0 && written_frames == 2 &&
                  out.output_samples() == 4 && out.queued() == reported_padding + 1,
                  "next block consumes retained lookahead without loss or duplication");
            if (f == Format::Float || f == Format::ExtFloat) {
                auto* data = reinterpret_cast<float*>(sample_data.data());
                check(data[0] == .75f && data[1] == .75f && data[2] == -.25f && data[3] == -.25f,
                      "float stereo continuity exact across source blocks");
            } else {
                auto* data = reinterpret_cast<int16_t*>(sample_data.data());
                check(data[0] == 24575 && data[1] == 24575 && data[2] == -8192 && data[3] == -8192,
                      "PCM16 stereo continuity exact across source blocks");
            }
        }
        check(refs == 0 && initialized == uninitialized, "healthy stream balances COM ownership");
    }
    struct Case { Fault fault; const char* stage; };
    for (const auto& c : {Case{Fault::Com, "CoInitializeEx"}, {Fault::Factory, "CoCreateInstance"},
             {Fault::Endpoint, "GetDefaultAudioEndpoint"}, {Fault::Activate, "Activate"},
             {Fault::Mix, "GetMixFormat"}, {Fault::Initialize, "Initialize"}, {Fault::Size, "GetBufferSize"},
             {Fault::ZeroSize, "GetBufferSize"}, {Fault::Service, "GetService"},
             {Fault::Buffer, "GetBuffer"}, {Fault::Release, "ReleaseBuffer"}, {Fault::Start, "Start"}}) {
        reset(); fault = c.fault;
        { stv::host::AudioOut out; check(!out.open(), c.stage); closed(out, c.stage); }
        check(refs == 0 && initialized == uninitialized, "startup failure balances COM ownership");
    }
    for (Format f : {Format::ChannelsZero, Format::RateZero, Format::Align, Format::Average,
                    Format::Float64, Format::UnsupportedTag, Format::ExtShort, Format::ExtUnknown, Format::ExtBits}) {
        reset(); format = f;
        { stv::host::AudioOut out; check(!out.open(), "malformed/unsupported mix format rejected"); closed(out, "MixFormat"); }
        check(refs == 0 && initialized == uninitialized, "format rejection balances COM ownership");
    }
    for (const auto& c : {Case{Fault::Padding, "GetCurrentPadding"}, {Fault::OversizedPadding, "GetCurrentPadding"},
                         {Fault::Buffer, "GetBuffer"}, {Fault::Release, "ReleaseBuffer"}}) {
        for (bool query_first : {false, true}) {
            if (query_first && (c.fault == Fault::Buffer || c.fault == Fault::Release)) continue;
            reset();
            {
                stv::host::AudioOut out; check(out.open(), "runtime failure fixture opened");
                reported_padding = 4798; // exercise conversion faults before the healthy buffer drains
                const float pcm[] = {.25f, -.5f}; out.push(pcm, 2); fault = c.fault;
                if (query_first) (void)out.queued(); else out.pump();
                closed(out, c.stage);
            }
            check(refs == 0 && initialized == uninitialized, "runtime device failure balances COM ownership");
        }
    }
#ifndef STV_PREP_BASELINE
    reset();
    {
        stv::host::AudioOut out; check(out.open(), "null-buffer fixture opened");
        reported_padding = 4798;
        const float pcm[] = {.5f, -.5f}; out.push(pcm, 2); fault = Fault::NullBuffer; out.pump(); closed(out, "GetBuffer");
    }
    check(refs == 0 && initialized == uninitialized, "null buffer balances COM ownership");
#endif
    reset(); format = Format::Pcm;
    {
        stv::host::AudioOut out; check(out.open(), "PCM clipping fixture opened");
        reported_padding = 4796;
        const float pcm[] = {2.0f, -2.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f}; out.push(pcm, 4); out.pump();
        check(out.output_ratio() == 1.0 && written_frames == 3 && out.output_samples() == 3 &&
              out.queued() == reported_padding + 1, "clipping fixture renders all three samples at unity ratio");
        auto* data = reinterpret_cast<int16_t*>(sample_data.data());
        check(data[0] == 32767 && data[1] == 32767 && data[2] == -32767 && data[3] == -32767 && data[4] == 0 && data[5] == 0,
              "PCM16 clips finite values and silences nonfinite values");
        check(out.underruns() == 0, "first sample fill does not count startup as underrun");
        reported_padding = 0;
        out.pump(); check(out.underruns() == 1 && out.min_padding() == 0, "later empty device queue records underrun");
        out.pump(); check(out.underruns() == 1, "repeated empty pumps count one continuous underrun");
    }
    check(refs == 0 && initialized == uninitialized, "clipping fixture balances COM ownership");
    backlog_recovery();
    missing_endpoint_recovery();
    endpoint_rate_recovery();
    endpoint_recovery_failures();
    empty_buffer_priming();
    for (HRESULT mode : {S_FALSE, RPC_E_CHANGED_MODE}) {
        reset(); com_result = mode;
        { stv::host::AudioOut out; check(out.open(), "existing COM apartment opens"); check(out.open(), "reopen releases old stream before acquiring new one"); }
        check(refs == 0 && initialized == uninitialized, "existing apartment reference count balanced");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
