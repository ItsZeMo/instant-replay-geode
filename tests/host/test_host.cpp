#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "capture/FrameBudget.hpp"
#include "capture/GameAudioRing.hpp"
#include "config/BitrateMath.hpp"
#include "config/CodecUtils.hpp"
#include "encoder/ConcatTimestampMath.hpp"
#include "encoder/MfSegmentConcat.hpp"
#include "encoder/SegmentAudioLayout.hpp"
#include "recorder/MicMixFifo.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "delayimp.lib")

namespace {

template <typename T>
class LocalComRef {
public:
    LocalComRef() = default;
    ~LocalComRef() { reset(); }
    LocalComRef(const LocalComRef&) = delete;
    LocalComRef& operator=(const LocalComRef&) = delete;
    T* get() const noexcept { return m_ptr; }
    T** put() noexcept { reset(); return &m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }
    void reset() noexcept {
        if (m_ptr) { m_ptr->Release(); m_ptr = nullptr; }
    }
private:
    T* m_ptr = nullptr;
};

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL at line " << __LINE__ << ": " #expr "\n"; \
        std::exit(1); \
    } \
} while (0)

#define CHECK_HR(hr) do { \
    HRESULT _h = (hr); \
    if (FAILED(_h)) { \
        std::cerr << "FAIL HRESULT 0x" << std::hex << static_cast<unsigned>(_h) << std::dec << " at line " << __LINE__ << ": " #hr "\n"; \
        std::exit(1); \
    } \
} while (0)

void writeSyntheticSegment(
    const std::wstring& path,
    double videoDurationSec,
    double gameAudioDurationSec,
    double micAudioDurationSec, // <= 0 means single audio track
    UINT32 fps = 30
) {
    DeleteFileW(path.c_str());

    LocalComRef<IMFAttributes> attr;
    CHECK_HR(MFCreateAttributes(attr.put(), 2));
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);

    LocalComRef<IMFSinkWriter> writer;
    CHECK_HR(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr.get(), writer.put()));

    const UINT32 width = 128;
    const UINT32 height = 72;

    DWORD videoIdx = 0;
    {
        LocalComRef<IMFMediaType> vOut, vIn;
        CHECK_HR(MFCreateMediaType(vOut.put()));
        vOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vOut->SetUINT32(MF_MT_AVG_BITRATE, 500000);
        vOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(vOut.get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(vOut.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(vOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        CHECK_HR(writer->AddStream(vOut.get(), &videoIdx));

        CHECK_HR(MFCreateMediaType(vIn.put()));
        vIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        vIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(vIn.get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(vIn.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(vIn.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        CHECK_HR(writer->SetInputMediaType(videoIdx, vIn.get(), nullptr));
    }

    auto addAacStream = [&](DWORD& outIdx) {
        LocalComRef<IMFMediaType> aOut, aIn;
        CHECK_HR(MFCreateMediaType(aOut.put()));
        aOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
        CHECK_HR(writer->AddStream(aOut.get(), &outIdx));

        CHECK_HR(MFCreateMediaType(aIn.put()));
        aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        aIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        CHECK_HR(writer->SetInputMediaType(outIdx, aIn.get(), nullptr));
    };

    DWORD audio1Idx = 0;
    addAacStream(audio1Idx);
    DWORD audio2Idx = 0;
    bool hasMic = (micAudioDurationSec > 0.0);
    if (hasMic) {
        addAacStream(audio2Idx);
    }

    CHECK_HR(writer->BeginWriting());

    const int numFrames = static_cast<int>(std::llround(videoDurationSec * fps));
    const LONGLONG frameDurHns = 10000000LL / fps;
    const DWORD frameBytes = width * height * 4;
    for (int f = 0; f < numFrames; ++f) {
        LocalComRef<IMFMediaBuffer> buf;
        CHECK_HR(MFCreateMemoryBuffer(frameBytes, buf.put()));
        BYTE* p = nullptr;
        CHECK_HR(buf->Lock(&p, nullptr, nullptr));
        memset(p, static_cast<int>((f * 7) & 0xFF), frameBytes);
        buf->Unlock();
        buf->SetCurrentLength(frameBytes);

        LocalComRef<IMFSample> smp;
        CHECK_HR(MFCreateSample(smp.put()));
        smp->AddBuffer(buf.get());
        smp->SetSampleTime(static_cast<LONGLONG>(f) * frameDurHns);
        smp->SetSampleDuration(frameDurHns);
        CHECK_HR(writer->WriteSample(videoIdx, smp.get()));
    }

    auto writePcm = [&](DWORD streamIdx, double durSec) {
        const int64_t totalSamples = static_cast<int64_t>(std::llround(durSec * 48000.0));
        const int64_t chunkSamples = 4800; // 100ms
        for (int64_t off = 0; off < totalSamples; off += chunkSamples) {
            int64_t n = std::min(chunkSamples, totalSamples - off);
            DWORD bytes = static_cast<DWORD>(n * 4);
            LocalComRef<IMFMediaBuffer> buf;
            CHECK_HR(MFCreateMemoryBuffer(bytes, buf.put()));
            BYTE* p = nullptr;
            CHECK_HR(buf->Lock(&p, nullptr, nullptr));
            memset(p, 0, bytes);
            buf->Unlock();
            buf->SetCurrentLength(bytes);

            LocalComRef<IMFSample> smp;
            CHECK_HR(MFCreateSample(smp.put()));
            smp->AddBuffer(buf.get());
            smp->SetSampleTime((off * 10000000LL) / 48000LL);
            smp->SetSampleDuration((n * 10000000LL) / 48000LL);
            CHECK_HR(writer->WriteSample(streamIdx, smp.get()));
        }
    };

    writePcm(audio1Idx, gameAudioDurationSec);
    if (hasMic) {
        writePcm(audio2Idx, micAudioDurationSec);
    }
    CHECK_HR(writer->Finalize());
}

void remuxWithProductionConcatState(const std::vector<std::wstring>& segPaths, const std::wstring& outPath) {
    DeleteFileW(outPath.c_str());
    LocalComRef<IMFAttributes> readerAttr, writerAttr;
    CHECK_HR(MFCreateAttributes(readerAttr.put(), 1));
    readerAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    CHECK_HR(MFCreateAttributes(writerAttr.put(), 1));
    writerAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    LocalComRef<IMFSourceReader> firstReader;
    CHECK_HR(MFCreateSourceReaderFromURL(segPaths[0].c_str(), readerAttr.get(), firstReader.put()));
    LocalComRef<IMFSinkWriter> writer;
    CHECK_HR(MFCreateSinkWriterFromURL(outPath.c_str(), nullptr, writerAttr.get(), writer.put()));

    struct StreamMap { DWORD rIdx; DWORD wIdx; bool isVideo; };
    std::vector<StreamMap> streams;
    LONGLONG videoFrameDurHns = 0;
    for (DWORD s = 0; s < 16; ++s) {
        LocalComRef<IMFMediaType> type;
        HRESULT hr = firstReader->GetNativeMediaType(s, 0, type.put());
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        CHECK_HR(hr);
        GUID major = GUID_NULL;
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        const bool isVid = (major == MFMediaType_Video);
        if (isVid && videoFrameDurHns == 0) {
            UINT32 num = 0, den = 1;
            if (SUCCEEDED(MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0) {
                videoFrameDurHns = (10000000LL * den) / num;
            }
        }
        DWORD wIdx = 0;
        CHECK_HR(writer->AddStream(type.get(), &wIdx));
        CHECK_HR(writer->SetInputMediaType(wIdx, type.get(), nullptr));
        streams.push_back({s, wIdx, isVid});
    }
    CHECK(!streams.empty());
    CHECK_HR(writer->BeginWriting());

    replay::ConcatTimestampState state(0);
    for (const auto& path : segPaths) {
        LocalComRef<IMFSourceReader> reader;
        CHECK_HR(MFCreateSourceReaderFromURL(path.c_str(), readerAttr.get(), reader.put()));
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        std::map<DWORD, StreamMap> smap;
        std::set<DWORD> remaining;
        for (const auto& st : streams) {
            CHECK_HR(reader->SetStreamSelection(st.rIdx, TRUE));
            smap[st.rIdx] = st;
            remaining.insert(st.rIdx);
        }
        state.beginSegment();
        while (!remaining.empty()) {
            DWORD actual = 0, flags = 0;
            LONGLONG ts = 0;
            LocalComRef<IMFSample> smp;
            CHECK_HR(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &actual, &flags, &ts, smp.put()));
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) remaining.erase(actual);
            if (!smp) continue;
            auto it = smap.find(actual);
            if (it == smap.end()) continue;
            LONGLONG dur = 0;
            smp->GetSampleDuration(&dur);
            int64_t sampleTime = state.computeSampleTime(ts, 0);
            if (it->second.isVideo) {
                UINT64 dts = 0;
                if (SUCCEEDED(smp->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))) {
                    int64_t newDts = 0;
                    state.adjustVideoTimestamps(static_cast<int64_t>(dts), 0, sampleTime, newDts);
                    smp->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(newDts));
                }
            }
            smp->SetSampleTime(sampleTime);
            CHECK_HR(writer->WriteSample(it->second.wIdx, smp.get()));
            CHECK(state.recordWrittenSample(it->second.wIdx, it->second.isVideo, sampleTime, dur));
        }
        bool anomaly = false;
        state.endSegment(videoFrameDurHns, &anomaly);
    }
    CHECK_HR(writer->Finalize());
}

void verifyMp4NoAudioOverlapAndSynced(const std::wstring& mp4Path, int expectedAudioStreams, LONGLONG maxAllowedSeamGapHns = -1) {
    LocalComRef<IMFAttributes> attr;
    CHECK_HR(MFCreateAttributes(attr.put(), 1));
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    LocalComRef<IMFSourceReader> reader;
    CHECK_HR(MFCreateSourceReaderFromURL(mp4Path.c_str(), attr.get(), reader.put()));
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);

    std::set<DWORD> audioStreams;
    std::set<DWORD> remaining;
    for (DWORD s = 0; s < 16; ++s) {
        LocalComRef<IMFMediaType> t;
        HRESULT hr = reader->GetNativeMediaType(s, 0, t.put());
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        CHECK_HR(hr);
        GUID major = GUID_NULL;
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Audio) audioStreams.insert(s);
        CHECK_HR(reader->SetStreamSelection(s, TRUE));
        remaining.insert(s);
    }
    CHECK(static_cast<int>(audioStreams.size()) == expectedAudioStreams);

    std::map<DWORD, LONGLONG> prevPts;
    std::map<DWORD, LONGLONG> prevEnd;
    std::map<DWORD, LONGLONG> maxGap;
    std::map<DWORD, int> packetCounts;
    while (!remaining.empty()) {
        DWORD s = 0, flags = 0;
        LONGLONG ts = 0;
        LocalComRef<IMFSample> smp;
        CHECK_HR(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &s, &flags, &ts, smp.put()));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) remaining.erase(s);
        if (!smp) continue;
        if (audioStreams.count(s)) {
            LONGLONG dur = 0;
            smp->GetSampleDuration(&dur);
            if (prevEnd.count(s)) {
                CHECK(ts >= prevEnd[s]); // Strictly NO overlap (previous sample ends <= next starts)
                CHECK((ts - prevPts[s]) >= 100000LL); // Never crushed to 1000 HNS (0.1 ms)
                maxGap[s] = std::max(maxGap[s], ts - prevEnd[s]);
            }
            prevPts[s] = ts;
            prevEnd[s] = ts + dur;
            packetCounts[s]++;
        }
    }
    const DWORD primaryAudioStream = *audioStreams.begin();
    if (maxAllowedSeamGapHns >= 0) {
        CHECK(maxGap[primaryAudioStream] <= maxAllowedSeamGapHns);
    }
    for (DWORD s : audioStreams) {
        CHECK(packetCounts[s] > 40);
    }
}

