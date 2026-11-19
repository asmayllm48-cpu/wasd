// wasd-bench: headless connection + decode measurement. Writes per-sample CSV.
// Usage: wasd-bench [--serial S] [--seconds N] [--max-size PX] [--bitrate BPS] [--fps N] [--sw] [--h265]
//                   [--tap X Y] [--csv PATH]
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "core/input_engine.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/stats.h"
#include "platform/adb.h"
#include "platform/paths.h"
#include "session/session.h"

using namespace wasd;
using Microsoft::WRL::ComPtr;

static void printStat(const char* name, const Percentiles& p) {
    std::printf("%-22s n=%-5zu p50=%8.2f ms  p95=%8.2f ms  p99=%8.2f ms  max=%8.2f ms\n", name, p.count, p.p50 / 1000,
                p.p95 / 1000, p.p99 / 1000, p.max / 1000);
}

// Measures the T0->T2 input processing path (key/mouse event -> serialized
// control message) entirely in-process, with no device needed. Gives honest
// P50/P95/P99 numbers for the engine path of 1 vs N concurrent virtual fingers.
static int runInputBench() {
    struct Ctx {
        proto::MsgBuffer buf{};
        uint64_t serialized = 0;
        void onTouch(const proto::TouchEvent& ev) {
            serialized += proto::serializeTouch(ev, buf.data());
        }
    };
    Ctx ctx;
    InputEngine e([&](const proto::TouchEvent& ev) { ctx.onTouch(ev); });
    e.setFrameSize(1280, 720);
    Profile p;
    p.maxVirtualFingers = 8;
    for (int i = 0; i < 4; ++i) {
        Binding b;
        b.type = BindingType::Tap;
        b.key = 'Q' + i;  // Q W E R
        b.pos = {0.2f + 0.2f * i, 0.5f};
        p.bindings.push_back(b);
    }
    e.setProfile(p);

    constexpr int N = 2000;
    SampleWindow oneFinger(N), fourFingers(N);
    int64_t t = 1000000;

    // Scenario A: one finger, press+release cycles.
    for (int i = 0; i < N; ++i) {
        const int64_t s = nowMicros();
        e.onKey('Q', true, t);
        e.onKey('Q', false, t + 40000);
        oneFinger.add(static_cast<double>(nowMicros() - s));
        t += 80000;
    }
    e.releaseAll();

    // Scenario B: four concurrent fingers pressed & released together.
    for (int i = 0; i < N; ++i) {
        const int64_t s = nowMicros();
        for (int k = 0; k < 4; ++k) e.onKey('Q' + k, true, t + k);
        e.tick(t + 5000);
        for (int k = 3; k >= 0; --k) e.onKey('Q' + k, false, t + 40000 + k);
        fourFingers.add(static_cast<double>(nowMicros() - s));
        t += 80000;
    }
    e.releaseAll();

    std::printf("input engine T0->T2 processing latency (serialize included)\n");
    printStat("1 finger cycle", oneFinger.summary());
    printStat("4 fingers cycle", fourFingers.summary());
    std::printf("total touch messages serialized: %llu\n", static_cast<unsigned long long>(ctx.serialized));
    return 0;
}

