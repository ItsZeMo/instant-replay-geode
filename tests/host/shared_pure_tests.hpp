#pragma once

// Platform-independent host tests shared by tests/host/test_host.cpp (Windows, MSVC / clang-cl) and
// tests/host/test_host_mac.mm (macOS), so the pure recorder / encoder math runs in both CI jobs (K7).
// Keep this file free of platform headers.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "capture/FrameBudget.hpp"
#include "capture/GameAudioRing.hpp"
#include "capture/MicStatusPolicy.hpp"
#include "config/BitrateMath.hpp"
#include "config/CodecUtils.hpp"
#include "encoder/ConcatTimestampMath.hpp"
#include "recorder/AudioQueuePolicy.hpp"
#include "recorder/MicMixFifo.hpp"
#include "recorder/SegmentRules.hpp"

#define IR_SHARED_CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL at " << __FILE__ << ":" << __LINE__ << ": " #expr "\n"; \
        std::exit(1); \
    } \
} while (0)

namespace replay_host_tests {

inline void testConcatTimestampMathPure() {
    std::cout << "[Test 1m] ConcatTimestampState C1 seam anchoring, anomaly fallback & B-frame DTS math...\n";
    // 1d. C1 Acceptance exact test on ConcatTimestampState:
    // Segment closes at S = 300,000,000 HNS (30s @ 60 FPS); audio ends cleanly at S,
    // last video frame ends at S + frameDur - 0.1 ms (PTS = S - 1000 HNS).
    // Next segment's first audio sample MUST land on exact S, and video PTS/DTS remain monotonic!
    {
        constexpr int64_t kFrameDur = 166667; // 1/60s in HNS
        constexpr int64_t S = 300000000;
        replay::ConcatTimestampState st(0);
        st.beginSegment();

        const int64_t seg1VideoPts = S - 1000; // ends at S + kFrameDur - 1000
        IR_SHARED_CHECK(st.recordWrittenSample(0, true, seg1VideoPts, kFrameDur));
        // Primary game audio (writerIndex=1) ends at exact S
        IR_SHARED_CHECK(st.recordWrittenSample(1, false, S - 213333, 213333));

        bool anomalyWarn = false;
        int64_t excess = 0;
        st.endSegment(kFrameDur, &anomalyWarn, &excess);
        IR_SHARED_CHECK(!anomalyWarn);
        IR_SHARED_CHECK(st.outputBase == S); // MUST anchor to primary audio end S, NOT S + kFrameDur - 1000!

        st.beginSegment();
        int64_t seg2AudioPts = st.computeSampleTime(0, 0);
        IR_SHARED_CHECK(seg2AudioPts == S); // 0 HNS gap between seg1 audio end and seg2 audio start!
        IR_SHARED_CHECK(st.recordWrittenSample(1, false, seg2AudioPts, 213333));

        int64_t seg2VideoPts = st.computeSampleTime(0, 0);
        int64_t seg2VideoDts = 0;
        st.adjustVideoTimestamps(0, 0, seg2VideoPts, seg2VideoDts);
        IR_SHARED_CHECK(seg2VideoPts > seg1VideoPts); // Strictly monotonic video PTS across seam
        IR_SHARED_CHECK(seg2VideoDts == S);
    }

    // 1e. C1 Anomaly fallback (> 1.5 frames longer video than primary audio):
    // Video exceeds primary audio by 2.0 frames -> falls back to segMaxEnd and signals anomalyWarn!
    {
        constexpr int64_t kFrameDur = 166667;
        constexpr int64_t S = 300000000;
        replay::ConcatTimestampState st(0);
        st.beginSegment();
        IR_SHARED_CHECK(st.recordWrittenSample(1, false, S - 213333, 213333)); // audio ends at S
        IR_SHARED_CHECK(st.recordWrittenSample(0, true, S + kFrameDur, kFrameDur)); // video ends at S + 2*kFrameDur

        bool anomalyWarn = false;
        int64_t excess = 0;
        st.endSegment(kFrameDur, &anomalyWarn, &excess);
        IR_SHARED_CHECK(anomalyWarn);
        IR_SHARED_CHECK(excess == 2 * kFrameDur);
        IR_SHARED_CHECK(st.outputBase == S + 2 * kFrameDur);
    }

    // 1f. B-frames + initialPtsShift + cutHns + C1 endSegment
    {
        constexpr int64_t kFrameDur = 333333; // 30 FPS
        const int64_t cutHns = 1000000;
        const int64_t firstDts = 666667;
        const int64_t initialShift = cutHns - firstDts;
        replay::ConcatTimestampState st(initialShift);
        st.beginSegment();

        int64_t pts0 = st.computeSampleTime(1000000, cutHns); int64_t dts0 = 0;
        st.adjustVideoTimestamps(666667, cutHns, pts0, dts0);
        IR_SHARED_CHECK(dts0 == 0 && pts0 == 333333);
        IR_SHARED_CHECK(st.recordWrittenSample(0, true, pts0, kFrameDur));

        int64_t pts1 = st.computeSampleTime(1666667, cutHns); int64_t dts1 = 0;
        st.adjustVideoTimestamps(1000000, cutHns, pts1, dts1);
        IR_SHARED_CHECK(dts1 == 333333 && pts1 == 1000000);
        IR_SHARED_CHECK(st.recordWrittenSample(0, true, pts1, kFrameDur));

        int64_t pts2 = st.computeSampleTime(1333333, cutHns); int64_t dts2 = 0;
        st.adjustVideoTimestamps(1333333, cutHns, pts2, dts2);
        IR_SHARED_CHECK(dts2 == 666666 && pts2 == 666666);
        IR_SHARED_CHECK(st.recordWrittenSample(0, true, pts2, kFrameDur));

        int64_t ap = st.computeSampleTime(2286667, cutHns);
        IR_SHARED_CHECK(st.recordWrittenSample(1, false, ap, 213333));
        bool anomalyWarn = false;
        st.endSegment(kFrameDur, &anomalyWarn);
        IR_SHARED_CHECK(!anomalyWarn);
        IR_SHARED_CHECK(st.outputBase == ap + 213333);

        st.beginSegment();
        int64_t s2a = st.computeSampleTime(0, 0);
        IR_SHARED_CHECK(s2a == ap + 213333);
        IR_SHARED_CHECK(st.recordWrittenSample(1, false, s2a, 213333));
        int64_t s2vPts = st.computeSampleTime(333333, 0); int64_t s2vDts = 0;
        st.adjustVideoTimestamps(0, 0, s2vPts, s2vDts);
        IR_SHARED_CHECK(s2vDts > dts2);
        IR_SHARED_CHECK(s2vPts - s2vDts == 333333);
    }

    // N4: 1-sample HNS tolerance absorbs independent 100 ns rounding of AAC packet PTS vs duration
    // (1024 / 48000 s = 213333.33 HNS) over 10,000 packets without hiding a real overlap.
    {
        IR_SHARED_CHECK(replay::ConcatTimestampState::audioOverlapToleranceHns(48000) == 209);
        IR_SHARED_CHECK(replay::ConcatTimestampState::audioOverlapToleranceHns(44100) == 227);
        replay::ConcatTimestampState st(0);
        st.beginSegment();
        const int64_t tol = replay::ConcatTimestampState::audioOverlapToleranceHns(48000);
        for (int64_t k = 0; k < 10000; ++k) {
            const int64_t pts = (k * 1024 * 10000000LL) / 48000;
            const int64_t dur = ((k + 1) * 1024 * 10000000LL) / 48000 - pts + 1; // end rounded up by 1 HNS
            IR_SHARED_CHECK(st.recordWrittenSample(1, false, pts, dur, nullptr, tol));
        }
        int64_t prevEnd = 0;
        IR_SHARED_CHECK(!st.recordWrittenSample(1, false, (9999 * 1024 * 10000000LL) / 48000, 213333, &prevEnd, tol));
        IR_SHARED_CHECK(prevEnd > 0);
    }

    // N4: macOS keeps negative B-frame DTS (allowNegativeDts) and unclamped PTS; Windows clamps both.
    {
        replay::ConcatTimestampState st(0);
        st.beginSegment();
        int64_t pts = 0;
        int64_t dts = 0;
        st.adjustVideoTimestamps(-333333, 0, pts, dts, false, true);
        IR_SHARED_CHECK(dts == -333333 && pts == 0);
        replay::ConcatTimestampState win(0);
        win.beginSegment();
        int64_t wpts = 0;
        int64_t wdts = 0;
        win.adjustVideoTimestamps(-333333, 0, wpts, wdts);
        IR_SHARED_CHECK(wdts == 0 && wpts == 0);
    }

    std::cout << "  PASS Test 1m\n";
}

inline void testFrameBudgetAnd4KPool() {
    std::cout << "[Test 7] Frame memory pool, worker queue & encoder in-flight budgets...\n";
    constexpr size_t kPoolBaseBytes = 50ull * 1024 * 1024;
    constexpr size_t kPoolCeilingBytes = 256ull * 1024 * 1024;
    constexpr size_t kPoolMaxFrames = 6;
    constexpr size_t kPoolMinFrames = 4;

    constexpr size_t kQueueBaseBytes = 64ull * 1024 * 1024;
    constexpr size_t kQueueCeilingBytes = 256ull * 1024 * 1024;
    constexpr size_t kQueueMaxFrames = 16;
    constexpr size_t kQueueMinFrames = 4;

    constexpr size_t kInFlightMaxBytes = 256ull * 1024 * 1024;
    constexpr size_t kInFlightMaxFrames = 16;
    constexpr size_t kInFlightMinFrames = 4;

    struct Buf { size_t byteSize; };
    auto getSz = [](const Buf& b) noexcept { return b.byteSize; };

    auto simulateFill = [&](size_t frameBytes, size_t maxCount, size_t effMaxBytes, bool isQueue) -> std::pair<size_t, size_t> {
        std::vector<std::unique_ptr<Buf>> container;
        for (int i = 0; i < 32; ++i) {
            size_t total = replay::sumFrameBufferBytes(container, getSz);
            bool ok = isQueue
                ? replay::canEnqueueVideoFrameBytes(container.size(), total, frameBytes, maxCount, effMaxBytes)
                : replay::canRetainPooledFrameBytes(container.size(), total, frameBytes, maxCount, effMaxBytes);
            if (ok) container.push_back(std::make_unique<Buf>(Buf{frameBytes}));
        }
        return {container.size(), replay::sumFrameBufferBytes(container, getSz)};
    };

    auto simulateInFlight = [&](size_t frameBytes) -> std::pair<size_t, size_t> {
        std::vector<std::unique_ptr<Buf>> inFlight;
        for (int i = 0; i < 32; ++i) {
            size_t total = replay::sumFrameBufferBytes(inFlight, getSz);
            if (replay::canAcceptInFlightFrameBytes(inFlight.size(), total, frameBytes, kInFlightMaxFrames, kInFlightMaxBytes, kInFlightMinFrames)) {
                inFlight.push_back(std::make_unique<Buf>(Buf{frameBytes}));
            }
        }
        return {inFlight.size(), replay::sumFrameBufferBytes(inFlight, getSz)};
    };

    const size_t sz1080 = 1920ull * 1080 * 4;
    const size_t sz1440 = 2560ull * 1440 * 4;
    const size_t sz4k   = 3840ull * 2160 * 4;
    const size_t sz8k   = 7680ull * 4320 * 4;
    const size_t sz16k  = 15360ull * 8640 * 4;

    const size_t effP1080 = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz1080, kPoolMinFrames, kPoolCeilingBytes);
    const size_t effQ1080 = replay::effectiveFrameByteBudget(kQueueBaseBytes, sz1080, kQueueMinFrames, kQueueCeilingBytes);
    IR_SHARED_CHECK(effP1080 == kPoolBaseBytes && effQ1080 == kQueueBaseBytes);
    auto [p1080c, p1080b] = simulateFill(sz1080, kPoolMaxFrames, effP1080, false);
    IR_SHARED_CHECK(p1080c == 6 && p1080b <= kPoolBaseBytes);
    auto [q1080c, q1080b] = simulateFill(sz1080, kQueueMaxFrames, effQ1080, true);
    IR_SHARED_CHECK(q1080c == 8 && q1080b <= kQueueBaseBytes);
    auto [f1080c, f1080b] = simulateInFlight(sz1080);
    IR_SHARED_CHECK(f1080c == 16 && f1080b <= kInFlightMaxBytes);