void testConcatAndC1SeamMath() {
    std::cout << "[Test 1] MediaFoundation AAC concat, C1 seam Continuity & B-frame math...\n";
    auto tmp = std::filesystem::temp_directory_path() / L"ir_host_test_concat";
    std::filesystem::create_directories(tmp);

    // 1a. audioEnd (2.0s) > videoEnd + 50ms (1.0s) across 2 segments -> zero overlap
    auto s1 = (tmp / L"s1.mp4").wstring();
    auto s2 = (tmp / L"s2.mp4").wstring();
    auto out1 = (tmp / L"out_tail.mp4").wstring();
    writeSyntheticSegment(s1, 1.0, 2.0, 0.0);
    writeSyntheticSegment(s2, 1.0, 1.0, 0.0);
    remuxWithProductionConcatState({s1, s2}, out1);
    verifyMp4NoAudioOverlapAndSynced(out1, 1);

    // 1b. C1 real MF 60 FPS test: video extends (1/60 - 0.1ms) past audio S -> primary AAC gap < 1 ms across seam!
    auto c1 = (tmp / L"c1.mp4").wstring();
    auto c2 = (tmp / L"c2.mp4").wstring();
    auto outC1 = (tmp / L"out_c1_60fps.mp4").wstring();
    writeSyntheticSegment(c1, 1.0 + (1.0 / 60.0), 1.0, 0.0, 60);
    writeSyntheticSegment(c2, 1.0, 1.0, 0.0, 60);
    remuxWithProductionConcatState({c1, c2}, outC1);
    verifyMp4NoAudioOverlapAndSynced(outC1, 1, 10000LL /* < 1 ms */);

    // 1c. Dual audio tracks (game 1.5s, mic 1.8s > video 1.0s)
    auto d1 = (tmp / L"d1.mp4").wstring();
    auto d2 = (tmp / L"d2.mp4").wstring();
    auto out3 = (tmp / L"out_dual.mp4").wstring();
    writeSyntheticSegment(d1, 1.0, 1.5, 1.8);
    writeSyntheticSegment(d2, 1.0, 1.0, 1.0);
    remuxWithProductionConcatState({d1, d2}, out3);
    verifyMp4NoAudioOverlapAndSynced(out3, 2);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::cout << "  PASS Test 1\n";
}

