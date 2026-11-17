#include "MfSegmentConcat.hpp"

#ifdef _WIN32

#include "ConcatTimestampMath.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace replay::mfconcat {

namespace {

template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    void reset() noexcept {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }
    T** put() noexcept { reset(); return &m_p; }
    T* get() const noexcept { return m_p; }
    T* operator->() const noexcept { return m_p; }
    explicit operator bool() const noexcept { return m_p != nullptr; }

private:
    T* m_p = nullptr;
};

std::string hrText(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) ||
        hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL) ||
        hr == STG_E_MEDIUMFULL) {
        return "disk is full";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == E_ACCESSDENIED || hr == STG_E_ACCESSDENIED) {
        return "access denied";
    }
    if (hr == STG_E_DISKISWRITEPROTECTED || hr == HRESULT_FROM_WIN32(ERROR_WRITE_PROTECT)) {
        return "drive is write-protected";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08X", static_cast<unsigned>(hr));
    return buf;
}

// Microsoft's AAC encoder accepts only these average byte rates (96/128/160/192 kbps).
bool isSupportedAacByteRate(UINT32 bytesPerSecond) noexcept {
    return bytesPerSecond == 12000 || bytesPerSecond == 16000 ||
           bytesPerSecond == 20000 || bytesPerSecond == 24000;
}

HRESULT makePcm16Type(UINT32 rate, UINT32 channels, IMFMediaType** out) {
    HRESULT hr = MFCreateMediaType(out);
    if (FAILED(hr)) return hr;
    (*out)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    (*out)->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    (*out)->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    (*out)->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    (*out)->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    (*out)->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    (*out)->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    (*out)->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    return S_OK;
}

int64_t framesToHns(int64_t frames, UINT32 rate) noexcept {
    return (frames * 10000000LL) / static_cast<int64_t>(rate);
}

int64_t hnsToFramesRounded(int64_t hns, UINT32 rate) noexcept {
    const int64_t num = hns * static_cast<int64_t>(rate);
    return (num >= 0) ? (num + 5000000LL) / 10000000LL : -((-num + 5000000LL) / 10000000LL);
}

struct StreamInfo {
    DWORD readerIndex = 0;
    DWORD writerIndex = 0;
    bool isVideo = false;
    int audioTrack = -1;      // 0 = first audio track (game), 1 = second (split microphone)
    UINT32 rate = 0;
    UINT32 channels = 0;
    UINT32 blockAlign = 0;
    int64_t written = 0;      // PCM frames written to this output track so far
    // Per-segment audio window (frames on the segment's own content timeline)
    int64_t segBaseFrames = 0;
    int64_t segCutFrames = 0;
    int64_t segLimitFrames = 0;
    int64_t prerollFrames = 0;
};

} // namespace

