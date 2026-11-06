// The game's CG movies (movie\*.xmv on the disc: 640x480, 29.97 fps, the picture letterboxed in the frame), drawn
// from HD versions where a movie pack has them, and as large as the screen takes them in widescreen.
//
// The game plays its own movie as ever: its timing, the voices (movie_sound_*.cv2, not in the movies), skipping and
// fades. Its decoder writes each frame into a 640x480 texture (linear A8R8G8B8) that it draws over the frame as one
// quad. Each new frame's picture is matched to the original's frames by a 16x12 grey thumbnail (the frame's 40x40
// blocks, each block's mean), from the frame drawn before on, so a dropped or repeated frame keeps the two together;
// the renderer then draws that frame of the HD version in its place (gpu::Extension::Replace, widescreen.cpp).
//
// A pack is a "movies" folder beside the game (or the folder CV_MOVIES names) with movies.toml:
//   [st01_00]
//   video = "st01_00.mp4"      # the picture without its bars, any scale; a path relative to the folder
//   frames = 5938              # the original's frame count
//   crop = [0, 43, 640, 394]   # where the picture lies in the 640x480 frame
// and <name>.sig in the folder: the original's thumbnails, 192 bytes a frame (each block's grey: (R+G+B)/3), the
// first frame first (tools/movies/make_movie_pack.py makes both from the game's movies and the HD videos). Videos are
// decoded by Windows (Media Foundation), on the graphics card when it can: H.264 on any Windows, HEVC with Microsoft's
// HEVC extension; on Linux by FFmpeg, on the processor (H.264 and HEVC, the release's LGPL build). A movie without a
// video in the pack plays as the game has it.
//
// movie_view: 0 the game's 4:3 frame as it was; 1 the movie's picture as large as the screen shows it whole (as tall
// as a 16:9 screen, as wide as a 16:10 one); 2 the screen filled, a little cut off the picture's top and bottom on
// 16:9 (its sides on 4:3). With or without a pack: the rows each movie's picture lies on are known.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "gpu/ext.h"
#include "kernel/kernel.h"

#ifdef _WIN32
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#endif

bool g_hd_movies = true;    // a movie pack's HD movies drawn (the settings file, menu.cpp)
uint32_t g_movie_view = 0;  // 0 the game's 4:3, 1 the picture whole as large as it goes, 2 the screen filled