void testConcatTimestampMathPure() {
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
        CHECK(st.recordWrittenSample(0, true, seg1VideoPts, kFrameDur));
        // Primary game audio (writerIndex=1) ends at exact S
        CHECK(st.recordWrittenSample(1, false, S - 213333, 213333));

        bool anomalyWarn = false;
        int64_t excess = 0;
        st.endSegment(kFrameDur, &anomalyWarn, &excess);
        CHECK(!anomalyWarn);
        CHECK(st.outputBase == S); // MUST anchor to primary audio end S, NOT S + kFrameDur - 1000!

        st.beginSegment();
        int64_t seg2AudioPts = st.computeSampleTime(0, 0);
        CHECK(seg2AudioPts == S); // 0 HNS gap between seg1 audio end and seg2 audio start!
        CHECK(st.recordWrittenSample(1, false, seg2AudioPts, 213333));

        int64_t seg2VideoPts = st.computeSampleTime(0, 0);
        int64_t seg2VideoDts = 0;
        st.adjustVideoTimestamps(0, 0, seg2VideoPts, seg2VideoDts);
        CHECK(seg2VideoPts > seg1VideoPts); // Strictly monotonic video PTS across seam
        CHECK(seg2VideoDts == S);
    }

    // 1e. C1 Anomaly fallback (> 1.5 frames longer video than primary audio):
    // Video exceeds primary audio by 2.0 frames -> falls back to segMaxEnd and signals anomalyWarn!
    {
        constexpr int64_t kFrameDur = 166667;
        constexpr int64_t S = 300000000;
        replay::ConcatTimestampState st(0);
        st.beginSegment();
        CHECK(st.recordWrittenSample(1, false, S - 213333, 213333)); // audio ends at S
        CHECK(st.recordWrittenSample(0, true, S + kFrameDur, kFrameDur)); // video ends at S + 2*kFrameDur

        bool anomalyWarn = false;
        int64_t excess = 0;
        st.endSegment(kFrameDur, &anomalyWarn, &excess);
        CHECK(anomalyWarn);
        CHECK(excess == 2 * kFrameDur);
        CHECK(st.outputBase == S + 2 * kFrameDur);
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
        CHECK(dts0 == 0 && pts0 == 333333);
        CHECK(st.recordWrittenSample(0, true, pts0, kFrameDur));

        int64_t pts1 = st.computeSampleTime(1666667, cutHns); int64_t dts1 = 0;
        st.adjustVideoTimestamps(1000000, cutHns, pts1, dts1);
        CHECK(dts1 == 333333 && pts1 == 1000000);
        CHECK(st.recordWrittenSample(0, true, pts1, kFrameDur));

        int64_t pts2 = st.computeSampleTime(1333333, cutHns); int64_t dts2 = 0;
        st.adjustVideoTimestamps(1333333, cutHns, pts2, dts2);
        CHECK(dts2 == 666666 && pts2 == 666666);
        CHECK(st.recordWrittenSample(0, true, pts2, kFrameDur));

        int64_t ap = st.computeSampleTime(2286667, cutHns);
        CHECK(st.recordWrittenSample(1, false, ap, 213333));
        bool anomalyWarn = false;
        st.endSegment(kFrameDur, &anomalyWarn);
        CHECK(!anomalyWarn);
        CHECK(st.outputBase == ap + 213333);

        st.beginSegment();
        int64_t s2a = st.computeSampleTime(0, 0);
        CHECK(s2a == ap + 213333);
        CHECK(st.recordWrittenSample(1, false, s2a, 213333));
        int64_t s2vPts = st.computeSampleTime(333333, 0); int64_t s2vDts = 0;
        st.adjustVideoTimestamps(0, 0, s2vPts, s2vDts);
        CHECK(s2vDts > dts2);
        CHECK(s2vPts - s2vDts == 333333);
    }

    // N4: 1-sample HNS tolerance absorbs independent 100 ns rounding of AAC packet PTS vs duration
    // (1024 / 48000 s = 213333.33 HNS) over 10,000 packets without hiding a real overlap.
    {
        CHECK(replay::ConcatTimestampState::audioOverlapToleranceHns(48000) == 209);
        CHECK(replay::ConcatTimestampState::audioOverlapToleranceHns(44100) == 227);
        replay::ConcatTimestampState st(0);
        st.beginSegment();
        const int64_t tol = replay::ConcatTimestampState::audioOverlapToleranceHns(48000);
        for (int64_t k = 0; k < 10000; ++k) {
            const int64_t pts = (k * 1024 * 10000000LL) / 48000;
            const int64_t dur = ((k + 1) * 1024 * 10000000LL) / 48000 - pts + 1; // end rounded up by 1 HNS
            CHECK(st.recordWrittenSample(1, false, pts, dur, nullptr, tol));
        }
        int64_t prevEnd = 0;
        CHECK(!st.recordWrittenSample(1, false, (9999 * 1024 * 10000000LL) / 48000, 213333, &prevEnd, tol));
        CHECK(prevEnd > 0);
    }

    // N4: macOS keeps negative B-frame DTS (allowNegativeDts) and unclamped PTS; Windows clamps both.
    {
        replay::ConcatTimestampState st(0);
        st.beginSegment();
        int64_t pts = 0;
        int64_t dts = 0;
        st.adjustVideoTimestamps(-333333, 0, pts, dts, false, true);
        CHECK(dts == -333333 && pts == 0);
        replay::ConcatTimestampState win(0);
        win.beginSegment();
        int64_t wpts = 0;
        int64_t wdts = 0;
        win.adjustVideoTimestamps(-333333, 0, wpts, wdts);
        CHECK(wdts == 0 && wpts == 0);
    }

    std::cout << "  PASS Test 1m\n";
}