    const size_t effP1440 = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz1440, kPoolMinFrames, kPoolCeilingBytes);
    const size_t effQ1440 = replay::effectiveFrameByteBudget(kQueueBaseBytes, sz1440, kQueueMinFrames, kQueueCeilingBytes);
    IR_SHARED_CHECK(effP1440 == sz1440 * 4 && effQ1440 == kQueueBaseBytes);
    auto [p1440c, p1440b] = simulateFill(sz1440, kPoolMaxFrames, effP1440, false);
    IR_SHARED_CHECK(p1440c == 4 && p1440b == sz1440 * 4);
    auto [q1440c, q1440b] = simulateFill(sz1440, kQueueMaxFrames, effQ1440, true);
    IR_SHARED_CHECK(q1440c == 4 && q1440b <= kQueueBaseBytes);
    auto [f1440c, f1440b] = simulateInFlight(sz1440);
    IR_SHARED_CHECK(f1440c == 16 && f1440b <= kInFlightMaxBytes);

    const size_t effP4k = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz4k, kPoolMinFrames, kPoolCeilingBytes);
    const size_t effQ4k = replay::effectiveFrameByteBudget(kQueueBaseBytes, sz4k, kQueueMinFrames, kQueueCeilingBytes);
    IR_SHARED_CHECK(effP4k == sz4k * 4 && effQ4k == sz4k * 4);
    auto [p4kc, p4kb] = simulateFill(sz4k, kPoolMaxFrames, effP4k, false);
    IR_SHARED_CHECK(p4kc == 4 && p4kb == sz4k * 4);
    auto [q4kc, q4kb] = simulateFill(sz4k, kQueueMaxFrames, effQ4k, true);
    IR_SHARED_CHECK(q4kc == 4 && q4kb == sz4k * 4);
    auto [f4kc, f4kb] = simulateInFlight(sz4k);
    IR_SHARED_CHECK(f4kc == 8 && f4kb == sz4k * 8 && f4kb <= kInFlightMaxBytes);

    const size_t effP8k = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz8k, kPoolMinFrames, kPoolCeilingBytes);
    IR_SHARED_CHECK(effP8k == kPoolCeilingBytes);
    auto [p8kc, p8kb] = simulateFill(sz8k, kPoolMaxFrames, effP8k, false);
    IR_SHARED_CHECK(p8kc == 2 && p8kb <= kPoolCeilingBytes);
    auto [f8kc, f8kb] = simulateInFlight(sz8k);
    IR_SHARED_CHECK(f8kc == 4);

    const size_t effP16k = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz16k, kPoolMinFrames, kPoolCeilingBytes);
    const size_t effQ16k = replay::effectiveFrameByteBudget(kQueueBaseBytes, sz16k, kQueueMinFrames, kQueueCeilingBytes);
    IR_SHARED_CHECK(effP16k == kPoolCeilingBytes && effQ16k == kQueueCeilingBytes);
    auto [p16kc, p16kb] = simulateFill(sz16k, kPoolMaxFrames, effP16k, false);
    IR_SHARED_CHECK(p16kc == 0 && p16kb == 0);
    auto [q16kc, q16kb] = simulateFill(sz16k, kQueueMaxFrames, effQ16k, true);
    IR_SHARED_CHECK(q16kc == 1 && q16kb == sz16k);

    auto runPoolSim = [&](size_t poolBudgetBytes) -> size_t {
        std::vector<std::unique_ptr<Buf>> pool;
        std::vector<std::unique_ptr<Buf>> hwQueue;
        size_t steadyStateAllocs = 0;
        for (int frame = 0; frame < 240; ++frame) {
            std::unique_ptr<Buf> cur;
            if (!pool.empty()) {
                cur = std::move(pool.back());
                pool.pop_back();
            } else {
                cur = std::make_unique<Buf>(Buf{sz4k});
                if (frame >= 10) steadyStateAllocs++;
            }
            hwQueue.push_back(std::move(cur));

            if (frame % 2 == 1 && hwQueue.size() >= 3) {
                for (int r = 0; r < 2; ++r) {
                    auto done = std::move(hwQueue.front());
                    hwQueue.erase(hwQueue.begin());
                    size_t total = replay::sumFrameBufferBytes(pool, getSz);
                    if (replay::canRetainPooledFrameBytes(pool.size(), total, done->byteSize, kPoolMaxFrames, poolBudgetBytes)) {
                        pool.push_back(std::move(done));
                    }
                }
            }
        }
        return steadyStateAllocs;
    };
    IR_SHARED_CHECK(runPoolSim(kPoolBaseBytes) > 100);
    IR_SHARED_CHECK(runPoolSim(effP4k) == 0);

    // W11: the encoder in-flight byte budget scales to ~12 frames for large frames, capped at 512 MiB.
    constexpr size_t kScaledInFlightCeiling = 512ull * 1024 * 1024;
    IR_SHARED_CHECK(replay::effectiveFrameByteBudget(kInFlightMaxBytes, sz1080, 12, kScaledInFlightCeiling) == kInFlightMaxBytes);
    const size_t budget4k = replay::effectiveFrameByteBudget(kInFlightMaxBytes, sz4k, 12, kScaledInFlightCeiling);
    IR_SHARED_CHECK(budget4k == sz4k * 12 && budget4k <= kScaledInFlightCeiling);
    {
        std::vector<std::unique_ptr<Buf>> inFlight;
        for (int i = 0; i < 32; ++i) {
            size_t total = replay::sumFrameBufferBytes(inFlight, getSz);
            if (replay::canAcceptInFlightFrameBytes(inFlight.size(), total, sz4k, kInFlightMaxFrames, budget4k, kInFlightMinFrames)) {
                inFlight.push_back(std::make_unique<Buf>(Buf{sz4k}));
            }
        }
        IR_SHARED_CHECK(inFlight.size() == 12); // was 8 with the flat 256 MiB budget
    }
    IR_SHARED_CHECK(replay::effectiveFrameByteBudget(kInFlightMaxBytes, sz8k, 12, kScaledInFlightCeiling) == kScaledInFlightCeiling);

    std::cout << "  PASS Test 7\n";
}