namespace {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif
namespace fs = std::filesystem;

constexpr uint32_t kW = 640, kH = 480;                           // the game's movie frame
constexpr uint32_t kSigW = 16, kSigH = 12, kSig = kSigW * kSigH;  // a thumbnail: 40x40 blocks
constexpr uint32_t kFmtLinA8R8G8B8 = 0x12;                       // the game's movie texture
constexpr uint32_t kAhead = 6;                                    // frames decoded ahead of the one drawn
constexpr uint32_t kFar = 45;                                     // a frame asked for past this many on: a seek
constexpr uint64_t kTag = 1ull << 62;                             // a movie frame's replacement id
constexpr uint64_t kIdleFrames = 90;  // the game's frames drawn without a new movie frame: it is over
constexpr double kFps = 30000.0 / 1001.0;

// the rows of a movie's picture in its 640x480 frame: the bars every movie has, but the opening's text crawl's
void DefaultCrop(const std::string& name, uint32_t& y, uint32_t& h) {
    y = 43;
    h = 394;
    if (name == "st00_00" || name == "st00_00j") {
        y = 32;
        h = name == "st00_00" ? 408 : 410;
    }
}

bool MovieTexture(const gpu::TextureDesc& d) {
    return d.linear && d.format == kFmtLinA8R8G8B8 && d.width == kW && d.height == kH;
}

#ifdef _WIN32
// decodes one video on a thread of its own, frames in order, a few ahead of the one asked for, each as the picture
// the renderer draws in place of the game's frame: the video in the middle of a frame of the game's shape (its bars
// black), so the game's quad and texture coordinates fit it as they are
class Decoder {
public:
    Decoder(fs::path video, uint32_t crop_h, uint8_t alpha)
        : video_(std::move(video)), crop_h_(crop_h), alpha_(alpha), thread_([this] { Run(); }) {}
    ~Decoder() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    // frame n's picture, null until it is decoded; frames from n on are decoded next
    std::shared_ptr<const gpu::Replacement> Frame(uint32_t n) {
        std::lock_guard lock(m_);
        if (n != want_) {
            want_ = n;
            cv_.notify_all();
        }
        auto it = ready_.find(n);
        return it != ready_.end() ? it->second : nullptr;
    }
    bool Failed() const { return failed_; }

private:
    bool Open(ComPtr<IMFSourceReader>& reader, bool hardware) {
        ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs, 4))) return false;
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (hardware) {
            // the graphics card decodes and converts; the frames come back to memory for the renderer to upload
            ComPtr<ID3D11Device> dev;
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                         D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
                return false;
            ComPtr<ID3D10Multithread> mt;
            if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
            UINT token = 0;
            if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_)) || FAILED(manager_->ResetDevice(dev.Get(), token)))
                return false;
            attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager_.Get());
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }
        if (FAILED(MFCreateSourceReaderFromURL(video_.c_str(), attrs.Get(), &reader))) return false;
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        // the video's picture: its frames are coded a little larger (HEVC's to a multiple of 32 or 64 rows: 2560x1576 as
        // 2560x1600), the rest outside its aperture
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native)) ||
            FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &native_w_, &native_h_)))
            return false;
        Aperture(native.Get(), native_w_, native_h_, pic_);
        pic_native_ = pic_;
        ComPtr<IMFMediaType> type;
        if (FAILED(MFCreateMediaType(&type))) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        // the picture alone at its own size, else as the decoder gives it
        MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, UINT32(pic_.Area.cx), UINT32(pic_.Area.cy));
        if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()))) {
            type->DeleteItem(MF_MT_FRAME_SIZE);
            if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get())))
                return false;
        }
        ComPtr<IMFMediaType> got;
        if (FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &got)) ||
            FAILED(MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &w_, &h_)) || !w_ || !h_)
            return false;
        // the picture in the frames as they come
        if (w_ == UINT32(pic_.Area.cx) && h_ == UINT32(pic_.Area.cy)) pic_ = MFVideoArea{{0, 0}, {0, 0}, {LONG(w_), LONG(h_)}};
        else Aperture(got.Get(), w_, h_, pic_);
        // a plain buffer's rows bottom up when the type's stride says so
        UINT32 stride = 0;
        if (SUCCEEDED(got->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) bottom_up_ = INT32(stride) < 0;
        return true;
    }

    // the part of a type's frames that is the picture: its minimum display aperture, else its geometric one, else the
    // whole frame
    static void Aperture(IMFMediaType* t, UINT32 w, UINT32 h, MFVideoArea& a) {
        UINT32 got = 0;
        a = MFVideoArea{{0, 0}, {0, 0}, {LONG(w), LONG(h)}};
        MFVideoArea v{};
        if ((SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&v), sizeof(v), &got)) ||
             SUCCEEDED(t->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&v), sizeof(v), &got))) &&
            got == sizeof(v) && v.Area.cx > 0 && v.Area.cy > 0 && v.OffsetX.value >= 0 && v.OffsetY.value >= 0 &&
            UINT32(v.OffsetX.value + v.Area.cx) <= w && UINT32(v.OffsetY.value + v.Area.cy) <= h)
            a = v;
    }

    // a decoded frame as the game frame's picture: the video's rows at the picture's rows of a frame of the game's
    // shape, black above and below. One level: it is about the screen's size, and the renderer filters it.
    // pitch from one row to the one below it
    std::shared_ptr<gpu::Replacement> Make(const uint8_t* top, LONG pitch) {
        auto r = std::make_shared<gpu::Replacement>();
        const uint32_t pw = uint32_t(pic_.Area.cx), ph = uint32_t(pic_.Area.cy);
        top += ptrdiff_t(pic_.OffsetY.value) * pitch + ptrdiff_t(pic_.OffsetX.value) * 4;
        // in the middle of the frame (the game's frames have the opening's text crawl a few rows above it), which
        // Movies::Scale takes it as
        uint32_t fw = pw, fh = std::max(uint32_t(std::lround(double(ph) * kH / crop_h_)), ph);
        uint32_t y0 = (fh - ph) / 2;
        const uint8_t a = alpha_;
        std::vector<uint8_t> pic(size_t(fw) * fh * 4, 0);
        for (size_t i = 3; i < pic.size(); i += 4) pic[i] = a;
        for (uint32_t y = 0; y < ph; y++) {
            const uint8_t* s = top + ptrdiff_t(y) * pitch;
            uint8_t* d = &pic[size_t(y0 + y) * fw * 4];
            for (uint32_t x = 0; x < pw; x++, s += 4, d += 4) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = a;
            }
        }
        if (!told_) {
            // the first frame's rows with anything brighter than black in them: the picture should fill its rows
            told_ = true;
            int first = -1, last = -1;
            for (uint32_t y = 0; y < ph; y++) {
                const uint8_t* s = &pic[size_t(y0 + y) * fw * 4];
                bool lit = false;
                for (uint32_t x = 0; x < pw && !lit; x++) lit = s[x * 4] > 24 || s[x * 4 + 1] > 24 || s[x * 4 + 2] > 24;
                if (lit) {
                    if (first < 0) first = int(y);
                    last = int(y);
                }
            }
            LOGR("movies: %s's first frame lit from row %d to %d of %u", video_.filename().string().c_str(), first, last, ph);
        }
        r->width = fw;
        r->height = fh;
        r->levels.push_back(std::move(pic));
        return r;
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        ComPtr<IMFSourceReader> reader;
        bool hw = Open(reader, true);
        if (!hw) {
            reader.Reset();
            manager_.Reset();
            if (!Open(reader, false)) {
                LOGW("movies: %s does not open for decoding (an HEVC video needs Microsoft's HEVC extension); the game's "
                     "own movie plays",
                     video_.filename().string().c_str());
                failed_ = true;
                MFShutdown();
                CoUninitialize();
                return;
            }
        }
        LOGR("movies: %s, %ux%u (its picture %ldx%ld at %ld,%ld), decoded %s to %ux%u (the picture %ldx%ld at %ld,%ld)",
             video_.filename().string().c_str(), native_w_, native_h_, long(pic_native_.Area.cx), long(pic_native_.Area.cy),
             long(pic_native_.OffsetX.value), long(pic_native_.OffsetY.value), hw ? "on the graphics card" : "by the processor",
             w_, h_, long(pic_.Area.cx), long(pic_.Area.cy), long(pic_.OffsetX.value), long(pic_.OffsetY.value));
        uint32_t next = 0;     // the frame the next sample should be
        bool end = false;
        LONGLONG origin = -1;  // the first frame's time
        for (;;) {
            uint32_t want;
            {
                std::unique_lock lock(m_);
                // the frames held start here (where decoding goes on from, when none are)
                auto first = [&] { return ready_.empty() ? next : ready_.begin()->first; };
                auto seek = [&] { return want_ + 2 < first() || want_ > next + kFar; };
                // ahead enough, or at the end: wait for the renderer to ask for a later frame, or one far off
                cv_.wait(lock, [&] { return stop_ || seek() || (want_ + kAhead > next && !end); });
                if (stop_) break;
                want = want_;
                if (seek()) {
                    ready_.clear();
                    PROPVARIANT pos;
                    PropVariantInit(&pos);
                    pos.vt = VT_I8;
                    pos.hVal.QuadPart = std::max<LONGLONG>(origin, 0) + LONGLONG(double(want) / kFps * 1e7);
                    reader->SetCurrentPosition(GUID_NULL, pos);
                    next = want;
                    end = false;
                }
            }
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &sample)) ||
                (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                end = true;
                continue;
            }
            if (!sample) continue;
            if (origin < 0) origin = ts;
            uint32_t n = uint32_t(std::max<LONGLONG>(0, std::llround(double(ts - origin) * 1e-7 * kFps)));
            next = n + 1;
            if (n + 2 < want) continue;  // on the way to a frame asked for
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
            std::shared_ptr<gpu::Replacement> pic;
            ComPtr<IMF2DBuffer> b2;
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
                pic = Make(scan0, pitch);  // the top row, and a negative pitch for rows bottom up
                b2->Unlock2D();
            } else {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                    LONG row = LONG(w_ * 4);
                    if (len >= DWORD(row) * h_) pic = bottom_up_ ? Make(p + ptrdiff_t(h_ - 1) * row, -row) : Make(p, row);
                    buf->Unlock();
                }
            }
            if (!pic) continue;
            std::lock_guard lock(m_);
            ready_[n] = std::move(pic);
            while (!ready_.empty() && ready_.begin()->first + 2 < want_) ready_.erase(ready_.begin());
        }
        reader.Reset();
        manager_.Reset();
        MFShutdown();
        CoUninitialize();
    }

    fs::path video_;
    uint32_t crop_h_;  // the picture's rows of the game's 480
    uint8_t alpha_;
    UINT32 w_ = 0, h_ = 0;                // the frames as decoded
    UINT32 native_w_ = 0, native_h_ = 0;  // as coded
    MFVideoArea pic_{}, pic_native_{};    // the picture in each
    bool bottom_up_ = false;
    bool told_ = false;
    std::atomic<bool> failed_{false};
    ComPtr<IMFDXGIDeviceManager> manager_;
    std::mutex m_;
    std::condition_variable cv_;
    std::map<uint32_t, std::shared_ptr<const gpu::Replacement>> ready_;
    uint32_t want_ = 0;
    bool stop_ = false;
    std::thread thread_;  // last: started once the rest is made
};
#else
// Linux: FFmpeg's decoding (libavformat, libavcodec and swscale; a release's lib folder has an LGPL build of them with
// the H.264 and HEVC decoders alone), on a thread of its own as Windows' decoder: frames in order, a few ahead of the
// one asked for, each as the picture the renderer draws in place of the game's frame (the video in the middle of a
// frame of the game's shape, its bars black)
class Decoder {
public:
    Decoder(fs::path video, uint32_t crop_h, uint8_t alpha)
        : video_(std::move(video)), crop_h_(crop_h), alpha_(alpha), thread_([this] { Run(); }) {}
    ~Decoder() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    // frame n's picture, null until it is decoded; frames from n on are decoded next
    std::shared_ptr<const gpu::Replacement> Frame(uint32_t n) {
        std::lock_guard lock(m_);
        if (n != want_) {
            want_ = n;
            cv_.notify_all();
        }
        auto it = ready_.find(n);
        return it != ready_.end() ? it->second : nullptr;
    }
    bool Failed() const { return failed_; }

private:
    // a decoded frame (BGRA, pw x ph, the picture without the coded rows past it) as the game frame's picture: its
    // rows at the picture's rows of a frame of the game's shape, black above and below; one level, as on Windows
    std::shared_ptr<gpu::Replacement> Make(const uint8_t* top, ptrdiff_t pitch, uint32_t pw, uint32_t ph) {
        auto r = std::make_shared<gpu::Replacement>();
        uint32_t fw = pw, fh = std::max(uint32_t(std::lround(double(ph) * kH / crop_h_)), ph);
        uint32_t y0 = (fh - ph) / 2;
        const uint8_t a = alpha_;
        std::vector<uint8_t> pic(size_t(fw) * fh * 4, 0);
        for (size_t i = 3; i < pic.size(); i += 4) pic[i] = a;
        for (uint32_t y = 0; y < ph; y++) {
            const uint8_t* s = top + ptrdiff_t(y) * pitch;
            uint8_t* d = &pic[size_t(y0 + y) * fw * 4];
            for (uint32_t x = 0; x < pw; x++, s += 4, d += 4) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = a;
            }
        }
        r->width = fw;
        r->height = fh;
        r->levels.push_back(std::move(pic));
        return r;
    }

    void Fail(const char* why) {
        LOGW("movies: %s %s; the game's own movie plays", video_.filename().string().c_str(), why);
        failed_ = true;
    }

    void Run() {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, video_.string().c_str(), nullptr, nullptr) < 0) return Fail("does not open");
        AVCodecContext* cc = nullptr;
        AVPacket* pkt = av_packet_alloc();
        AVFrame* fr = av_frame_alloc();
        SwsContext* sws = nullptr;
        std::vector<uint8_t> bgra;
        int si = -1;
        const AVCodec* codec = nullptr;
        if (avformat_find_stream_info(fmt, nullptr) < 0 ||
            (si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0 || !codec ||
            !(cc = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(cc, fmt->streams[si]->codecpar) < 0) {
            Fail("has no video this build of FFmpeg decodes (H.264 or HEVC)");
        } else {
            cc->thread_count = 0;  // as many as it finds worth
            if (avcodec_open2(cc, codec, nullptr) < 0) {
                Fail("does not open for decoding");
            } else {
                Loop(fmt, cc, si, pkt, fr, sws, bgra);
            }
        }
        sws_freeContext(sws);
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
    }

    void Loop(AVFormatContext* fmt, AVCodecContext* cc, int si, AVPacket* pkt, AVFrame* fr, SwsContext*& sws,
              std::vector<uint8_t>& bgra) {
        const double tb = av_q2d(fmt->streams[si]->time_base);
        LOGR("movies: %s, %dx%d %s, decoded by the processor (FFmpeg %s)", video_.filename().string().c_str(),
             cc->width, cc->height, avcodec_get_name(cc->codec_id), av_version_info());
        uint32_t next = 0;      // the frame the next one decoded should be
        bool end = false;       // the video's last frame given
        bool flushed = false;   // the decoder told the video is over
        int64_t origin = AV_NOPTS_VALUE;  // the first frame's time
        for (;;) {
            uint32_t want;
            {
                std::unique_lock lock(m_);
                auto first = [&] { return ready_.empty() ? next : ready_.begin()->first; };
                auto seek = [&] { return want_ + 2 < first() || want_ > next + kFar; };
                cv_.wait(lock, [&] { return stop_ || seek() || (want_ + kAhead > next && !end); });
                if (stop_) break;
                want = want_;
                if (seek()) {
                    ready_.clear();
                    int64_t at = (origin == AV_NOPTS_VALUE ? 0 : origin) + int64_t(double(want) / kFps / tb);
                    av_seek_frame(fmt, si, at, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(cc);
                    next = want;
                    end = flushed = false;
                }
            }
            int r = avcodec_receive_frame(cc, fr);
            if (r == AVERROR(EAGAIN)) {
                // more of the video for the decoder; at its end, told so, for the frames it still holds
                if (flushed) {
                    end = true;
                    continue;
                }
                int got = av_read_frame(fmt, pkt);
                if (got < 0) {
                    avcodec_send_packet(cc, nullptr);
                    flushed = true;
                } else {
                    if (pkt->stream_index == si) avcodec_send_packet(cc, pkt);
                    av_packet_unref(pkt);
                }
                continue;
            }
            if (r < 0) {  // AVERROR_EOF, or a broken stream
                end = true;
                continue;
            }
            int64_t ts = fr->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = fr->pts;
            if (origin == AV_NOPTS_VALUE) origin = ts == AV_NOPTS_VALUE ? 0 : ts;
            uint32_t n = uint32_t(std::max<int64_t>(0, std::llround(double(ts - origin) * tb * kFps)));
            next = n + 1;
            if (n + 2 < want) continue;  // on the way to a frame asked for
            const uint32_t pw = uint32_t(fr->width), ph = uint32_t(fr->height);
            sws = sws_getCachedContext(sws, fr->width, fr->height, AVPixelFormat(fr->format), fr->width, fr->height,
                                       AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws || !pw || !ph) continue;
            bgra.resize(size_t(pw) * ph * 4);
            uint8_t* dst[1] = {bgra.data()};
            int dst_pitch[1] = {int(pw * 4)};
            sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, dst_pitch);
            std::shared_ptr<gpu::Replacement> pic = Make(bgra.data(), ptrdiff_t(pw) * 4, pw, ph);
            if (!told_) {
                told_ = true;
                LOGR("movies: %s's frames %ux%u", video_.filename().string().c_str(), pw, ph);
            }
            std::lock_guard lock(m_);
            ready_[n] = std::move(pic);
            while (!ready_.empty() && ready_.begin()->first + 2 < want_) ready_.erase(ready_.begin());
        }
    }

    fs::path video_;
    uint32_t crop_h_;  // the picture's rows of the game's 480
    uint8_t alpha_;
    bool told_ = false;
    std::atomic<bool> failed_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::map<uint32_t, std::shared_ptr<const gpu::Replacement>> ready_;
    uint32_t want_ = 0;
    bool stop_ = false;
    std::thread thread_;  // last: started once the rest is made
};
#endif