void testFrameBudgetAnd4KPool() {
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
    CHECK(effP1080 == kPoolBaseBytes && effQ1080 == kQueueBaseBytes);
    auto [p1080c, p1080b] = simulateFill(sz1080, kPoolMaxFrames, effP1080, false);
    CHECK(p1080c == 6 && p1080b <= kPoolBaseBytes);
    auto [q1080c, q1080b] = simulateFill(sz1080, kQueueMaxFrames, effQ1080, true);
    CHECK(q1080c == 8 && q1080b <= kQueueBaseBytes);
    auto [f1080c, f1080b] = simulateInFlight(sz1080);
    CHECK(f1080c == 16 && f1080b <= kInFlightMaxBytes);

    const size_t effP1440 = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz1440, kPoolMinFrames, kPoolCeilingBytes);
    const size_t effQ1440 = replay::effectiveFrameByteBudget(kQueueBaseBytes, sz1440, kQueueMinFrames, kQueueCeilingBytes);
    CHECK(effP1440 == sz1440 * 4 && effQ1440 == kQueueBaseBytes);
    auto [p1440c, p1440b] = simulateFill(sz1440, kPoolMaxFrames, effP1440, false);
    CHECK(p1440c == 4 && p1440b == sz1440 * 4);
    auto [q1440c, q1440b] = simulateFill(sz1440, kQueueMaxFrames, effQ1440, true);
    CHECK(q1440c == 4 && q1440b <= kQueueBaseBytes);
    auto [f1440c, f1440b] = simulateInFlight(sz1440);
    CHECK(f1440c == 16 && f1440b <= kInFlightMaxBytes);

    const size_t effP4k = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz4k, kPoolMinFrames, kPoolCeilingBytes);
    const size_t effQ4k = replay::effectiveFrameByteBudget(kQueueBaseBytes, sz4k, kQueueMinFrames, kQueueCeilingBytes);
    CHECK(effP4k == sz4k * 4 && effQ4k == sz4k * 4);
    auto [p4kc, p4kb] = simulateFill(sz4k, kPoolMaxFrames, effP4k, false);
    CHECK(p4kc == 4 && p4kb == sz4k * 4);
    auto [q4kc, q4kb] = simulateFill(sz4k, kQueueMaxFrames, effQ4k, true);
    CHECK(q4kc == 4 && q4kb == sz4k * 4);
    auto [f4kc, f4kb] = simulateInFlight(sz4k);
    CHECK(f4kc == 8 && f4kb == sz4k * 8 && f4kb <= kInFlightMaxBytes);

    const size_t effP8k = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz8k, kPoolMinFrames, kPoolCeilingBytes);
    CHECK(effP8k == kPoolCeilingBytes);
    auto [p8kc, p8kb] = simulateFill(sz8k, kPoolMaxFrames, effP8k, false);
    CHECK(p8kc == 2 && p8kb <= kPoolCeilingBytes);
    auto [f8kc, f8kb] = simulateInFlight(sz8k);
    CHECK(f8kc == 4);

    const size_t effP16k = replay::effectiveFrameByteBudget(kPoolBaseBytes, sz16k, kPoolMinFrames, kPoolCeilingBytes);
    const size_t effQ16k = replay::effectiveFrameByteBudget(kQueueBaseBytes, sz16k, kQueueMinFrames, kQueueCeilingBytes);
    CHECK(effP16k == kPoolCeilingBytes && effQ16k == kQueueCeilingBytes);
    auto [p16kc, p16kb] = simulateFill(sz16k, kPoolMaxFrames, effP16k, false);
    CHECK(p16kc == 0 && p16kb == 0);
    auto [q16kc, q16kb] = simulateFill(sz16k, kQueueMaxFrames, effQ16k, true);
    CHECK(q16kc == 1 && q16kb == sz16k);

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
    CHECK(runPoolSim(kPoolBaseBytes) > 100);
    CHECK(runPoolSim(effP4k) == 0);

    std::cout << "  PASS Test 7\n";
}