ConcatResult concatenate(
    const std::vector<SegmentInput>& segments,
    const std::wstring& outputPath,
    double trimStartSeconds,
    const Options& options
) {
    ConcatResult res;
    auto fail = [&res](std::string msg) -> ConcatResult {
        res.success = false;
        res.error = std::move(msg);
        return res;
    };
    auto warn = [&options](const std::string& msg) {
        if (options.logWarn) options.logWarn(msg);
    };

    if (segments.empty()) return fail("no recorded segments");

    Com<IMFAttributes> readerAttr;
    HRESULT hr = MFCreateAttributes(readerAttr.put(), 1);
    if (FAILED(hr)) return fail("MFCreateAttributes: " + hrText(hr));
    readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);

    // 1. Discover the stream layout from the first segment and configure the output writer:
    //    video passthrough, audio PCM -> AAC with the segment's rate / channels / bitrate.
    std::vector<StreamInfo> streams;
    LONGLONG videoFrameDurationHns = 0;

    Com<IMFSinkWriter> writer;
    {
        Com<IMFSourceReader> firstReader;
        hr = MFCreateSourceReaderFromURL(segments[0].path.c_str(), readerAttr.get(), firstReader.put());
        if (FAILED(hr) || !firstReader) return fail("cannot open first segment: " + hrText(hr));

        Com<IMFAttributes> writerAttr;
        hr = MFCreateAttributes(writerAttr.put(), 2);
        if (FAILED(hr)) return fail("MFCreateAttributes: " + hrText(hr));
        writerAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        // Explicit MP4 container so the exporter can write to a temporary "*.mp4.part" name (W3).
        writerAttr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);

        hr = MFCreateSinkWriterFromURL(outputPath.c_str(), NULL, writerAttr.get(), writer.put());
        if (FAILED(hr) || !writer) return fail("cannot create output file: " + hrText(hr));

        int audioTracks = 0;
        for (DWORD s = 0; s < 16; ++s) {
            Com<IMFMediaType> type;
            hr = firstReader->GetNativeMediaType(s, 0, type.put());
            if (hr == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(hr) || !type) return fail("cannot read stream type: " + hrText(hr));

            GUID major = GUID_NULL;
            type->GetGUID(MF_MT_MAJOR_TYPE, &major);
            StreamInfo info;
            info.readerIndex = s;

            if (major == MFMediaType_Video) {
                info.isVideo = true;
                if (videoFrameDurationHns == 0) {
                    UINT32 fpsNum = 0, fpsDen = 1;
                    if (SUCCEEDED(MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen)) &&
                        fpsNum > 0 && fpsDen > 0) {
                        videoFrameDurationHns = (10000000LL * static_cast<LONGLONG>(fpsDen)) / static_cast<LONGLONG>(fpsNum);
                    }
                }
                hr = writer->AddStream(type.get(), &info.writerIndex);
                if (FAILED(hr)) return fail("cannot add output stream: " + hrText(hr));
                hr = writer->SetInputMediaType(info.writerIndex, type.get(), NULL);
                if (FAILED(hr)) return fail("cannot configure output stream: " + hrText(hr));
            } else if (major == MFMediaType_Audio) {
                info.audioTrack = audioTracks++;
                UINT32 rate = 0, channels = 0, bytesPerSec = 0;
                if (FAILED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) || rate == 0) rate = 48000;
                if (FAILED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) || channels == 0) channels = 2;
                if (FAILED(type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytesPerSec)) ||
                    !isSupportedAacByteRate(bytesPerSec)) {
                    bytesPerSec = (info.audioTrack == 0) ? 24000 : 16000;
                }
                info.rate = rate;
                info.channels = channels;
                info.blockAlign = channels * 2;

                Com<IMFMediaType> aacOut;
                hr = MFCreateMediaType(aacOut.put());
                if (FAILED(hr)) return fail("MFCreateMediaType: " + hrText(hr));
                aacOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                aacOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                aacOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                aacOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                aacOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                aacOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSec);
                hr = writer->AddStream(aacOut.get(), &info.writerIndex);
                if (FAILED(hr)) return fail("cannot add output audio stream: " + hrText(hr));

                Com<IMFMediaType> pcmIn;
                hr = makePcm16Type(rate, channels, pcmIn.put());
                if (FAILED(hr)) return fail("MFCreateMediaType: " + hrText(hr));
                hr = writer->SetInputMediaType(info.writerIndex, pcmIn.get(), NULL);
                if (FAILED(hr)) return fail("cannot configure AAC encoder: " + hrText(hr));
            } else {
                continue; // unknown stream type: not part of a replay
            }
            streams.push_back(info);
        }
        if (streams.empty()) return fail("first segment has no streams");

        hr = writer->BeginWriting();
        if (FAILED(hr)) return fail("BeginWriting: " + hrText(hr));
    }

    // 2. Scan first segment's video stream for keyframe cut point AND initial B-frame DTS lead (#21).
    LONGLONG cutHns = 0;
    LONGLONG firstVideoDts = 0;
    bool hasFirstVideoDts = false;
    const LONGLONG trimHns = static_cast<LONGLONG>(std::max(0.0, trimStartSeconds) * 1e7);
    auto videoIt = std::find_if(streams.begin(), streams.end(), [](const StreamInfo& s) { return s.isVideo; });

    if (videoIt != streams.end()) {
        Com<IMFSourceReader> scan;
        hr = MFCreateSourceReaderFromURL(segments[0].path.c_str(), readerAttr.get(), scan.put());
        if (FAILED(hr) || !scan) return fail("cannot open first segment: " + hrText(hr));
        scan->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        scan->SetStreamSelection(videoIt->readerIndex, TRUE);

        struct KeyCandidate {
            LONGLONG pts = -1;
            LONGLONG dts = 0;
            bool hasDts = false;
        };
        KeyCandidate initialSample, lastKeyBefore, firstKeyAfter;
        int emptyScans = 0;

        while (true) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            Com<IMFSample> sample;
            hr = scan->ReadSample(videoIt->readerIndex, 0, nullptr, &flags, &ts, sample.put());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) return fail("cannot scan first segment: " + hrText(hr));
            if (sample) {
                UINT64 rawDts = 0;
                bool sampleHasDts = SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &rawDts));
                LONGLONG sampleDts = static_cast<LONGLONG>(rawDts);

                if (initialSample.pts < 0) {
                    initialSample = {ts, sampleDts, sampleHasDts};
                    if (trimHns <= 0) break;
                }

                UINT32 clean = 0;
                bool isKey = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean != 0;
                if (ts <= trimHns) {
                    if (isKey) lastKeyBefore = {ts, sampleDts, sampleHasDts};
                } else {
                    if (isKey && firstKeyAfter.pts < 0) {
                        firstKeyAfter = {ts, sampleDts, sampleHasDts};
                        break;
                    }
                    if (ts - trimHns > 50000000LL) break; // scan up to 5s past trimHns
                }
                emptyScans = 0;
            } else if (++emptyScans > 100000) {
                return fail("reader stalled while scanning first segment");
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }

        if (trimHns <= 0) {
            cutHns = 0;
            hasFirstVideoDts = initialSample.hasDts;
            firstVideoDts = initialSample.dts;
        } else {
            constexpr LONGLONG kMaxKeyframeDriftHns = 20000000LL; // 2s
            KeyCandidate chosen = {0, initialSample.dts, initialSample.hasDts};
            if (lastKeyBefore.pts >= 0 && (trimHns - lastKeyBefore.pts) <= kMaxKeyframeDriftHns) {
                chosen = lastKeyBefore;
            } else if (firstKeyAfter.pts >= 0 && (firstKeyAfter.pts - trimHns) <= kMaxKeyframeDriftHns) {
                chosen = firstKeyAfter;
            } else if (lastKeyBefore.pts >= 0) {
                chosen = lastKeyBefore;
            } else if (firstKeyAfter.pts >= 0) {
                chosen = firstKeyAfter;
            }
            cutHns = chosen.pts;
            hasFirstVideoDts = chosen.hasDts;
            firstVideoDts = chosen.dts;
        }
    } else if (trimHns > 0) {
        cutHns = trimHns;
    }

    // Shift all streams equally by any initial B-frame decode lead (cutHns - firstVideoDts)
    // so rebased DTS starts at >= 0 without clamping multiple initial packets to DTS == 0 (#21).
    const LONGLONG initialPtsShift = (hasFirstVideoDts && firstVideoDts < cutHns)
        ? (cutHns - firstVideoDts)
        : 0;

    // 3. Merge every segment: remux video, decode + stitch audio onto the output timeline.
    ConcatTimestampState concatState(initialPtsShift);
    size_t samplesWritten = 0;
    std::vector<BYTE> silenceBytes;

    double totalSeconds = 0.0;
    for (const auto& seg : segments) totalSeconds += std::max(0.0, seg.durationSeconds);
    double completedSeconds = 0.0;
    auto lastProgressLog = std::chrono::steady_clock::now();
    auto reportProgress = [&](double segmentSeconds, size_t segIdx) {
        int permille = 0;
        if (totalSeconds > 0.0) {
            permille = static_cast<int>(std::clamp((completedSeconds + segmentSeconds) / totalSeconds, 0.0, 1.0) * 1000.0);
        } else {
            permille = static_cast<int>((segIdx * 1000) / segments.size());
        }
        if (options.progressPermille) options.progressPermille->store(permille, std::memory_order_relaxed);
        auto now = std::chrono::steady_clock::now();
        if (options.logInfo && now - lastProgressLog >= std::chrono::seconds(2)) {
            lastProgressLog = now;
            options.logInfo("Instant Replay: export progress " + std::to_string(permille / 10) + "% (segment " +
                            std::to_string(segIdx + 1) + "/" + std::to_string(segments.size()) + ")");
        }
    };

    auto writePcm = [&](StreamInfo& st, const BYTE* data, int64_t frames) -> HRESULT {
        const DWORD bytes = static_cast<DWORD>(frames * st.blockAlign);
        Com<IMFMediaBuffer> buf;
        HRESULT h = MFCreateMemoryBuffer(bytes, buf.put());
        if (FAILED(h) || !buf) return FAILED(h) ? h : E_OUTOFMEMORY;
        BYTE* dst = nullptr;
        h = buf->Lock(&dst, nullptr, nullptr);
        if (FAILED(h) || !dst) return FAILED(h) ? h : E_FAIL;
        if (data) {
            memcpy(dst, data, bytes);
        } else {
            memset(dst, 0, bytes);
        }
        buf->Unlock();
        buf->SetCurrentLength(bytes);

        Com<IMFSample> out;
        h = MFCreateSample(out.put());
        if (FAILED(h) || !out) return FAILED(h) ? h : E_OUTOFMEMORY;
        out->AddBuffer(buf.get());
        const int64_t startHns = framesToHns(st.written, st.rate);
        const int64_t endHns = framesToHns(st.written + frames, st.rate);
        out->SetSampleTime(startHns);
        out->SetSampleDuration(endHns - startHns);
        h = writer->WriteSample(st.writerIndex, out.get());
        if (FAILED(h)) return h;
        concatState.recordWrittenSample(st.writerIndex, false, startHns, endHns - startHns);
        st.written += frames;
        ++samplesWritten;
        return S_OK;
    };

    auto writeSilence = [&](StreamInfo& st, int64_t frames) -> HRESULT {
        while (frames > 0) {
            const int64_t chunk = std::min<int64_t>(frames, st.rate);
            HRESULT h = writePcm(st, nullptr, chunk);
            if (FAILED(h)) return h;
            frames -= chunk;
        }
        return S_OK;
    };

    for (size_t segIdx = 0; segIdx < segments.size(); ++segIdx) {
        const SegmentInput& seg = segments[segIdx];
        const std::string segLabel = "segment " + std::to_string(segIdx + 1) + "/" + std::to_string(segments.size());

        Com<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromURL(seg.path.c_str(), readerAttr.get(), reader.put());
        if (FAILED(hr) || !reader) return fail("cannot open " + segLabel + ": " + hrText(hr));

        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        std::map<DWORD, size_t> readerToStream;
        std::set<DWORD> remaining;
        for (size_t i = 0; i < streams.size(); ++i) {
            StreamInfo& st = streams[i];
            Com<IMFMediaType> native;
            hr = reader->GetNativeMediaType(st.readerIndex, 0, native.put());
            if (FAILED(hr) || !native) return fail(segLabel + " is missing a stream: " + hrText(hr));
            if (!st.isVideo) {
                // The source reader inserts Microsoft's AAC decoder for us.
                Com<IMFMediaType> pcm;
                hr = makePcm16Type(st.rate, st.channels, pcm.put());
                if (FAILED(hr)) return fail("MFCreateMediaType: " + hrText(hr));
                hr = reader->SetCurrentMediaType(st.readerIndex, nullptr, pcm.get());
                if (FAILED(hr)) return fail("cannot decode audio in " + segLabel + ": " + hrText(hr));
                Com<IMFMediaType> current;
                UINT32 rate = 0, channels = 0, bits = 0;
                if (FAILED(reader->GetCurrentMediaType(st.readerIndex, current.put())) || !current ||
                    FAILED(current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) ||
                    FAILED(current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
                    FAILED(current->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) ||
                    rate != st.rate || channels != st.channels || bits != 16) {
                    return fail("unexpected decoded audio format in " + segLabel);
                }
            }
            hr = reader->SetStreamSelection(st.readerIndex, TRUE);
            if (FAILED(hr)) return fail("cannot select stream in " + segLabel + ": " + hrText(hr));
            readerToStream[st.readerIndex] = i;
            remaining.insert(st.readerIndex);
        }

        const LONGLONG segCut = (segIdx == 0) ? cutHns : 0;
        bool seenFirstVideoKey = (segCut <= 0);
        concatState.beginSegment();
        const int64_t segBaseHns = concatState.outputBase;
        for (auto& st : streams) {
            if (st.isVideo) continue;
            st.segBaseFrames = hnsToFramesRounded(segBaseHns, st.rate);
            st.segCutFrames = hnsToFramesRounded(segCut, st.rate);
            st.prerollFrames = hnsToFramesRounded(options.audioPrerollHns, st.rate);
            const int64_t known = (st.audioTrack >= 0 && st.audioTrack < 2) ? seg.audioFrames[st.audioTrack] : -1;
            st.segLimitFrames = (known >= 0) ? known : std::numeric_limits<int64_t>::max();
        }
        int emptyReads = 0;
        bool gapWarned = false;
        const double segSeconds = std::max(0.0, seg.durationSeconds);

        while (!remaining.empty()) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            Com<IMFSample> sample;
            hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &actualStream, &flags, &timestamp, sample.put());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                return fail("cannot read " + segLabel + ": " + hrText(hr));
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                remaining.erase(actualStream);
            }
            if (!sample) {
                if (++emptyReads > 100000) return fail("reader stalled on " + segLabel);
                continue;
            }
            emptyReads = 0;

            auto mapIt = readerToStream.find(actualStream);
            if (mapIt == readerToStream.end()) continue;
            StreamInfo& st = streams[mapIt->second];

            if (st.isVideo) {
                if (timestamp < segCut) continue; // trimmed away (before the replay start)
                if (!seenFirstVideoKey) {
                    UINT32 clean = 0;
                    if (FAILED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) || clean == 0) {
                        continue;
                    }
                    seenFirstVideoKey = true;
                }

                LONGLONG sampleDuration = 0;
                sample->GetSampleDuration(&sampleDuration);
                int64_t sampleTime = concatState.computeSampleTime(timestamp, segCut);
                UINT64 dts = 0;
                if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))) {
                    int64_t newDts = 0;
                    concatState.adjustVideoTimestamps(static_cast<int64_t>(dts), segCut, sampleTime, newDts);
                    sample->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(newDts));
                }
                sample->SetSampleTime(sampleTime);

                hr = writer->WriteSample(st.writerIndex, sample.get());
                if (FAILED(hr)) {
                    return fail("write failed (" + segLabel + "): " + hrText(hr));
                }
                ++samplesWritten;
                concatState.recordWrittenSample(st.writerIndex, true, sampleTime, sampleDuration);
                reportProgress(std::min(segSeconds, static_cast<double>(timestamp - segCut) / 1e7), segIdx);
                continue;
            }

            // Decoded PCM: map it onto the segment's content timeline (drop pre-roll, trim start,
            // cut trailing AAC padding), then onto the continuous output timeline.
            Com<IMFMediaBuffer> buf;
            hr = sample->ConvertToContiguousBuffer(buf.put());
            if (FAILED(hr) || !buf) return fail("cannot read decoded audio in " + segLabel + ": " + hrText(hr));
            BYTE* data = nullptr;
            DWORD len = 0;
            hr = buf->Lock(&data, nullptr, &len);
            if (FAILED(hr) || !data) return fail("cannot lock decoded audio in " + segLabel + ": " + hrText(hr));

            const int64_t frames = static_cast<int64_t>(len / st.blockAlign);
            const int64_t srcFirst = hnsToFramesRounded(timestamp, st.rate) - st.prerollFrames;
            const int64_t clipStart = std::max(srcFirst, st.segCutFrames);
            const int64_t clipEnd = std::min(srcFirst + frames, st.segLimitFrames);
            HRESULT writeHr = S_OK;
            if (clipEnd > clipStart) {
                const int64_t outStart = st.segBaseFrames + (clipStart - st.segCutFrames);
                if (outStart > st.written) {
                    const int64_t gap = outStart - st.written;
                    if (!gapWarned && gap > static_cast<int64_t>(st.rate) / 10) {
                        gapWarned = true;
                        warn("MediaFoundationEncoder: audio track " + std::to_string(st.audioTrack) + " has a " +
                             std::to_string((gap * 1000) / st.rate) + " ms gap in " + segLabel + ", filling with silence");
                    }
                    writeHr = writeSilence(st, gap);
                }
                if (SUCCEEDED(writeHr)) {
                    const int64_t skip = std::max<int64_t>(0, st.written - outStart);
                    const int64_t first = clipStart + skip;
                    const int64_t count = clipEnd - first;
                    if (count > 0) {
                        writeHr = writePcm(st, data + (first - srcFirst) * st.blockAlign, count);
                    }
                }
            }
            buf->Unlock();
            if (FAILED(writeHr)) {
                return fail("audio write failed (" + segLabel + "): " + hrText(writeHr));
            }
        }

        bool anomalyWarn = false;
        int64_t excessOverAudioHns = 0;
        concatState.endSegment(videoFrameDurationHns, &anomalyWarn, &excessOverAudioHns);
        if (anomalyWarn) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(excessOverAudioHns) / 10000.0);
            warn("MediaFoundationEncoder: video end exceeded primary audio end by " + std::string(buf) +
                 " ms (>1.5 frames) in " + segLabel + ", falling back to max segment end");
        }
        completedSeconds += segSeconds;
        reportProgress(0.0, segIdx);
    }

    if (samplesWritten == 0) return fail("no media samples were written");

    hr = writer->Finalize();
    if (FAILED(hr)) return fail("finalizing MP4 failed: " + hrText(hr));
    if (options.progressPermille) options.progressPermille->store(1000, std::memory_order_relaxed);

    res.success = true;
    res.durationSeconds = static_cast<double>(std::max<LONGLONG>(0, concatState.outputBase - initialPtsShift)) / 1e7;
    return res;
}

} // namespace replay::mfconcat

#endif // _WIN32