inline void testGameAudioRing() {
    std::cout << "[Test 8] SPSC GameAudioRing deterministic interleaving & overflow...\n";
    auto ring = std::make_unique<replay::GameAudioRing>();

    std::vector<float> pkt1(512 * 2, 0.25f);
    std::vector<float> pkt2(512 * 2, 0.75f);
    const int64_t t1EndNs = 10'000'000'000LL;
    const int64_t t2EndNs = 15'000'000'000LL;
    const int64_t pktDurNs = (512LL * 1000000000LL) / 48000LL;

    IR_SHARED_CHECK(ring->push(pkt1.data(), 512, 2, 1.0f, 48000, t1EndNs));

    std::vector<float> out;
    int outRate = 0;
    std::chrono::steady_clock::time_point outTs{};
    size_t drained1 = ring->drain(out, outRate, outTs, [&]() {
        IR_SHARED_CHECK(ring->push(pkt2.data(), 512, 2, 1.0f, 48000, t2EndNs));
    });
    IR_SHARED_CHECK(drained1 == 512);
    IR_SHARED_CHECK(outRate == 48000);
    IR_SHARED_CHECK(std::abs(out[0] - 0.25f) < 1e-4f);
    IR_SHARED_CHECK(outTs.time_since_epoch().count() == (t1EndNs - pktDurNs));

    size_t drained2 = ring->drain(out, outRate, outTs);
    IR_SHARED_CHECK(drained2 == 512);
    IR_SHARED_CHECK(std::abs(out[0] - 0.75f) < 1e-4f);
    IR_SHARED_CHECK(outTs.time_since_epoch().count() == (t2EndNs - pktDurNs));

    ring->reset();
    std::vector<float> big(1024 * 2, 0.5f);
    int pushed = 0;
    for (int i = 0; i < 300; ++i) {
        if (ring->push(big.data(), 1024, 2, 1.0f, 48000, 1'000'000'000LL + i * 21333333LL)) {
            pushed++;
        }
    }
    IR_SHARED_CHECK(pushed == 256);
    IR_SHARED_CHECK(!ring->push(big.data(), 1024, 2, 1.0f, 48000, 99'000'000'000LL));
    size_t allFrames = ring->drain(out, outRate, outTs);
    IR_SHARED_CHECK(allFrames == replay::GameAudioRing::kRingFrames);
    IR_SHARED_CHECK(ring->push(pkt1.data(), 512, 2, 1.0f, 48000, 50'000'000'000LL));

    ring->reset();
    float oneFrame[2] = {0.1f, -0.1f};
    int metaPushed = 0;
    for (size_t i = 0; i < replay::GameAudioRing::kMetaCapacity + 100; ++i) {
        if (ring->push(oneFrame, 1, 2, 1.0f, 48000, 2'000'000'000LL + static_cast<int64_t>(i) * 1000000LL)) {
            metaPushed++;
        }
    }
    IR_SHARED_CHECK(metaPushed == static_cast<int>(replay::GameAudioRing::kMetaCapacity));
    size_t metaFrames = ring->drain(out, outRate, outTs);
    IR_SHARED_CHECK(metaFrames == replay::GameAudioRing::kMetaCapacity);
    IR_SHARED_CHECK(ring->push(pkt2.data(), 512, 2, 1.0f, 48000, 60'000'000'000LL));

    std::cout << "  PASS Test 8\n";
}

inline void testMicMixFifo() {
    std::cout << "[Test 9] Time-aligned MicMixFifo (drift, pause, stale discard, overlap, rebase)...\n";
    constexpr int kRate = 48000;
    constexpr size_t kMaxBuf = kRate * 5;

    {
        replay::MicMixFifo fifo;
        std::vector<float> mic(960 * 2, 0.25f);
        fifo.appendResampled(mic.data(), 960, kRate, 7200, kMaxBuf);

        std::vector<float> game(4800 * 2, 0.10f);
        IR_SHARED_CHECK(fifo.mixInto(game.data(), 4800, 0) == 0);
        IR_SHARED_CHECK(std::abs(game[0] - 0.10f) < 1e-5f);
        IR_SHARED_CHECK(fifo.availFrames() == 960);

        std::vector<float> game2(4800 * 2, 0.10f);
        IR_SHARED_CHECK(fifo.mixInto(game2.data(), 4800, 4800) == 960);
        IR_SHARED_CHECK(std::abs(game2[0] - 0.10f) < 1e-5f);
        IR_SHARED_CHECK(std::abs(game2[2399 * 2] - 0.10f) < 1e-5f);
        IR_SHARED_CHECK(std::abs(game2[2400 * 2] - 0.35f) < 1e-5f);
        IR_SHARED_CHECK(std::abs(game2[3359 * 2] - 0.35f) < 1e-5f);
        IR_SHARED_CHECK(std::abs(game2[3360 * 2] - 0.10f) < 1e-5f);
        IR_SHARED_CHECK(fifo.availFrames() == 0);
    }

    {
        replay::MicMixFifo fifo;
        std::vector<float> oldMic(4800 * 2, 0.40f);
        fifo.appendResampled(oldMic.data(), 4800, kRate, 0, kMaxBuf);
        std::vector<float> recentMic(4800 * 2, 0.20f);
        fifo.appendResampled(recentMic.data(), 4800, kRate, 6000, kMaxBuf);
        std::vector<float> game(4800 * 2, 0.10f);
        size_t mixed = fifo.mixInto(game.data(), 4800, 9600);
        IR_SHARED_CHECK(mixed == 1920);
        IR_SHARED_CHECK(std::abs(game[0] - 0.30f) < 1e-5f);
        IR_SHARED_CHECK(fifo.availFrames() == 0);

        std::vector<float> veryOld(2400 * 2, 0.50f);
        fifo.appendResampled(veryOld.data(), 2400, kRate, 0, kMaxBuf);
        std::vector<float> gameLate(4800 * 2, 0.10f);
        IR_SHARED_CHECK(fifo.mixInto(gameLate.data(), 4800, 9600) == 0);
        IR_SHARED_CHECK(std::abs(gameLate[0] - 0.10f) < 1e-5f);
        IR_SHARED_CHECK(fifo.availFrames() == 0);
    }

    for (int sign : {-1, 1}) {
        replay::MicMixFifo fifo;
        std::vector<float> micPkt(960 * 2, 0.1f);
        std::vector<float> gameBuf(960 * 2, 0.0f);
        const int64_t totalChunks = 90000;
        int64_t maxAbsSkew = 0;

        for (int64_t c = 0; c < totalChunks; ++c) {
            int64_t gameStart = c * 960;
            int64_t driftSamples = (gameStart * (-sign)) / 10000;
            int64_t micStart = std::max<int64_t>(0, gameStart + driftSamples);

            fifo.appendResampled(micPkt.data(), 960, kRate, micStart, kMaxBuf);
            std::fill(gameBuf.begin(), gameBuf.end(), 0.0f);
            fifo.mixInto(gameBuf.data(), 960, gameStart);

            if (fifo.hasAnchor) {
                int64_t skew = std::llabs((gameStart + 960) - fifo.headSample);
                maxAbsSkew = std::max(maxAbsSkew, skew);
            }
        }
        IR_SHARED_CHECK(maxAbsSkew <= 1920 + 960);
    }

    {
        replay::MicMixFifo fifo;
        std::vector<float> mic(960 * 2, 0.25f);
        for (int c = 0; c < 50; ++c) {
            fifo.appendResampled(mic.data(), 960, kRate, c * 960, kMaxBuf);
            std::vector<float> g(960 * 2, 0.1f);
            IR_SHARED_CHECK(fifo.mixInto(g.data(), 960, c * 960) > 0);
        }
        fifo.appendResampled(mic.data(), 960, kRate, 144000, kMaxBuf);
        std::vector<float> gResume(960 * 2, 0.1f);
        size_t mixed = fifo.mixInto(gResume.data(), 960, 144000);
        IR_SHARED_CHECK(mixed > 0);
        IR_SHARED_CHECK(std::abs(gResume[mixed * 2 - 2] - 0.35f) < 1e-5f);
    }

    {
        replay::MicMixFifo fifo;
        auto seg0 = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        auto seg1 = seg0 + std::chrono::seconds(30);

        std::vector<float> mic(2400 * 2, 0.30f);
        fifo.appendResampled(mic.data(), 2400, kRate, 1440000 + 2400, kMaxBuf);
        IR_SHARED_CHECK(fifo.headSample == 1442400);

        fifo.rebaseSegmentOrigin(seg0, seg1);
        IR_SHARED_CHECK(fifo.headSample == 2400);

        fifo.advanceOnGameOverlap(3600, 1200);
        IR_SHARED_CHECK(fifo.headSample == 3600);
        IR_SHARED_CHECK(fifo.availFrames() == 1200);

        std::vector<float> game(2400 * 2, 0.10f);
        IR_SHARED_CHECK(fifo.mixInto(game.data(), 2400, 3600) == 1200);
        IR_SHARED_CHECK(std::abs(game[0] - 0.40f) < 1e-5f);
    }

    std::cout << "  PASS Test 9\n";
}

inline void testH264BoundsAndCodecSpecC3() {
    std::cout << "[Test 10] H.264 4096x2304 ultrawide clamping & shared custom codec parser...\n";

    // C3 acceptance cases: 21:9 2160p -> 4096x1714, 32:9 2160p -> 4096x1152
    {
        auto r1 = replay::clampDimensionsToBounds(5160, 2160);
        IR_SHARED_CHECK(r1.clamped);
        IR_SHARED_CHECK(r1.width == 4096 && r1.height == 1714);
        IR_SHARED_CHECK((r1.width % 2) == 0 && (r1.height % 2) == 0);

        auto r2 = replay::clampDimensionsToBounds(7680, 2160);
        IR_SHARED_CHECK(r2.clamped);
        IR_SHARED_CHECK(r2.width == 4096 && r2.height == 1152);
        IR_SHARED_CHECK((r2.width % 2) == 0 && (r2.height % 2) == 0);

        // 32:9 1440p native (5120x1440 -> 4096x1152)
        auto r3 = replay::clampDimensionsToBounds(5120, 1440);
        IR_SHARED_CHECK(r3.clamped);
        IR_SHARED_CHECK(r3.width == 4096 && r3.height == 1152);

        // Height > 2304 clamping (3440x2400 -> 3302x2304)
        auto r4 = replay::clampDimensionsToBounds(3440, 2400);
        IR_SHARED_CHECK(r4.clamped);
        IR_SHARED_CHECK(r4.width == 3302 && r4.height == 2304);
        IR_SHARED_CHECK((r4.width % 2) == 0 && (r4.height % 2) == 0);

        // Standard 16:9 4K stays untouched
        auto r5 = replay::clampDimensionsToBounds(3840, 2160);
        IR_SHARED_CHECK(!r5.clamped);
        IR_SHARED_CHECK(r5.width == 3840 && r5.height == 2160);
    }

    // Shared custom codec spec parser
    {
        auto av1 = replay::resolveRequestedCodecSpec(0, true, "");
        IR_SHARED_CHECK(av1.family == replay::VideoCodecFamily::AV1 && av1.hardware);

        auto hevc = replay::resolveRequestedCodecSpec(1, false, "");
        IR_SHARED_CHECK(hevc.family == replay::VideoCodecFamily::HEVC && !hevc.hardware);

        auto h264 = replay::resolveRequestedCodecSpec(2, true, "");
        IR_SHARED_CHECK(h264.family == replay::VideoCodecFamily::H264 && h264.hardware);

        auto custAv1 = replay::resolveRequestedCodecSpec(3, true, "av1_nvenc");
        IR_SHARED_CHECK(custAv1.family == replay::VideoCodecFamily::AV1 && custAv1.hardware);

        auto custHevc = replay::resolveRequestedCodecSpec(3, true, "HEVC_AMF");
        IR_SHARED_CHECK(custHevc.family == replay::VideoCodecFamily::HEVC && custHevc.hardware);

        auto custSw264 = replay::resolveRequestedCodecSpec(3, true, "software_h264");
        IR_SHARED_CHECK(custSw264.family == replay::VideoCodecFamily::H264 && !custSw264.hardware);

        auto custEmpty = replay::resolveRequestedCodecSpec(3, true, "");
        IR_SHARED_CHECK(custEmpty.family == replay::VideoCodecFamily::H264 && custEmpty.hardware);
    }

    std::cout << "  PASS Test 10\n";
}

inline void testCodecFallbackBitratesW1() {
    std::cout << "[Test W1] Per-family session bitrates for codec fallbacks & CUSTOM multipliers...\n";
    using replay::VideoCodecFamily;
    const int vbr = replay::kRateControlAdaptiveVbr;

    // Dev PC settings: base 80 / max 120 Mbps, 2160p preset (x2.4), 60 FPS, Adaptive VBR.
    const double fps60 = replay::fpsBitrateMultiplier(60);
    const auto table = replay::computeCodecBitrateTable(80, 120, 80, vbr, 2.4, fps60);
    auto explicitSelection = [&](VideoCodecFamily f) {
        return replay::computeEffectiveBitrates(80, 120, 80, vbr, 2.4, fps60, f);
    };
    IR_SHARED_CHECK(table.valid);
    IR_SHARED_CHECK(table.forFamily(VideoCodecFamily::AV1).meanMbps == 134);
    IR_SHARED_CHECK(table.forFamily(VideoCodecFamily::AV1).maxMbps == 202);

    // AV1 -> HEVC and AV1 -> H.264 fallbacks get exactly what an explicit HEVC / H.264 selection gets.
    const auto hevc = table.forFamily(VideoCodecFamily::HEVC);
    const auto h264 = table.forFamily(VideoCodecFamily::H264);
    IR_SHARED_CHECK(hevc.meanMbps == explicitSelection(VideoCodecFamily::HEVC).meanMbps && hevc.meanMbps == 163);
    IR_SHARED_CHECK(hevc.maxMbps == explicitSelection(VideoCodecFamily::HEVC).maxMbps && hevc.maxMbps == 245);
    IR_SHARED_CHECK(h264.meanMbps == explicitSelection(VideoCodecFamily::H264).meanMbps && h264.meanMbps == 211);
    IR_SHARED_CHECK(h264.maxMbps == 300); // 316.8 clamped to the 300 Mbps ceiling

    // Exhaustive: every base/res/fps/rc combination keeps the table identical to direct per-family math.
    for (int base : {5, 12, 35, 50, 80, 133, 250}) {
        for (double res : {0.5, 1.0, 1.6, 2.4}) {
            for (int fps : {30, 60, 120, 144, 240}) {
                for (int rc : {replay::kRateControlAdaptiveVbr, replay::kRateControlCbr, replay::kRateControlQuality}) {
                    const auto t = replay::computeCodecBitrateTable(base, base * 2, 65, rc, res, replay::fpsBitrateMultiplier(fps));
                    for (auto fam : {VideoCodecFamily::H264, VideoCodecFamily::HEVC, VideoCodecFamily::AV1}) {
                        const auto d = replay::computeEffectiveBitrates(base, base * 2, 65, rc, res, replay::fpsBitrateMultiplier(fps), fam);
                        IR_SHARED_CHECK(t.forFamily(fam).meanMbps == d.meanMbps && t.forFamily(fam).maxMbps == d.maxMbps);
                        IR_SHARED_CHECK(d.maxMbps >= d.meanMbps);
                    }
                }
            }
        }
    }

    // CBR is an explicit target: never scaled per codec.
    const auto cbr = replay::computeCodecBitrateTable(80, 120, 80, replay::kRateControlCbr, 2.4, fps60);
    IR_SHARED_CHECK(cbr.forFamily(VideoCodecFamily::AV1).meanMbps == 80 && cbr.forFamily(VideoCodecFamily::H264).meanMbps == 80);

    // CUSTOM strings use the parsed family's multiplier instead of a flat x1.00.
    auto customMult = [](const char* s) {
        return replay::codecBitrateMultiplier(replay::resolveRequestedCodecSpec(3, true, s).family);
    };
    IR_SHARED_CHECK(std::abs(customMult("av1_nvenc") - 0.70) < 1e-9);
    IR_SHARED_CHECK(std::abs(customMult("software_h264") - 1.10) < 1e-9);
    IR_SHARED_CHECK(std::abs(customMult("hevc_amf") - 0.85) < 1e-9);
    // Preset H.264 and custom "h264" resolve to the same family -> same effective bitrate.
    IR_SHARED_CHECK(replay::resolveRequestedCodecSpec(3, true, "h264").family == replay::resolveRequestedCodecSpec(2, true, "").family);

    std::cout << "  PASS Test W1\n";
}

inline void testResolutionBitrateSemanticsW9() {
    std::cout << "[Test W9] Bitrate scales with the encoded size (Native 4K == 2160p preset)...\n";
    auto approxEq = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    // 16:9 sizes reproduce the preset table.
    IR_SHARED_CHECK(approxEq(replay::resolutionBitrateMultiplier(1280, 720), 0.50));
    IR_SHARED_CHECK(approxEq(replay::resolutionBitrateMultiplier(1920, 1080), 1.00));
    IR_SHARED_CHECK(approxEq(replay::resolutionBitrateMultiplier(2560, 1440), 1.60));
    IR_SHARED_CHECK(approxEq(replay::resolutionBitrateMultiplier(3840, 2160), 2.40));
    // Native 3840x2160 and the 2160p preset both encode 3840x2160 -> identical effective bitrate
    // (previously Native used h/1080 = 2.0 and the preset 2.4).
    const auto nativeFourK = replay::computeEffectiveBitrates(80, 120, 80, replay::kRateControlAdaptiveVbr,
        replay::resolutionBitrateMultiplier(3840, 2160), 1.0, replay::VideoCodecFamily::AV1);
    IR_SHARED_CHECK(nativeFourK.meanMbps == 134 && nativeFourK.maxMbps == 202);
    // Non-16:9: more pixels -> more bitrate, monotonic, clamped to [0.5, 2.5].
    const double ultrawide = replay::resolutionBitrateMultiplier(2560, 1080);
    IR_SHARED_CHECK(ultrawide > 1.0 && ultrawide < 1.6);
    IR_SHARED_CHECK(replay::resolutionBitrateMultiplier(3440, 1440) > replay::resolutionBitrateMultiplier(2560, 1440));
    IR_SHARED_CHECK(approxEq(replay::resolutionBitrateMultiplier(5120, 2880), 2.50));
    IR_SHARED_CHECK(approxEq(replay::resolutionBitrateMultiplier(640, 360), 0.50));
    double prev = 0.0;
    for (int h = 360; h <= 4320; h += 36) {
        const double m = replay::resolutionBitrateMultiplier((h * 16) / 9, h);
        IR_SHARED_CHECK(m + 1e-12 >= prev);
        prev = m;
    }
    // CBR stays exact regardless of size / FPS / codec.
    const auto cbr = replay::computeEffectiveBitrates(50, 60, 80, replay::kRateControlCbr,
        replay::resolutionBitrateMultiplier(3840, 2160), replay::fpsBitrateMultiplier(240), replay::VideoCodecFamily::H264);
    IR_SHARED_CHECK(cbr.meanMbps == 50 && cbr.maxMbps == 50);
    std::cout << "  PASS Test W9\n";
}

inline void testSegmentDurationRuleW14() {
    std::cout << "[Test W14] Segment duration = later of video and game-audio end...\n";
    const double frame60 = 1.0 / 60.0;
    // GD minimized: 5 s of video, 30 s of game audio -> 30 s segment (was 5 s).
    IR_SHARED_CHECK(std::abs(replay::segmentDurationSeconds(50000000LL - 166667LL, frame60, 30LL * 48000, 48000) - 30.0) < 1e-6);
    // Normal rotation: audio split exactly at S, video ends one frame later -> video end wins.
    const double normal = replay::segmentDurationSeconds(300000000LL - 1000LL, frame60, 30LL * 48000, 48000);
    IR_SHARED_CHECK(normal > 30.0 && normal < 30.0 + frame60 + 1e-6);
    // 44.1 kHz game audio is converted with its own rate.
    IR_SHARED_CHECK(std::abs(replay::segmentDurationSeconds(-1, frame60, 44100LL * 12, 44100) - 12.0) < 1e-9);
    // Keep rule: needs a finalized file with at least one frame; any track > 0.25 s keeps it.
    IR_SHARED_CHECK(replay::shouldKeepSegment(true, true, replay::segmentDurationSeconds(0, frame60, 48000, 48000)));
    IR_SHARED_CHECK(!replay::shouldKeepSegment(true, true, 0.2));
    IR_SHARED_CHECK(replay::shouldKeepSegment(true, false, 30.0, true));   // Windows: audio-only (minimized) segment kept
    IR_SHARED_CHECK(!replay::shouldKeepSegment(true, false, 30.0, false)); // macOS keeps requiring a video frame
    IR_SHARED_CHECK(!replay::shouldKeepSegment(true, false, 0.2, true));
    IR_SHARED_CHECK(!replay::shouldKeepSegment(false, true, 30.0));
    std::cout << "  PASS Test W14\n";
}

inline void testMicQueueDurationLimitW15() {
    std::cout << "[Test W15] Microphone worker queue is bounded by time, not packet count...\n";
    constexpr size_t kLimit = 48000 * 3; // 3 s
    auto frames = [](const std::vector<float>& c) { return c.size() / 2; };

    // 2 s of 5 ms packets (400 packets) fit; the old 150-packet limit dropped 250 of them (1.25 s).
    std::deque<std::vector<float>> q;
    uint64_t dropped = 0;
    for (int i = 0; i < 400; ++i) {
        dropped += replay::enforceAudioQueueFrameLimit(q, 240, kLimit, frames);
        q.emplace_back(240 * 2, 0.0f);
    }
    IR_SHARED_CHECK(dropped == 0 && q.size() == 400);

    // Keep pushing 10 ms packets: total never exceeds 3 s and drops are counted in frames.
    for (int i = 0; i < 200; ++i) {
        dropped += replay::enforceAudioQueueFrameLimit(q, 480, kLimit, frames);
        q.emplace_back(480 * 2, 0.0f);
    }
    size_t total = 0;
    for (const auto& c : q) total += frames(c);
    IR_SHARED_CHECK(total <= kLimit);
    IR_SHARED_CHECK(dropped == 96000 + 200 * 480 - kLimit);

    // A chunk larger than the limit is still accepted alone.
    std::deque<std::vector<float>> big;
    big.emplace_back(1000 * 2, 0.0f);
    IR_SHARED_CHECK(replay::enforceAudioQueueFrameLimit(big, kLimit + 10, kLimit, frames) == 1000);
    IR_SHARED_CHECK(big.empty());
    std::cout << "  PASS Test W15\n";
}

// T-K1: only an explicit privacy denial stops microphone capture; 3 s of digital silence is a soft status
// (TCC-block hint without a grant, "muted?" with a grant) that real audio clears.
inline void testMicStatusPolicyK1() {
    std::cout << "[Test K1] Microphone status policy: silence is soft, only a denial stops capture...\n";
    using replay::MicCaptureStatus;
    using replay::MicPrivacyAuthorization;

    // Granted (e.g. launched through Steam): silence means a muted / silent input, never "use Steam".
    IR_SHARED_CHECK(replay::micStatusForDigitalSilence(MicPrivacyAuthorization::Authorized, false) ==
                    MicCaptureStatus::DigitalSilenceDetected);
    IR_SHARED_CHECK(replay::micStatusForDigitalSilence(MicPrivacyAuthorization::Authorized, true) ==
                    MicCaptureStatus::DigitalSilenceDetected);
    // Undetermined grant and no usage description (GD launched outside Steam): TCC-block hint.
    IR_SHARED_CHECK(replay::micStatusForDigitalSilence(MicPrivacyAuthorization::NotDetermined, false) ==
                    MicCaptureStatus::BlockedNoUsageDescription);
    IR_SHARED_CHECK(replay::micStatusForDigitalSilence(MicPrivacyAuthorization::NotDetermined, true) ==
                    MicCaptureStatus::DigitalSilenceDetected);

    // Silence statuses are soft: they never stop capture and real audio clears them.
    for (auto s : {MicCaptureStatus::BlockedNoUsageDescription, MicCaptureStatus::DigitalSilenceDetected}) {
        IR_SHARED_CHECK(replay::isMicSilenceStatus(s));
        IR_SHARED_CHECK(!replay::micStatusStopsCapture(s));
    }
    for (auto s : {MicCaptureStatus::Idle, MicCaptureStatus::Active, MicCaptureStatus::UsingFallbackDevice,
                   MicCaptureStatus::NoInputDevice}) {
        IR_SHARED_CHECK(!replay::micStatusStopsCapture(s));
        IR_SHARED_CHECK(!replay::isMicSilenceStatus(s));
    }

    // Only an explicit denial (or a restriction) is final for the launch.
    IR_SHARED_CHECK(replay::micStatusStopsCapture(MicCaptureStatus::PermissionDenied));
    IR_SHARED_CHECK(replay::micAuthorizationBlocksCapture(MicPrivacyAuthorization::Denied));
    IR_SHARED_CHECK(replay::micAuthorizationBlocksCapture(MicPrivacyAuthorization::Restricted));
    IR_SHARED_CHECK(!replay::micAuthorizationBlocksCapture(MicPrivacyAuthorization::NotDetermined));
    IR_SHARED_CHECK(!replay::micAuthorizationBlocksCapture(MicPrivacyAuthorization::Authorized));

    // Poll-loop transitions: the capture callback's flags are sticky per queue (the 3 s silence alert is
    // never cleared), so after the mic wakes up the loop must report recovery once and then stay quiet
    // instead of flip-flopping silence <-> active every 50 ms poll.
    {
        using Event = replay::MicSilenceTracker::Event;
        replay::MicSilenceTracker tracker;
        int entered = 0, recovered = 0;
        auto step = [&](bool alert, bool nonZero) {
            const Event e = tracker.poll(alert, nonZero);
            if (e == Event::EnteredSilence) ++entered;
            if (e == Event::Recovered) ++recovered;
            return e;
        };
        for (int i = 0; i < 60; ++i) IR_SHARED_CHECK(step(false, false) == Event::None); // first 3 s
        IR_SHARED_CHECK(step(true, false) == Event::EnteredSilence);
        for (int i = 0; i < 100; ++i) IR_SHARED_CHECK(step(true, false) == Event::None);  // still muted
        IR_SHARED_CHECK(step(true, true) == Event::Recovered);                               // unmuted
        for (int i = 0; i < 1000; ++i) IR_SHARED_CHECK(step(true, true) == Event::None);  // no flip-flop
        IR_SHARED_CHECK(entered == 1 && recovered == 1);

        // Real audio before the first poll saw the alert: never reported as silent.
        replay::MicSilenceTracker quick;
        IR_SHARED_CHECK(quick.poll(true, true) == Event::None);
        IR_SHARED_CHECK(quick.poll(true, true) == Event::None);

        // A new queue (device switch) starts fresh and may report silence again.
        replay::MicSilenceTracker next;
        IR_SHARED_CHECK(next.poll(true, false) == Event::EnteredSilence);
    }
    std::cout << "  PASS Test K1\n";
}

// Runs every platform-independent test above.
inline void runSharedPureTests() {
    testConcatTimestampMathPure();
    testFrameBudgetAnd4KPool();
    testGameAudioRing();
    testMicMixFifo();
    testH264BoundsAndCodecSpecC3();
    testCodecFallbackBitratesW1();
    testResolutionBitrateSemanticsW9();
    testSegmentDurationRuleW14();
    testMicQueueDurationLimitW15();
    testMicStatusPolicyK1();
}

} // namespace replay_host_tests

#undef IR_SHARED_CHECK
