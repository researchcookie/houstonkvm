#include "video/video_encoder.h"

#include <turbojpeg.h>
#include <wels/codec_api.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace houston_kvm {

namespace {
void rgbToI420(const uint8_t* rgb, int width, int height,
               uint8_t* y, uint8_t* u, uint8_t* v, size_t chromaStride) {
    for (int j = 0; j < height; ++j) {
        const uint8_t* row = rgb + static_cast<size_t>(j) * width * 3;
        uint8_t* yRow = y + static_cast<size_t>(j) * width;
        for (int i = 0; i < width; ++i) {
            size_t idx = static_cast<size_t>(i) * 3;
            int r = row[idx], g = row[idx + 1], b = row[idx + 2];
            yRow[i] = static_cast<uint8_t>(
                std::clamp((77 * r + 150 * g + 29 * b) >> 8, 0, 255));
        }
    }
    for (int j = 0; j < height; j += 2) {
        const uint8_t* row = rgb + static_cast<size_t>(j) * width * 3;
        uint8_t* uRow = u + (j / 2) * chromaStride;
        uint8_t* vRow = v + (j / 2) * chromaStride;
        for (int i = 0; i < width; i += 2) {
            size_t idx = static_cast<size_t>(i) * 3;
            int r = row[idx], g = row[idx + 1], b = row[idx + 2];
            uRow[i / 2] = static_cast<uint8_t>(
                std::clamp(((-43 * r - 84 * g + 127 * b) >> 8) + 128, 0, 255));
            vRow[i / 2] = static_cast<uint8_t>(
                std::clamp(((127 * r - 106 * g - 21 * b) >> 8) + 128, 0, 255));
        }
    }
}

// Software H.264 (Cisco's OpenH264), after decoding the JPEG with
// libjpeg-turbo.
struct OpenH264Encoder final : VideoEncoder {
    tjhandle     tj      = nullptr;
    ISVCEncoder* encoder = nullptr;
    uint32_t     width   = 0;
    uint32_t     height  = 0;
    uint32_t     fps;
    uint32_t     bitrateKbps;
    int64_t      frameIndex = 0;
    std::vector<uint8_t> yuvBuf;
    std::vector<uint8_t> nalBuf; // scratch buffer for concatenating encoded layers

    OpenH264Encoder(uint32_t fps_, uint32_t bitrateKbps_)
        : tj(tjInitDecompress()), fps(fps_), bitrateKbps(bitrateKbps_) {
        if (!tj) throw std::runtime_error("VideoEncoder: tjInitDecompress failed");
    }

    ~OpenH264Encoder() override {
        if (encoder) {
            encoder->Uninitialize();
            WelsDestroySVCEncoder(encoder);
        }
        if (tj) tjDestroy(tj);
    }

    void ensureEncoder(uint32_t w, uint32_t h) {
        if (encoder && w == width && h == height) return;

        if (encoder) {
            encoder->Uninitialize();
            WelsDestroySVCEncoder(encoder);
            encoder = nullptr;
        }
        width  = w;
        height = h;

        if (WelsCreateSVCEncoder(&encoder) != 0 || !encoder)
            throw std::runtime_error("VideoEncoder: WelsCreateSVCEncoder failed");

        SEncParamExt param;
        encoder->GetDefaultParams(&param);
        param.iUsageType             = CAMERA_VIDEO_REAL_TIME;
        param.iPicWidth               = static_cast<int>(width);
        param.iPicHeight              = static_cast<int>(height);
        param.iTargetBitrate          = static_cast<int>(bitrateKbps) * 1000;
        param.iRCMode                 = RC_BITRATE_MODE;
        param.fMaxFrameRate           = static_cast<float>(fps);
        param.iTemporalLayerNum       = 1;
        param.iSpatialLayerNum        = 1;
        param.bEnableDenoise          = false;
        param.bEnableFrameSkip        = false;
        param.bEnableLongTermReference = false;
        // A short forced-keyframe period fights RC_BITRATE_MODE's rate
        // control: every period, quality snaps back to a full I-frame,
        // then degrades via quantization until the next forced reset —
        // visible as a periodic quality "pulse", worst on high-spatial-
        // frequency content like terminal text. New subscribers and
        // packet-loss recovery already get an on-demand keyframe via RTCP
        // PLI (see publisher.cpp's forceKeyframe() call), so periodic
        // forcing isn't needed for correctness — this is just a distant
        // safety net bounding worst-case drift if a PLI is ever missed.
        param.uiIntraPeriod           = fps * 10;
        param.eSpsPpsIdStrategy       = CONSTANT_ID;
        param.sSpatialLayers[0].iVideoWidth   = static_cast<int>(width);
        param.sSpatialLayers[0].iVideoHeight  = static_cast<int>(height);
        param.sSpatialLayers[0].fFrameRate    = static_cast<float>(fps);
        param.sSpatialLayers[0].iSpatialBitrate = param.iTargetBitrate;
        param.sSpatialLayers[0].uiProfileIdc  = PRO_BASELINE;

        if (encoder->InitializeExt(&param) != cmResultSuccess)
            throw std::runtime_error("VideoEncoder: openh264 InitializeExt failed");

        int videoFormat = videoFormatI420;
        encoder->SetOption(ENCODER_OPTION_DATAFORMAT, &videoFormat);

        size_t cw = (width + 1) / 2, ch = (height + 1) / 2;
        yuvBuf.assign(static_cast<size_t>(width) * height + 2 * cw * ch, 0);
    }

