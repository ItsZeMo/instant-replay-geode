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
#include <deque>
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
#include "recorder/SegmentRules.hpp"
#include "recorder/AudioQueuePolicy.hpp"

#include "shared_pure_tests.hpp"

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

// Writes one segment the way MediaFoundationEncoder does since W2: H.264 video plus AAC tracks that
// start with kSegmentAudioPrerollHns of silence, with the real audio shifted by the pre-roll.
struct PcmTrack {
    std::vector<int16_t> pcm; // interleaved stereo int16
    UINT32 rate = 48000;
    UINT32 bytesPerSecond = 24000;
};

void writePrerollSegment(
    const std::wstring& path,
    double videoDurationSec,
    const std::vector<PcmTrack>& tracks,
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
        aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, tracks[t].rate);
        aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, tracks[t].bytesPerSecond);
        CHECK_HR(writer->AddStream(aOut.get(), &audioIdx[t]));
        CHECK_HR(MFCreateMediaType(aIn.put()));
        aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, tracks[t].rate);
        aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        aIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, tracks[t].rate * 4);
        CHECK_HR(writer->SetInputMediaType(audioIdx[t], aIn.get(), nullptr));
    }
    CHECK_HR(writer->BeginWriting());

    auto writeAudio = [&](size_t t, const int16_t* data, int64_t frames, int64_t startFrame) {
        const int64_t rate = tracks[t].rate;
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
        smp->SetSampleTime((startFrame * 10000000LL) / rate);
        smp->SetSampleDuration(((startFrame + frames) * 10000000LL) / rate - (startFrame * 10000000LL) / rate);
        CHECK_HR(writer->WriteSample(audioIdx[t], smp.get()));
    };

    std::vector<int64_t> written(tracks.size(), 0);
    for (size_t t = 0; t < tracks.size(); ++t) {
        writeAudio(t, nullptr, replay::segmentAudioPrerollFrames(static_cast<int>(tracks[t].rate)), 0);
    }

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
        for (size_t t = 0; t < tracks.size(); ++t) {
            const int64_t rate = tracks[t].rate;
            const int64_t preroll = replay::segmentAudioPrerollFrames(static_cast<int>(rate));
            const int64_t target = ((f + 1) * frameDurHns * rate) / 10000000LL;
            const int64_t total = static_cast<int64_t>(tracks[t].pcm.size() / 2);
            while (written[t] < std::min(target, total)) {
                const int64_t n = std::min<int64_t>(rate / 100, std::min(target, total) - written[t]);
                writeAudio(t, tracks[t].pcm.data() + written[t] * 2, n, preroll + written[t]);
                written[t] += n;
            }
        }
    }
    for (size_t t = 0; t < tracks.size(); ++t) {
        const int64_t rate = tracks[t].rate;
        const int64_t preroll = replay::segmentAudioPrerollFrames(static_cast<int>(rate));
        const int64_t total = static_cast<int64_t>(tracks[t].pcm.size() / 2);
        while (written[t] < total) {
            const int64_t n = std::min<int64_t>(rate / 100, total - written[t]);
            writeAudio(t, tracks[t].pcm.data() + written[t] * 2, n, preroll + written[t]);
            written[t] += n;
        }
    }
    CHECK_HR(writer->Finalize());
}

// Decodes the audio track with the given sample rate to the left channel (float), placed by sample timestamps.
std::vector<float> decodeAudioTrack(const std::wstring& path, UINT32 wantedRate) {
    LocalComRef<IMFSourceReader> reader;
    CHECK_HR(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put()));
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    DWORD stream = MAXDWORD;
    for (DWORD s = 0; s < 16; ++s) {
        LocalComRef<IMFMediaType> t;
        HRESULT hr = reader->GetNativeMediaType(s, 0, t.put());
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        CHECK_HR(hr);
        GUID major = GUID_NULL;
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        UINT32 rate = 0;
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        if (major == MFMediaType_Audio && rate == wantedRate) { stream = s; break; }
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
            const size_t pos = static_cast<size_t>(std::llround(static_cast<double>(ts) * wantedRate / 1e7));
            if (left.size() < pos + n) left.resize(pos + n, 0.0f);
            for (size_t k = 0; k < n; ++k) left[pos + k] = f[k * 2];
            buf->Unlock();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    return left;
}