void testGameAudioRing() {
    std::cout << "[Test 8] SPSC GameAudioRing deterministic interleaving & overflow...\n";
    auto ring = std::make_unique<replay::GameAudioRing>();

    std::vector<float> pkt1(512 * 2, 0.25f);
    std::vector<float> pkt2(512 * 2, 0.75f);
    const int64_t t1EndNs = 10'000'000'000LL;
    const int64_t t2EndNs = 15'000'000'000LL;
    const int64_t pktDurNs = (512LL * 1000000000LL) / 48000LL;

    CHECK(ring->push(pkt1.data(), 512, 2, 1.0f, 48000, t1EndNs));

    std::vector<float> out;
    int outRate = 0;
    std::chrono::steady_clock::time_point outTs{};
    size_t drained1 = ring->drain(out, outRate, outTs, [&]() {
        CHECK(ring->push(pkt2.data(), 512, 2, 1.0f, 48000, t2EndNs));
    });
    CHECK(drained1 == 512);
    CHECK(outRate == 48000);
    CHECK(std::abs(out[0] - 0.25f) < 1e-4f);
    CHECK(outTs.time_since_epoch().count() == (t1EndNs - pktDurNs));

    size_t drained2 = ring->drain(out, outRate, outTs);
    CHECK(drained2 == 512);
    CHECK(std::abs(out[0] - 0.75f) < 1e-4f);
    CHECK(outTs.time_since_epoch().count() == (t2EndNs - pktDurNs));

    ring->reset();
    std::vector<float> big(1024 * 2, 0.5f);
    int pushed = 0;
    for (int i = 0; i < 300; ++i) {
        if (ring->push(big.data(), 1024, 2, 1.0f, 48000, 1'000'000'000LL + i * 21333333LL)) {
            pushed++;
        }
    }
    CHECK(pushed == 256);
    CHECK(!ring->push(big.data(), 1024, 2, 1.0f, 48000, 99'000'000'000LL));
    size_t allFrames = ring->drain(out, outRate, outTs);
    CHECK(allFrames == replay::GameAudioRing::kRingFrames);
    CHECK(ring->push(pkt1.data(), 512, 2, 1.0f, 48000, 50'000'000'000LL));

    ring->reset();
    float oneFrame[2] = {0.1f, -0.1f};
    int metaPushed = 0;
    for (size_t i = 0; i < replay::GameAudioRing::kMetaCapacity + 100; ++i) {
        if (ring->push(oneFrame, 1, 2, 1.0f, 48000, 2'000'000'000LL + static_cast<int64_t>(i) * 1000000LL)) {
            metaPushed++;
        }
    }
    CHECK(metaPushed == static_cast<int>(replay::GameAudioRing::kMetaCapacity));
    size_t metaFrames = ring->drain(out, outRate, outTs);
    CHECK(metaFrames == replay::GameAudioRing::kMetaCapacity);
    CHECK(ring->push(pkt2.data(), 512, 2, 1.0f, 48000, 60'000'000'000LL));

    std::cout << "  PASS Test 8\n";
}

void testMicMixFifo() {
    std::cout << "[Test 9] Time-aligned MicMixFifo (drift, pause, stale discard, overlap, rebase)...\n";
    constexpr int kRate = 48000;
    constexpr size_t kMaxBuf = kRate * 5;

    {
        replay::MicMixFifo fifo;
        std::vector<float> mic(960 * 2, 0.25f);
        fifo.appendResampled(mic.data(), 960, kRate, 7200, kMaxBuf);

        std::vector<float> game(4800 * 2, 0.10f);
        CHECK(fifo.mixInto(game.data(), 4800, 0) == 0);
        CHECK(std::abs(game[0] - 0.10f) < 1e-5f);
        CHECK(fifo.availFrames() == 960);

        std::vector<float> game2(4800 * 2, 0.10f);
        CHECK(fifo.mixInto(game2.data(), 4800, 4800) == 960);
        CHECK(std::abs(game2[0] - 0.10f) < 1e-5f);
        CHECK(std::abs(game2[2399 * 2] - 0.10f) < 1e-5f);
        CHECK(std::abs(game2[2400 * 2] - 0.35f) < 1e-5f);
        CHECK(std::abs(game2[3359 * 2] - 0.35f) < 1e-5f);
        CHECK(std::abs(game2[3360 * 2] - 0.10f) < 1e-5f);
        CHECK(fifo.availFrames() == 0);
    }

    {
        replay::MicMixFifo fifo;
        std::vector<float> oldMic(4800 * 2, 0.40f);
        fifo.appendResampled(oldMic.data(), 4800, kRate, 0, kMaxBuf);
        std::vector<float> recentMic(4800 * 2, 0.20f);
        fifo.appendResampled(recentMic.data(), 4800, kRate, 6000, kMaxBuf);
        std::vector<float> game(4800 * 2, 0.10f);
        size_t mixed = fifo.mixInto(game.data(), 4800, 9600);
        CHECK(mixed == 1920);
        CHECK(std::abs(game[0] - 0.30f) < 1e-5f);
        CHECK(fifo.availFrames() == 0);

        std::vector<float> veryOld(2400 * 2, 0.50f);
        fifo.appendResampled(veryOld.data(), 2400, kRate, 0, kMaxBuf);
        std::vector<float> gameLate(4800 * 2, 0.10f);
        CHECK(fifo.mixInto(gameLate.data(), 4800, 9600) == 0);
        CHECK(std::abs(gameLate[0] - 0.10f) < 1e-5f);
        CHECK(fifo.availFrames() == 0);
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
        CHECK(maxAbsSkew <= 1920 + 960);
    }

    {
        replay::MicMixFifo fifo;
        std::vector<float> mic(960 * 2, 0.25f);
        for (int c = 0; c < 50; ++c) {
            fifo.appendResampled(mic.data(), 960, kRate, c * 960, kMaxBuf);
            std::vector<float> g(960 * 2, 0.1f);
            CHECK(fifo.mixInto(g.data(), 960, c * 960) > 0);
        }
        fifo.appendResampled(mic.data(), 960, kRate, 144000, kMaxBuf);
        std::vector<float> gResume(960 * 2, 0.1f);
        size_t mixed = fifo.mixInto(gResume.data(), 960, 144000);
        CHECK(mixed > 0);
        CHECK(std::abs(gResume[mixed * 2 - 2] - 0.35f) < 1e-5f);
    }

    {
        replay::MicMixFifo fifo;
        auto seg0 = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
        auto seg1 = seg0 + std::chrono::seconds(30);

        std::vector<float> mic(2400 * 2, 0.30f);
        fifo.appendResampled(mic.data(), 2400, kRate, 1440000 + 2400, kMaxBuf);
        CHECK(fifo.headSample == 1442400);

        fifo.rebaseSegmentOrigin(seg0, seg1);
        CHECK(fifo.headSample == 2400);

        fifo.advanceOnGameOverlap(3600, 1200);
        CHECK(fifo.headSample == 3600);
        CHECK(fifo.availFrames() == 1200);

        std::vector<float> game(2400 * 2, 0.10f);
        CHECK(fifo.mixInto(game.data(), 2400, 3600) == 1200);
        CHECK(std::abs(game[0] - 0.40f) < 1e-5f);
    }

    std::cout << "  PASS Test 9\n";
}

void testH264BoundsAndCodecSpecC3() {
    std::cout << "[Test 10] H.264 4096x2304 ultrawide clamping & shared custom codec parser...\n";

    // C3 acceptance cases: 21:9 2160p -> 4096x1714, 32:9 2160p -> 4096x1152
    {
        auto r1 = replay::clampDimensionsToBounds(5160, 2160);
        CHECK(r1.clamped);
        CHECK(r1.width == 4096 && r1.height == 1714);
        CHECK((r1.width % 2) == 0 && (r1.height % 2) == 0);

        auto r2 = replay::clampDimensionsToBounds(7680, 2160);
        CHECK(r2.clamped);
        CHECK(r2.width == 4096 && r2.height == 1152);
        CHECK((r2.width % 2) == 0 && (r2.height % 2) == 0);

        // 32:9 1440p native (5120x1440 -> 4096x1152)
        auto r3 = replay::clampDimensionsToBounds(5120, 1440);
        CHECK(r3.clamped);
        CHECK(r3.width == 4096 && r3.height == 1152);

        // Height > 2304 clamping (3440x2400 -> 3302x2304)
        auto r4 = replay::clampDimensionsToBounds(3440, 2400);
        CHECK(r4.clamped);
        CHECK(r4.width == 3302 && r4.height == 2304);
        CHECK((r4.width % 2) == 0 && (r4.height % 2) == 0);

        // Standard 16:9 4K stays untouched
        auto r5 = replay::clampDimensionsToBounds(3840, 2160);
        CHECK(!r5.clamped);
        CHECK(r5.width == 3840 && r5.height == 2160);
    }

    // Shared custom codec spec parser
    {
        auto av1 = replay::resolveRequestedCodecSpec(0, true, "");
        CHECK(av1.family == replay::VideoCodecFamily::AV1 && av1.hardware);

        auto hevc = replay::resolveRequestedCodecSpec(1, false, "");
        CHECK(hevc.family == replay::VideoCodecFamily::HEVC && !hevc.hardware);

        auto h264 = replay::resolveRequestedCodecSpec(2, true, "");
        CHECK(h264.family == replay::VideoCodecFamily::H264 && h264.hardware);

        auto custAv1 = replay::resolveRequestedCodecSpec(3, true, "av1_nvenc");
        CHECK(custAv1.family == replay::VideoCodecFamily::AV1 && custAv1.hardware);

        auto custHevc = replay::resolveRequestedCodecSpec(3, true, "HEVC_AMF");
        CHECK(custHevc.family == replay::VideoCodecFamily::HEVC && custHevc.hardware);

        auto custSw264 = replay::resolveRequestedCodecSpec(3, true, "software_h264");
        CHECK(custSw264.family == replay::VideoCodecFamily::H264 && !custSw264.hardware);

        auto custEmpty = replay::resolveRequestedCodecSpec(3, true, "");
        CHECK(custEmpty.family == replay::VideoCodecFamily::H264 && custEmpty.hardware);
    }

    std::cout << "  PASS Test 10\n";
}


void testCodecFallbackBitratesW1() {
    std::cout << "[Test W1] Per-family session bitrates for codec fallbacks & CUSTOM multipliers...\n";
    using replay::VideoCodecFamily;
    const int vbr = replay::kRateControlAdaptiveVbr;

    // Dev PC settings: base 80 / max 120 Mbps, 2160p preset (x2.4), 60 FPS, Adaptive VBR.
    const double fps60 = replay::fpsBitrateMultiplier(60);
    const auto table = replay::computeCodecBitrateTable(80, 120, 80, vbr, 2.4, fps60);
    auto explicitSelection = [&](VideoCodecFamily f) {
        return replay::computeEffectiveBitrates(80, 120, 80, vbr, 2.4, fps60, f);
    };
    CHECK(table.valid);
    CHECK(table.forFamily(VideoCodecFamily::AV1).meanMbps == 134);
    CHECK(table.forFamily(VideoCodecFamily::AV1).maxMbps == 202);

    // AV1 -> HEVC and AV1 -> H.264 fallbacks get exactly what an explicit HEVC / H.264 selection gets.
    const auto hevc = table.forFamily(VideoCodecFamily::HEVC);
    const auto h264 = table.forFamily(VideoCodecFamily::H264);
    CHECK(hevc.meanMbps == explicitSelection(VideoCodecFamily::HEVC).meanMbps && hevc.meanMbps == 163);
    CHECK(hevc.maxMbps == explicitSelection(VideoCodecFamily::HEVC).maxMbps && hevc.maxMbps == 245);
    CHECK(h264.meanMbps == explicitSelection(VideoCodecFamily::H264).meanMbps && h264.meanMbps == 211);
    CHECK(h264.maxMbps == 300); // 316.8 clamped to the 300 Mbps ceiling

    // Exhaustive: every base/res/fps/rc combination keeps the table identical to direct per-family math.
    for (int base : {5, 12, 35, 50, 80, 133, 250}) {
        for (double res : {0.5, 1.0, 1.6, 2.4}) {
            for (int fps : {30, 60, 120, 144, 240}) {
                for (int rc : {replay::kRateControlAdaptiveVbr, replay::kRateControlCbr, replay::kRateControlQuality}) {
                    const auto t = replay::computeCodecBitrateTable(base, base * 2, 65, rc, res, replay::fpsBitrateMultiplier(fps));
                    for (auto fam : {VideoCodecFamily::H264, VideoCodecFamily::HEVC, VideoCodecFamily::AV1}) {
                        const auto d = replay::computeEffectiveBitrates(base, base * 2, 65, rc, res, replay::fpsBitrateMultiplier(fps), fam);
                        CHECK(t.forFamily(fam).meanMbps == d.meanMbps && t.forFamily(fam).maxMbps == d.maxMbps);
                        CHECK(d.maxMbps >= d.meanMbps);
                    }
                }
            }
        }
    }

    // CBR is an explicit target: never scaled per codec.
    const auto cbr = replay::computeCodecBitrateTable(80, 120, 80, replay::kRateControlCbr, 2.4, fps60);
    CHECK(cbr.forFamily(VideoCodecFamily::AV1).meanMbps == 80 && cbr.forFamily(VideoCodecFamily::H264).meanMbps == 80);

    // CUSTOM strings use the parsed family's multiplier instead of a flat x1.00.
    auto customMult = [](const char* s) {
        return replay::codecBitrateMultiplier(replay::resolveRequestedCodecSpec(3, true, s).family);
    };
    CHECK(std::abs(customMult("av1_nvenc") - 0.70) < 1e-9);
    CHECK(std::abs(customMult("software_h264") - 1.10) < 1e-9);
    CHECK(std::abs(customMult("hevc_amf") - 0.85) < 1e-9);
    // Preset H.264 and custom "h264" resolve to the same family -> same effective bitrate.
    CHECK(replay::resolveRequestedCodecSpec(3, true, "h264").family == replay::resolveRequestedCodecSpec(2, true, "").family);

    std::cout << "  PASS Test W1\n";
}

// Writes one segment the way MediaFoundationEncoder does since W2: H.264 video plus AAC tracks that
// start with kSegmentAudioPrerollHns of silence, with the real audio shifted by the pre-roll.
void writePrerollSegment(
    const std::wstring& path,
    double videoDurationSec,
    const std::vector<std::vector<int16_t>>& tracks, // interleaved stereo int16 per audio track
    UINT32 fps = 30
) {
    DeleteFileW(path.c_str());
    LocalComRef<IMFAttributes> attr;
    CHECK_HR(MFCreateAttributes(attr.put(), 2));
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    LocalComRef<IMFSinkWriter> writer;
    CHECK_HR(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr.get(), writer.put()));

    const UINT32 width = 128, height = 72;
    DWORD videoIdx = 0;
    {
        LocalComRef<IMFMediaType> vOut, vIn;
        CHECK_HR(MFCreateMediaType(vOut.put()));
        vOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vOut->SetUINT32(MF_MT_AVG_BITRATE, 500000);
        vOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(vOut.get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(vOut.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(vOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        CHECK_HR(writer->AddStream(vOut.get(), &videoIdx));
        CHECK_HR(MFCreateMediaType(vIn.put()));
        vIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        vIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(vIn.get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(vIn.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(vIn.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        CHECK_HR(writer->SetInputMediaType(videoIdx, vIn.get(), nullptr));
    }
    std::vector<DWORD> audioIdx(tracks.size(), 0);
    for (size_t t = 0; t < tracks.size(); ++t) {
        LocalComRef<IMFMediaType> aOut, aIn;
        CHECK_HR(MFCreateMediaType(aOut.put()));
        aOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, t == 0 ? 24000 : 16000);
        CHECK_HR(writer->AddStream(aOut.get(), &audioIdx[t]));
        CHECK_HR(MFCreateMediaType(aIn.put()));
        aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        aIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        CHECK_HR(writer->SetInputMediaType(audioIdx[t], aIn.get(), nullptr));
    }
    CHECK_HR(writer->BeginWriting());

    auto writeAudio = [&](size_t t, const int16_t* data, int64_t frames, int64_t startFrame) {
        const DWORD bytes = static_cast<DWORD>(frames * 4);
        LocalComRef<IMFMediaBuffer> buf;
        CHECK_HR(MFCreateMemoryBuffer(bytes, buf.put()));
        BYTE* p = nullptr;
        CHECK_HR(buf->Lock(&p, nullptr, nullptr));
        if (data) memcpy(p, data, bytes); else memset(p, 0, bytes);
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        LocalComRef<IMFSample> smp;
        CHECK_HR(MFCreateSample(smp.put()));
        smp->AddBuffer(buf.get());
        smp->SetSampleTime((startFrame * 10000000LL) / 48000LL);
        smp->SetSampleDuration((frames * 10000000LL) / 48000LL);
        CHECK_HR(writer->WriteSample(audioIdx[t], smp.get()));
    };

    const int64_t preroll = replay::segmentAudioPrerollFrames(48000);
    std::vector<int64_t> written(tracks.size(), 0);
    for (size_t t = 0; t < tracks.size(); ++t) writeAudio(t, nullptr, preroll, 0);

    const int numFrames = static_cast<int>(std::llround(videoDurationSec * fps));
    const LONGLONG frameDurHns = 10000000LL / fps;
    const DWORD frameBytes = width * height * 4;
    for (int f = 0; f < numFrames; ++f) {
        LocalComRef<IMFMediaBuffer> buf;
        CHECK_HR(MFCreateMemoryBuffer(frameBytes, buf.put()));
        BYTE* p = nullptr;
        CHECK_HR(buf->Lock(&p, nullptr, nullptr));
        memset(p, static_cast<int>((f * 7) & 0xFF), frameBytes);
        buf->Unlock();
        buf->SetCurrentLength(frameBytes);
        LocalComRef<IMFSample> smp;
        CHECK_HR(MFCreateSample(smp.put()));
        smp->AddBuffer(buf.get());
        smp->SetSampleTime(static_cast<LONGLONG>(f) * frameDurHns);
        smp->SetSampleDuration(frameDurHns);
        CHECK_HR(writer->WriteSample(videoIdx, smp.get()));
        // Interleave ~10 ms audio packets up to the end of this frame, like WASAPI/FMOD delivery.
        const int64_t target = ((f + 1) * frameDurHns * 48000LL) / 10000000LL;
        for (size_t t = 0; t < tracks.size(); ++t) {
            const int64_t total = static_cast<int64_t>(tracks[t].size() / 2);
            while (written[t] < std::min(target, total)) {
                const int64_t n = std::min<int64_t>(480, std::min(target, total) - written[t]);
                writeAudio(t, tracks[t].data() + written[t] * 2, n, preroll + written[t]);
                written[t] += n;
            }
        }
    }
    for (size_t t = 0; t < tracks.size(); ++t) {
        const int64_t total = static_cast<int64_t>(tracks[t].size() / 2);
        while (written[t] < total) {
            const int64_t n = std::min<int64_t>(480, total - written[t]);
            writeAudio(t, tracks[t].data() + written[t] * 2, n, preroll + written[t]);
            written[t] += n;
        }
    }
    CHECK_HR(writer->Finalize());
}

// Decodes the N-th audio track to the left channel (float), placed by sample timestamps.
std::vector<float> decodeAudioTrack(const std::wstring& path, int ordinal) {
    LocalComRef<IMFSourceReader> reader;
    CHECK_HR(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put()));
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    DWORD stream = MAXDWORD;
    int seen = 0;
    for (DWORD s = 0; s < 16; ++s) {
        LocalComRef<IMFMediaType> t;
        HRESULT hr = reader->GetNativeMediaType(s, 0, t.put());
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        CHECK_HR(hr);
        GUID major = GUID_NULL;
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Audio && seen++ == ordinal) { stream = s; break; }
    }
    CHECK(stream != MAXDWORD);
    CHECK_HR(reader->SetStreamSelection(stream, TRUE));
    LocalComRef<IMFMediaType> pcm;
    CHECK_HR(MFCreateMediaType(pcm.put()));
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    CHECK_HR(reader->SetCurrentMediaType(stream, nullptr, pcm.get()));
    std::vector<float> left;
    while (true) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        LocalComRef<IMFSample> smp;
        CHECK_HR(reader->ReadSample(stream, 0, nullptr, &flags, &ts, smp.put()));
        if (smp) {
            LocalComRef<IMFMediaBuffer> buf;
            CHECK_HR(smp->ConvertToContiguousBuffer(buf.put()));
            BYTE* p = nullptr;
            DWORD len = 0;
            CHECK_HR(buf->Lock(&p, nullptr, &len));
            const float* f = reinterpret_cast<const float*>(p);
            const size_t n = len / 8;
            const size_t pos = static_cast<size_t>(std::llround(static_cast<double>(ts) * 48000.0 / 1e7));
            if (left.size() < pos + n) left.resize(pos + n, 0.0f);
            for (size_t k = 0; k < n; ++k) left[pos + k] = f[k * 2];
            buf->Unlock();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    return left;
}

std::vector<int16_t> sineSegment(int64_t firstFrame, int64_t frames, double hz, double amp) {
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * 2);
    for (int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(firstFrame + i) / 48000.0;
        const auto v = static_cast<int16_t>(std::lround(std::sin(2.0 * 3.14159265358979323846 * hz * t) * amp));
        pcm[static_cast<size_t>(i) * 2] = v;
        pcm[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return pcm;
}

// Every 5 ms window from `fromSec` to the end minus 25 ms stays within +-1 dB of the sine's RMS.
int countSeamDropouts(const std::vector<float>& left, double amp, double fromSec, double* outMinDb) {
    const size_t win = 240;
    const double nominal = amp / 32768.0 / std::sqrt(2.0);
    const double lo = nominal * std::pow(10.0, -1.0 / 20.0);
    const double hi = nominal * std::pow(10.0, 1.0 / 20.0);
    int bad = 0;
    double minDb = 0.0;
    const size_t start = static_cast<size_t>(fromSec * 48000.0);
    for (size_t w0 = start; w0 + win + 1200 <= left.size(); w0 += win) {
        double acc = 0.0;
        for (size_t k = 0; k < win; ++k) acc += static_cast<double>(left[w0 + k]) * left[w0 + k];
        const double rms = std::sqrt(acc / win);
        minDb = std::min(minDb, 20.0 * std::log10(std::max(rms, 1e-9) / nominal));
        if (rms < lo || rms > hi) ++bad;
    }
    if (outMinDb) *outMinDb = minDb;
    return bad;
}

void testExportAudioSeamsW2() {
    std::cout << "[Test W2] Export re-encodes continuous AAC: no dropouts at segment seams...\n";
    auto tmp = std::filesystem::temp_directory_path() / L"ir_host_test_w2";
    std::filesystem::create_directories(tmp);

    constexpr int64_t kSegFrames = 96000; // 2.0 s per segment
    constexpr double kAmp = 16000.0;
    std::vector<replay::mfconcat::SegmentInput> inputs;
    for (int seg = 0; seg < 3; ++seg) {
        auto path = (tmp / (L"w2_seg" + std::to_wstring(seg) + L".mp4")).wstring();
        writePrerollSegment(path, 2.0, {
            sineSegment(seg * kSegFrames, kSegFrames, 441.0, kAmp),
            sineSegment(seg * kSegFrames, kSegFrames, 300.0, kAmp)
        });
        replay::mfconcat::SegmentInput in;
        in.path = path;
        in.durationSeconds = 2.0;
        in.audioFrames[0] = kSegFrames;
        in.audioFrames[1] = kSegFrames;
        inputs.push_back(in);
    }

    replay::mfconcat::Options opt;
    opt.audioPrerollHns = replay::kSegmentAudioPrerollHns;
    std::atomic<int> progress{-1};
    opt.progressPermille = &progress;
    int warnings = 0;
    opt.logWarn = [&warnings](const std::string& msg) { ++warnings; std::cout << "  warn: " << msg << "\n"; };

    auto out = (tmp / L"w2_out.mp4.part").wstring(); // exporter writes a temporary *.mp4.part (W3)
    auto res = replay::mfconcat::concatenate(inputs, out, 0.0, opt);
    if (!res.success) std::cerr << "  concat error: " << res.error << "\n";
    CHECK(res.success);
    CHECK(std::abs(res.durationSeconds - 6.0) < 0.05);
    CHECK(progress.load() == 1000);
    CHECK(warnings == 0);

    for (int track = 0; track < 2; ++track) {
        auto left = decodeAudioTrack(out, track);
        CHECK(left.size() >= static_cast<size_t>(3 * kSegFrames - 2048));
        double minDb = 0.0;
        // Skip the first 20 ms: the output's own AAC stream start (single, unavoidable).
        const int bad = countSeamDropouts(left, kAmp, 0.020, &minDb);
        std::cout << "  track " << track << ": windows outside +-1 dB = " << bad << " (min " << minDb << " dB)\n";
        CHECK(bad == 0);
    }

    // Trimmed export keeps the stitched audio continuous as well.
    auto outTrim = (tmp / L"w2_trim.mp4").wstring();
    auto resTrim = replay::mfconcat::concatenate(inputs, outTrim, 0.7, opt);
    CHECK(resTrim.success);
    CHECK(resTrim.durationSeconds > 4.0 && resTrim.durationSeconds <= 6.0);
    auto leftTrim = decodeAudioTrack(outTrim, 0);
    CHECK(countSeamDropouts(leftTrim, kAmp, 0.020, nullptr) == 0);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::cout << "  PASS Test W2\n";
}

void testResolutionBitrateSemanticsW9() {
    std::cout << "[Test W9] Bitrate scales with the encoded size (Native 4K == 2160p preset)...\n";
    auto approxEq = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    // 16:9 sizes reproduce the preset table.
    CHECK(approxEq(replay::resolutionBitrateMultiplier(1280, 720), 0.50));
    CHECK(approxEq(replay::resolutionBitrateMultiplier(1920, 1080), 1.00));
    CHECK(approxEq(replay::resolutionBitrateMultiplier(2560, 1440), 1.60));
    CHECK(approxEq(replay::resolutionBitrateMultiplier(3840, 2160), 2.40));
    // Native 3840x2160 and the 2160p preset both encode 3840x2160 -> identical effective bitrate
    // (previously Native used h/1080 = 2.0 and the preset 2.4).
    const auto nativeFourK = replay::computeEffectiveBitrates(80, 120, 80, replay::kRateControlAdaptiveVbr,
        replay::resolutionBitrateMultiplier(3840, 2160), 1.0, replay::VideoCodecFamily::AV1);
    CHECK(nativeFourK.meanMbps == 134 && nativeFourK.maxMbps == 202);
    // Non-16:9: more pixels -> more bitrate, monotonic, clamped to [0.5, 2.5].
    const double ultrawide = replay::resolutionBitrateMultiplier(2560, 1080);
    CHECK(ultrawide > 1.0 && ultrawide < 1.6);
    CHECK(replay::resolutionBitrateMultiplier(3440, 1440) > replay::resolutionBitrateMultiplier(2560, 1440));
    CHECK(approxEq(replay::resolutionBitrateMultiplier(5120, 2880), 2.50));
    CHECK(approxEq(replay::resolutionBitrateMultiplier(640, 360), 0.50));
    double prev = 0.0;
    for (int h = 360; h <= 4320; h += 36) {
        const double m = replay::resolutionBitrateMultiplier((h * 16) / 9, h);
        CHECK(m + 1e-12 >= prev);
        prev = m;
    }
    // CBR stays exact regardless of size / FPS / codec.
    const auto cbr = replay::computeEffectiveBitrates(50, 60, 80, replay::kRateControlCbr,
        replay::resolutionBitrateMultiplier(3840, 2160), replay::fpsBitrateMultiplier(240), replay::VideoCodecFamily::H264);
    CHECK(cbr.meanMbps == 50 && cbr.maxMbps == 50);
    std::cout << "  PASS Test W9\n";
}

// Media Foundation may be missing on CI images (Windows Server without the Media Foundation
// feature): mfplat / mfreadwrite are delay-loaded and the MF integration tests are skipped then.
bool mediaFoundationTestsAvailable(std::string& whyNot) {
    if (!LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) ||
        !LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        whyNot = "Media Foundation DLLs are not installed";
        return false;
    }
    if (FAILED(MFStartup(MF_VERSION))) {
        whyNot = "MFStartup failed";
        return false;
    }
    auto hasMft = [](const GUID& category, const GUID& major, const GUID& subtype, bool isInput) {
        MFT_REGISTER_TYPE_INFO info{major, subtype};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                               isInput ? &info : nullptr, isInput ? nullptr : &info, &acts, &count);
        if (acts) {
            for (UINT32 i = 0; i < count; ++i) if (acts[i]) acts[i]->Release();
            CoTaskMemFree(acts);
        }
        return SUCCEEDED(hr) && count > 0;
    };
    if (!hasMft(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, MFAudioFormat_AAC, false)) {
        whyNot = "no Media Foundation AAC encoder";
    } else if (!hasMft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio, MFAudioFormat_AAC, true)) {
        whyNot = "no Media Foundation AAC decoder";
    } else if (!hasMft(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, MFVideoFormat_H264, false)) {
        whyNot = "no Media Foundation H.264 encoder";
    } else {
        return true;
    }
    MFShutdown();
    return false;
}

} // namespace

int main() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CHECK_HR(coHr);

    // Pure tests: always run (also on CI runners without Media Foundation).
    testConcatTimestampMathPure();
    testFrameBudgetAnd4KPool();
    testGameAudioRing();
    testMicMixFifo();
    testH264BoundsAndCodecSpecC3();
    testCodecFallbackBitratesW1();
    testResolutionBitrateSemanticsW9();

    std::string whyNot;
    if (mediaFoundationTestsAvailable(whyNot)) {
        testConcatAndC1SeamMath();
        testExportAudioSeamsW2();
        MFShutdown();
    } else {
        std::cout << "SKIP Media Foundation integration tests (Test 1, Test W2): " << whyNot << "\n";
    }

    CoUninitialize();
    std::cout << "ALL HOST TESTS PASSED!\n";
    return 0;
}