int main(int argc, char** argv) {
    SessionOptions opts;
    int seconds = 10;
    std::string csv;
    bool tap = false;
    float tapX = 0.5f, tapY = 0.5f;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--input")) return runInputBench();
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--serial")) opts.serial = next();
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atoi(next());
        else if (!std::strcmp(argv[i], "--max-size")) opts.video.maxSize = std::atoi(next());
        else if (!std::strcmp(argv[i], "--bitrate")) opts.video.bitRate = std::atoi(next());
        else if (!std::strcmp(argv[i], "--fps")) opts.video.maxFps = std::atoi(next());
        else if (!std::strcmp(argv[i], "--sw")) opts.video.hardwareDecode = false;
        else if (!std::strcmp(argv[i], "--h265")) opts.video.h265 = true;
        else if (!std::strcmp(argv[i], "--csv")) csv = next();
        else if (!std::strcmp(argv[i], "--tap")) {
            tap = true;
            tapX = static_cast<float>(std::atof(next()));
            tapY = static_cast<float>(std::atof(next()));
        }
    }
    platform::Adb adb(platform::runtimeDir() + L"\\adb.exe");
    std::string err;
    if (!adb.startServer(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    auto devs = adb.devices(err);
    for (auto& d : devs) std::printf("device %s state=%s model=%s %s\n", d.serial.c_str(), d.state.c_str(), d.model.c_str(), d.transport.c_str());
    if (opts.serial.empty()) {
        for (auto& d : devs)
            if (d.usable()) opts.serial = d.serial;
    }
    if (opts.serial.empty()) {
        std::fprintf(stderr, "no authorized device (check the USB debugging prompt on the phone)\n");
        return 3;
    }
    std::printf("android %s (sdk %s), model %s\n", adb.getprop(opts.serial, "ro.build.version.release").c_str(),
                adb.getprop(opts.serial, "ro.build.version.sdk").c_str(), adb.getprop(opts.serial, "ro.product.model").c_str());

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (opts.video.hardwareDecode &&
        SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                    D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    }
    Session s(adb, platform::runtimeDir() + L"\\scrcpy-server");
    s.start(opts, dev.Get(), [](video::DecodedFrame&&) {}, [](uint32_t, uint32_t) {});
    const int64_t t0 = nowMicros();
    while (s.state() == SessionState::Starting && nowMicros() - t0 < 15000000) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (s.state() != SessionState::Running) {
        std::fprintf(stderr, "session failed: %s\n", s.lastError().c_str());
        return 4;
    }
    std::printf("connected in %.0f ms: device=\"%s\" decoder=%s\n", (nowMicros() - t0) / 1000.0, s.deviceName().c_str(),
                s.decoderDescription().c_str());
    const int64_t tRun = nowMicros();
    bool tapped = false;
    while (nowMicros() - tRun < seconds * 1000000LL && s.state() == SessionState::Running) {
        if (tap && !tapped && s.frameWidth()) {
            proto::TouchEvent ev;
            ev.pointerId = 0x1000;
            ev.frameWidth = static_cast<uint16_t>(s.frameWidth());
            ev.frameHeight = static_cast<uint16_t>(s.frameHeight());
            ev.x = static_cast<int32_t>(tapX * ev.frameWidth);
            ev.y = static_cast<int32_t>(tapY * ev.frameHeight);
            ev.action = proto::MotionAction::Down;
            s.sendTouch(ev, nowMicros());
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            ev.action = proto::MotionAction::Up;
            s.sendTouch(ev, nowMicros());
            tapped = true;
            std::printf("tap sent at %d,%d\n", ev.x, ev.y);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const double secs = (nowMicros() - tRun) / 1e6;
    std::printf("video %ux%u, %llu frames decoded in %.1f s = %.1f fps, %.2f Mbit/s, decode errors %llu\n", s.frameWidth(),
                s.frameHeight(), static_cast<unsigned long long>(s.counters.decodedFrames.load()), secs,
                s.counters.decodedFrames / secs, s.counters.videoBytes * 8 / secs / 1e6,
                static_cast<unsigned long long>(s.counters.decodeErrors.load()));
    printStat("decode (rx->frame)", s.decodeLatencyUs.summary());
    printStat("packet interval", s.frameIntervalUs.summary());
    printStat("control send", s.sendLatencyUs.summary());
    if (!csv.empty()) {
        std::vector<CsvRow> rows;
        const std::string run = std::to_string(t0), mode = std::to_string(s.frameWidth()) + "x" + std::to_string(s.frameHeight());
        auto add = [&](const char* metric, const SampleWindow& w) {
            auto v = w.snapshot();
            for (std::size_t i = 0; i < v.size(); ++i) rows.push_back({run, opts.serial, mode, metric, "internal", i, v[i]});
        };
        add("decode_us", s.decodeLatencyUs);
        add("packet_interval_us", s.frameIntervalUs);
        add("control_send_us", s.sendLatencyUs);
        std::printf(writeCsv(csv, rows, &err) ? "csv written: %s\n" : "csv failed: %s\n", writeCsv(csv, rows, &err) ? csv.c_str() : err.c_str());
    }
    s.stop();
    return 0;
}