struct Movie {
    fs::path video;
    uint32_t frames = 0, crop_y = 43, crop_h = 394;
    std::vector<float> sig;  // each frame's thumbnail, made zero mean and unit length
    std::vector<bool> flat;  // a frame all of one grey (black): its thumbnail tells nothing
};

// a thumbnail made zero mean and unit length; false when it is all about one grey
bool Normalize(const float* in, float* out) {
    float mean = 0;
    for (uint32_t i = 0; i < kSig; i++) mean += in[i];
    mean /= kSig;
    float len = 0;
    for (uint32_t i = 0; i < kSig; i++) {
        out[i] = in[i] - mean;
        len += out[i] * out[i];
    }
    len = std::sqrt(len);
    if (len < 2.0f * std::sqrt(float(kSig))) return false;  // its blocks within about 2 of their mean
    for (uint32_t i = 0; i < kSig; i++) out[i] /= len;
    return true;
}

class Movies {
public:
    void Init() {
        fs::path dir;
        if (const char* env = std::getenv("CV_MOVIES")) dir = env;
        else dir = app::ExeDir() / "movies";
        std::error_code ec;
        fs::path manifest = dir / "movies.toml";
        if (!fs::is_regular_file(manifest, ec)) return;
        toml::table t;
        try {
            t = toml::parse_file(manifest.string());
        } catch (const std::exception& e) {
            LOGW("movies: %s does not read: %s", manifest.string().c_str(), e.what());
            return;
        }
        for (auto&& [k, v] : t) {
            const toml::table* m = v.as_table();
            if (!m) continue;
            std::string name(k.str());
            Movie mv;
            mv.video = dir / fs::path((*m)["video"].value_or(std::string()));
            mv.frames = uint32_t((*m)["frames"].value_or(int64_t(0)));
            DefaultCrop(name, mv.crop_y, mv.crop_h);
            if (const toml::array* c = (*m)["crop"].as_array(); c && c->size() == 4) {
                mv.crop_y = uint32_t(c->get(1)->value_or(int64_t(mv.crop_y)));
                mv.crop_h = uint32_t(c->get(3)->value_or(int64_t(mv.crop_h)));
            }
            std::ifstream f(dir / (name + ".sig"), std::ios::binary);
            std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!fs::is_regular_file(mv.video, ec) || raw.size() < kSig || !mv.crop_h || mv.crop_y + mv.crop_h > kH) {
                LOGW("movies: %s has no video or thumbnails in %s", name.c_str(), dir.string().c_str());
                continue;
            }
            size_t n = raw.size() / kSig;
            mv.sig.resize(n * kSig);
            mv.flat.resize(n);
            float tmp[kSig];
            for (size_t i = 0; i < n; i++) {
                for (uint32_t j = 0; j < kSig; j++) tmp[j] = raw[i * kSig + j];
                mv.flat[i] = !Normalize(tmp, &mv.sig[i * kSig]);
            }
            if (mv.frames && mv.frames != n)
                LOGW("movies: %s has %zu thumbnails for its %u frames", name.c_str(), n, mv.frames);
            movies_[name] = std::move(mv);
        }
        LOGR("movies: %zu HD movies in %s", movies_.size(), dir.string().c_str());
    }

    // the title opened a file, on its thread: a movie's starts it
    void Opened(const std::string& path) {
        std::string p = path;
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::replace(p.begin(), p.end(), '/', '\\');
        size_t at = p.rfind("movie\\");
        if (at == std::string::npos || p.size() < at + 10 || p.compare(p.size() - 4, 4, ".xmv") != 0) return;
        std::string name = p.substr(at + 6, p.size() - at - 10);
        if (name.find('\\') != std::string::npos) return;
        std::lock_guard lock(m_);
        Summary();  // the movie before's
        name_ = name;
        LOGR("movies: %s starts%s", name.c_str(),
             !movies_.count(name) ? "" : g_hd_movies ? ", its HD version from the pack" : " (HD movies are off)");
        Start();
    }

    // the first draw of each frame the game draws, on the render thread. Counted by the game's frames, not the
    // presents: while the game is paused (the window in the background, the F10 menu) it draws none, and its movie
    // goes on from where it was
    void Frame(uint64_t) {
        std::lock_guard lock(m_);
        frames_++;
        if (active_ && frames_ > last_ + kIdleFrames) Stop();
    }

    // a texture the renderer uploads: the movie's frame gets the id of its HD frame
    uint64_t Replace(const gpu::TextureDesc& d, const uint8_t* guest) {
        if (!guest || !MovieTexture(d)) return 0;
        std::lock_guard lock(m_);
        if (!active_) return 0;
        uint32_t pitch = d.pitch ? d.pitch : kW * 4;
        alpha_ = guest[size_t(kH / 2) * pitch + (kW / 2) * 4 + 3];
        const Movie* mv = Current();
        if (!mv) {
            // no HD version: the game's frames, placed as movie_view says
            last_ = frames_;
            Seen(d.data);
            return 0;
        }
        int64_t f = Match(*mv, guest, pitch);
        if (f < 0) return 0;  // not a frame of the movie
        last_ = frames_;
        Seen(d.data);
        frame_ = uint32_t(f);
        if (!g_hd_movies) return 0;
        // made with the movie's first frame, which gives the alpha its quad blends with
        if (!decoder_) decoder_ = std::make_unique<Decoder>(mv->video, mv->crop_h, alpha_);
        if (decoder_->Failed()) return 0;
        if (++matched_ <= 2 || matched_ % 900 == 0)
            LOGR("movies: %s frame %u (alpha %u)", name_.c_str(), frame_, unsigned(alpha_));
        return kTag | (uint64_t(serial_) << 32) | (uint64_t(frame_) + 1);
    }

    // telemetry's "movies" (telemetry.cpp): the movie playing (its name, empty when none) and the frames drawn from its
    // HD video so far
    std::string Telemetry() {
        std::lock_guard lock(m_);
        return "{\"name\":\"" + (active_ ? name_ : std::string()) + "\",\"hd_frames\":" + std::to_string(made_) + "}";
    }

    std::shared_ptr<const gpu::Replacement> Replaced(uint64_t id) {
        std::lock_guard lock(m_);
        if (!decoder_ || uint32_t((id >> 32) & 0x3FFFFFFF) != serial_) return std::make_shared<gpu::Replacement>();
        auto r = decoder_->Frame(uint32_t(id & 0xFFFFFFFF) - 1);
        // for the log: frames drawn from the HD video, and those not decoded yet when first drawn
        if (r && id != made_id_) {
            made_id_ = id;
            made_++;
        } else if (!r && id != late_id_) {
            late_id_ = id;
            late_++;
        }
        return r;
    }

    // a draw of a texture: false but for the movie's frame, then how much larger it is drawn across (sx) and up and
    // down (sy) about the screen's middle. squeeze: the 4:3 frame's width on the screen, a part of the screen's; qx, qy
    // the game's pixels a texel of the frame takes across and down.
    //
    // The game reads the 640x480 frame's rows -17 to 497 over its 480 (as the PS2 version showed its 448 line movies
    // inside a TV's overscan), squashing the picture to 94% of its height. Fit and Fill draw it at its own shape
    // again: the PS2's, on which its 394 rows were 4:3's 640 wide
    bool Scale(const gpu::TextureDesc& d, float squeeze, float qx, float qy, float& sx, float& sy) {
        if (!MovieTexture(d) || g_movie_view == 0) return false;
        std::lock_guard lock(m_);
        if (!active_ || std::find(std::begin(textures_), std::end(textures_), d.data) == std::end(textures_)) return false;
        if (!told_view_) {
            told_view_ = true;
            LOGR("movies: the game draws its frame at %.4f pixels a texel across, %.4f down", qx, qy);
        }
        // the picture's rows: the HD frames have it in the middle; the game's frames where the movie has it, then as
        // much as lies as far above the middle as below
        uint32_t y, h;
        const Movie* mv = Current();
        if (mv) {
            y = mv->crop_y;
            h = mv->crop_h;
        } else {
            DefaultCrop(name_, y, h);
        }
        if (!(mv && g_hd_movies && decoder_ && !decoder_->Failed()))
            h = 2 * std::min(kH / 2 - std::min(y, kH / 2), std::max(y + h, kH / 2) - kH / 2);
        if (!h) return false;
        // across, as the game's pixels go: the picture as tall as the screen at its own shape, or the frame as wide
        float tall = float(kH) / float(h) / qx;
        float wide = 1.0f / (squeeze * qx);
        sx = g_movie_view == 2 ? std::max(tall, wide) : std::min(tall, wide);
        sy = sx * qx / qy;  // its own shape: as many pixels a texel down as across
        return true;
    }