    const char* name() const override { return "OpenH264"; }
    void encode(const uint8_t* jpegData, size_t jpegSize, const EncodedCb& onNal) override;
    void forceKeyframe() override;
};

void OpenH264Encoder::encode(const uint8_t* jpegData, size_t jpegSize,
                             const EncodedCb& onNal) {
    int width = 0, height = 0, subsamp = 0, colorspace = 0;
    if (tjDecompressHeader3(tj, const_cast<unsigned char*>(jpegData),
                             static_cast<unsigned long>(jpegSize),
                             &width, &height, &subsamp, &colorspace) != 0)
        return; // malformed frame, drop
    if (width <= 0 || height <= 0) return;

    ensureEncoder(static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    size_t ySize = static_cast<size_t>(width) * height;
    size_t cw = (static_cast<size_t>(width) + 1) / 2;
    size_t ch = (static_cast<size_t>(height) + 1) / 2;
    size_t cSize = cw * ch;
    uint8_t* yPlane = yuvBuf.data();
    uint8_t* uPlane = yPlane + ySize;
    uint8_t* vPlane = uPlane + cSize;

    bool ok = false;
    if (subsamp == TJSAMP_420) {
        unsigned char* planes[3] = {yPlane, uPlane, vPlane};
        int strides[3] = {width, static_cast<int>(cw), static_cast<int>(cw)};
        ok = tjDecompressToYUVPlanes(
                 tj, const_cast<unsigned char*>(jpegData),
                 static_cast<unsigned long>(jpegSize),
                 planes, width, strides, height, 0) == 0;
    }
    if (!ok) {
        std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
        if (tjDecompress2(tj, const_cast<unsigned char*>(jpegData),
                           static_cast<unsigned long>(jpegSize),
                           rgb.data(), width, 0, height, TJPF_RGB, 0) != 0)
            return; // malformed frame, drop
        rgbToI420(rgb.data(), width, height, yPlane, uPlane, vPlane, cw);
    }

    SSourcePicture pic;
    std::memset(&pic, 0, sizeof(pic));
    pic.iPicWidth   = width;
    pic.iPicHeight  = height;
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0]  = width;
    pic.iStride[1]  = static_cast<int>(cw);
    pic.iStride[2]  = static_cast<int>(cw);
    pic.pData[0]    = yPlane;
    pic.pData[1]    = uPlane;
    pic.pData[2]    = vPlane;
    pic.uiTimeStamp = frameIndex * 1000 / static_cast<int64_t>(fps);
    ++frameIndex;

    SFrameBSInfo bsInfo;
    std::memset(&bsInfo, 0, sizeof(bsInfo));
    if (encoder->EncodeFrame(&pic, &bsInfo) != cmResultSuccess) return;
    if (bsInfo.eFrameType == videoFrameTypeSkip) return;

    nalBuf.clear();
    for (int layer = 0; layer < bsInfo.iLayerNum; ++layer) {
        const SLayerBSInfo& li = bsInfo.sLayerInfo[layer];
        int size = 0;
        for (int n = 0; n < li.iNalCount; ++n) size += li.pNalLengthInByte[n];
        if (size > 0 && li.pBsBuf)
            nalBuf.insert(nalBuf.end(), li.pBsBuf, li.pBsBuf + size);
    }
    if (!nalBuf.empty()) onNal(nalBuf.data(), nalBuf.size());
}

void OpenH264Encoder::forceKeyframe() {
    if (encoder) encoder->ForceIntraFrame(true);
}

} // namespace

std::unique_ptr<VideoEncoder> VideoEncoder::create(uint32_t fps, uint32_t bitrateKbps) {
    return std::make_unique<OpenH264Encoder>(fps, bitrateKbps);
}

}