std::vector<int16_t> sineSegment(int64_t firstFrame, int64_t frames, double hz, double amp, int rate = 48000) {
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * 2);
    for (int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(firstFrame + i) / static_cast<double>(rate);
        const auto v = static_cast<int16_t>(std::lround(std::sin(2.0 * 3.14159265358979323846 * hz * t) * amp));
        pcm[static_cast<size_t>(i) * 2] = v;
        pcm[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return pcm;
}

// Every 5 ms window from `fromSec` to the end minus 25 ms stays within +-1 dB of the sine's RMS.
int countSeamDropouts(const std::vector<float>& left, double amp, double fromSec, double* outMinDb, int rate = 48000) {
    const size_t win = static_cast<size_t>(rate / 200); // 5 ms
    const double nominal = amp / 32768.0 / std::sqrt(2.0);
    const double lo = nominal * std::pow(10.0, -1.0 / 20.0);
    const double hi = nominal * std::pow(10.0, 1.0 / 20.0);
    int bad = 0;
    double minDb = 0.0;
    const size_t start = static_cast<size_t>(fromSec * rate);
    for (size_t w0 = start; w0 + win + static_cast<size_t>(rate / 40) <= left.size(); w0 += win) {
        double acc = 0.0;
        for (size_t k = 0; k < win; ++k) acc += static_cast<double>(left[w0 + k]) * left[w0 + k];
        const double rms = std::sqrt(acc / win);
        minDb = std::min(minDb, 20.0 * std::log10(std::max(rms, 1e-9) / nominal));
        if (rms < lo || rms > hi) ++bad;
    }
    if (outMinDb) *outMinDb = minDb;
    return bad;
}

// Track handler + timescale in 'moov/trak' order, i.e. the order players list streams in.
std::vector<std::pair<std::string, uint32_t>> mp4TrakOrder(const std::wstring& path) {
    std::vector<std::pair<std::string, uint32_t>> out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    CHECK(f != nullptr);
    std::vector<unsigned char> moov;
    auto be32 = [](const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
    long long pos = 0;
    while (true) {
        unsigned char hdr[16];
        if (_fseeki64(f, pos, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) break;
        uint64_t size = be32(hdr);
        if (size == 1) {
            if (fread(hdr + 8, 1, 8, f) != 8) break;
            size = (uint64_t(be32(hdr + 8)) << 32) | be32(hdr + 12);
        }
        if (size < 8) break;
        if (memcmp(hdr + 4, "moov", 4) == 0) {
            moov.resize(static_cast<size_t>(size));
            _fseeki64(f, pos, SEEK_SET);
            CHECK(fread(moov.data(), 1, moov.size(), f) == moov.size());
            break;
        }
        pos += static_cast<long long>(size);
    }
    fclose(f);
    CHECK(!moov.empty());
    auto findChild = [&](size_t begin, size_t end, const char* type, size_t& outBegin, size_t& outEnd) {
        for (size_t i = begin; i + 8 <= end;) {
            const uint32_t sz = be32(&moov[i]);
            if (sz < 8 || i + sz > end) return false;
            if (memcmp(&moov[i + 4], type, 4) == 0) { outBegin = i + 8; outEnd = i + sz; return true; }
            i += sz;
        }
        return false;
    };
    for (size_t i = 8; i + 8 <= moov.size();) {
        const uint32_t sz = be32(&moov[i]);
        if (sz < 8 || i + sz > moov.size()) break;
        if (memcmp(&moov[i + 4], "trak", 4) == 0) {
            size_t mb = 0, me = 0, hb = 0, he = 0, db = 0, de = 0;
            if (findChild(i + 8, i + sz, "mdia", mb, me) && findChild(mb, me, "hdlr", hb, he) && findChild(mb, me, "mdhd", db, de)) {
                const std::string handler(reinterpret_cast<const char*>(&moov[hb + 8]), 4);
                const uint32_t timescale = (moov[db] == 0) ? be32(&moov[db + 12]) : be32(&moov[db + 20]);
                out.emplace_back(handler, timescale);
            }
        }
        i += sz;
    }
    return out;
}

void testExportAudioSeamsW2() {
    std::cout << "[Test W2] Export re-encodes continuous AAC: no dropouts at segment seams...\n";
    auto tmp = std::filesystem::temp_directory_path() / L"ir_host_test_w2";
    std::filesystem::create_directories(tmp);

    // Like the game on the dev PC: FMOD at 44.1 kHz (192 kbps track added first), microphone at
    // 48 kHz (128 kbps). Media Foundation lists MP4 tracks in reverse order, so the exporter must
    // not rely on listing order; frame counts differ per track rate.
    constexpr int64_t kGameFrames = 88200; // 2.0 s @ 44.1 kHz
    constexpr int64_t kMicFrames = 96000;  // 2.0 s @ 48 kHz
    constexpr double kAmp = 16000.0;
    std::vector<replay::mfconcat::SegmentInput> inputs;
    for (int seg = 0; seg < 3; ++seg) {
        auto path = (tmp / (L"w2_seg" + std::to_wstring(seg) + L".mp4")).wstring();
        writePrerollSegment(path, 2.0, {
            PcmTrack{sineSegment(seg * kGameFrames, kGameFrames, 441.0, kAmp, 44100), 44100, 24000},
            PcmTrack{sineSegment(seg * kMicFrames, kMicFrames, 300.0, kAmp, 48000), 48000, 16000}
        });
        replay::mfconcat::SegmentInput in;
        in.path = path;
        in.durationSeconds = 2.0;
        in.audioFrames[0] = kGameFrames;
        in.audioFrames[1] = kMicFrames;
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

    for (UINT32 rate : {44100u, 48000u}) {
        auto left = decodeAudioTrack(out, rate);
        CHECK(left.size() >= static_cast<size_t>(rate) * 6 - 2048);
        double minDb = 0.0;
        // Skip the first 20 ms: the output's own AAC stream start (single, unavoidable).
        const int bad = countSeamDropouts(left, kAmp, 0.020, &minDb, static_cast<int>(rate));
        std::cout << "  " << rate << " Hz track: windows outside +-1 dB = " << bad << " (min " << minDb << " dB)\n";
        CHECK(bad == 0);
    }
    // Players list tracks in 'trak' order: video, then the game track, then the microphone.
    const auto order = mp4TrakOrder(out);
    CHECK(order.size() == 3);
    CHECK(order[0].first == "vide" && order[1].first == "soun" && order[1].second == 44100 &&
          order[2].first == "soun" && order[2].second == 48000);

    // Trimmed export keeps the stitched audio continuous as well.
    auto outTrim = (tmp / L"w2_trim.mp4").wstring();
    auto resTrim = replay::mfconcat::concatenate(inputs, outTrim, 0.7, opt);
    CHECK(resTrim.success);
    CHECK(resTrim.durationSeconds > 4.0 && resTrim.durationSeconds <= 6.0);
    for (UINT32 rate : {44100u, 48000u}) {
        CHECK(countSeamDropouts(decodeAudioTrack(outTrim, rate), kAmp, 0.020, nullptr, static_cast<int>(rate)) == 0);
    }

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::cout << "  PASS Test W2\n";
}

// W14 (found in game): with GD minimized no frame is captured, but FMOD audio continues and MF still
// finalizes the segment (empty video track). Such audio-only segments must survive the export.
void testAudioOnlySegmentExportW14() {
    std::cout << "[Test W14b] Export keeps audio-only segments (minimized game) continuous...\n";
    auto tmp = std::filesystem::temp_directory_path() / L"ir_host_test_w14";
    std::filesystem::create_directories(tmp);

    constexpr int64_t kSegFrames = 96000;
    constexpr double kAmp = 16000.0;
    std::vector<replay::mfconcat::SegmentInput> inputs;
    for (int seg = 0; seg < 3; ++seg) {
        auto path = (tmp / (L"w14_seg" + std::to_wstring(seg) + L".mp4")).wstring();
        const double videoSec = (seg == 1) ? 0.0 : 2.0; // middle segment: no video frames at all
        writePrerollSegment(path, videoSec, {PcmTrack{sineSegment(seg * kSegFrames, kSegFrames, 441.0, kAmp), 48000, 24000}});
        replay::mfconcat::SegmentInput in;
        in.path = path;
        in.durationSeconds = 2.0;
        in.audioFrames[0] = kSegFrames;
        inputs.push_back(in);
    }

    replay::mfconcat::Options opt;
    opt.audioPrerollHns = replay::kSegmentAudioPrerollHns;
    auto out = (tmp / L"w14_out.mp4").wstring();
    auto res = replay::mfconcat::concatenate(inputs, out, 0.0, opt);
    if (!res.success) std::cerr << "  concat error: " << res.error << "\n";
    CHECK(res.success);
    CHECK(std::abs(res.durationSeconds - 6.0) < 0.05);
    CHECK(countSeamDropouts(decodeAudioTrack(out, 48000), kAmp, 0.020, nullptr) == 0);

    // Replay that starts inside an audio-only segment: the trim must still apply (audio cut).
    std::vector<replay::mfconcat::SegmentInput> fromAudioOnly(inputs.begin() + 1, inputs.end());
    auto outTrim = (tmp / L"w14_trim.mp4").wstring();
    auto resTrim = replay::mfconcat::concatenate(fromAudioOnly, outTrim, 0.5, opt);
    if (!resTrim.success) std::cerr << "  concat error: " << resTrim.error << "\n";
    CHECK(resTrim.success);
    CHECK(std::abs(resTrim.durationSeconds - 3.5) < 0.05);
    CHECK(countSeamDropouts(decodeAudioTrack(outTrim, 48000), kAmp, 0.020, nullptr) == 0);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::cout << "  PASS Test W14b\n";
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

    // Pure tests: always run (also on CI runners without Media Foundation); shared with macOS (K7).
    replay_host_tests::runSharedPureTests();

    std::string whyNot;
    if (mediaFoundationTestsAvailable(whyNot)) {
        testConcatAndC1SeamMath();
        testExportAudioSeamsW2();
        testAudioOnlySegmentExportW14();
        MFShutdown();
    } else {
        std::cout << "SKIP Media Foundation integration tests (Test 1, Test W2): " << whyNot << "\n";
    }

    CoUninitialize();
    std::cout << "ALL HOST TESTS PASSED!\n";
    return 0;
}