private:
    const Movie* Current() const {
        auto it = movies_.find(name_);
        return it != movies_.end() ? &it->second : nullptr;
    }

    void Start() {
        decoder_.reset();
        active_ = true;
        last_ = frames_;
        frame_ = UINT32_MAX;
        matched_ = made_ = late_ = 0;
        std::fill(std::begin(textures_), std::end(textures_), 0u);
        serial_ = (serial_ + 1) & 0x3FFFFFFF;
    }

    void Summary() {
        if (matched_)
            LOGR("movies: %s: %u frames, %u of them drawn from the HD video, %u not decoded yet when first drawn",
                 name_.c_str(), matched_, made_, late_);
        matched_ = 0;
    }

    void Stop() {
        Summary();
        decoder_.reset();
        active_ = false;
        std::fill(std::begin(textures_), std::end(textures_), 0u);
        LOGR("movies: %s is over", name_.c_str());
    }

    // the textures the movie's frames went into: the game may take turns between a few
    void Seen(uint32_t data) {
        if (std::find(std::begin(textures_), std::end(textures_), data) != std::end(textures_)) return;
        for (uint32_t& t : textures_)
            if (!t) {
                t = data;
                return;
            }
        textures_[0] = data;
    }

    // the original's frame the game's frame is, -1 for none: near the one before (a little back, up to a second and
    // a half on), else anywhere; one all one grey (a fade's black), the next one
    int64_t Match(const Movie& mv, const uint8_t* guest, uint32_t pitch) {
        float sums[kSig] = {};
        for (uint32_t y = 0; y < kH; y++) {
            const uint8_t* row = guest + size_t(y) * pitch;
            float* s = &sums[(y / (kH / kSigH)) * kSigW];
            for (uint32_t x = 0; x < kW; x++) s[x / (kW / kSigW)] += float(row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]);
        }
        const float block = float((kW / kSigW) * (kH / kSigH)) * 3.0f;
        for (float& v : sums) v /= block;
        const uint32_t n = uint32_t(mv.flat.size());
        const uint32_t expect = frame_ == UINT32_MAX ? 0 : std::min(frame_ + 1, n - 1);
        float sig[kSig];
        if (!Normalize(sums, sig)) return expect;
        auto score = [&](uint32_t i) {
            if (mv.flat[i]) return -1.0f;
            const float* r = &mv.sig[size_t(i) * kSig];
            float c = 0;
            for (uint32_t j = 0; j < kSig; j++) c += sig[j] * r[j];
            return c;
        };
        // the best in [lo, hi): of those within a hair of the best (a still shot repeats its thumbnail), the nearest
        // the frame expected
        auto best_in = [&](uint32_t lo, uint32_t hi, float& best) {
            std::vector<float> c(hi - lo);
            best = -2.0f;
            for (uint32_t i = lo; i < hi; i++) best = std::max(best, c[i - lo] = score(i));
            uint32_t at = lo;
            int64_t gap = INT64_MAX;
            for (uint32_t i = lo; i < hi; i++)
                if (c[i - lo] >= best - 0.002f && std::llabs(int64_t(i) - int64_t(expect)) < gap) {
                    gap = std::llabs(int64_t(i) - int64_t(expect));
                    at = i;
                }
            return at;
        };
        float best;
        uint32_t at = best_in(expect > 2 ? expect - 2 : 0, std::min(n, expect + kFar + 1), best);
        if (best >= 0.9f) return at;
        float all;
        uint32_t any = best_in(0, n, all);
        if (all > 0.95f) return any;
        return best >= 0.6f ? int64_t(at) : -1;
    }

    std::map<std::string, Movie> movies_;
    std::mutex m_;
    std::string name_;
    bool active_ = false;
    uint64_t frames_ = 0, last_ = 0;  // the game's frames, and the one the movie's last frame came in
    uint32_t frame_ = UINT32_MAX, serial_ = 0, matched_ = 0, made_ = 0, late_ = 0;
    uint64_t made_id_ = 0, late_id_ = 0;
    bool told_view_ = false;
    uint32_t textures_[4] = {};
    uint8_t alpha_ = 255;
    std::unique_ptr<Decoder> decoder_;
};

Movies g_movies;

}  // namespace

void MoviesInit() {
    g_movies.Init();
    kernel::g_file_opened = [](const std::string& path) { g_movies.Opened(path); };
}
void MoviesFrame(uint64_t presents) { g_movies.Frame(presents); }
uint64_t MoviesReplace(const gpu::TextureDesc& d, const uint8_t* guest) { return g_movies.Replace(d, guest); }
bool MoviesOwn(uint64_t id) { return (id & kTag) != 0; }
std::string MoviesJson() { return g_movies.Telemetry(); }
std::shared_ptr<const gpu::Replacement> MoviesReplaced(uint64_t id) { return g_movies.Replaced(id); }
bool MoviesScale(const gpu::TextureDesc& d, float squeeze, float qx, float qy, float& sx, float& sy) {
    return g_movies.Scale(d, squeeze, qx, qy, sx, sy);
}
