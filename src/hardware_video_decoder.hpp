#pragma once

#include "Limelight.h"

#include <cstdint>
#include <memory>
#include <string>

namespace cv { class Mat; }

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace vanta
{
    struct DecodedVideoStats
    {
        int width{};
        int height{};
        int configuredFps{};
        double receivedFramesPerSecond{};
        double decodedFramesPerSecond{};
        std::uint64_t receivedFrames{};
        std::uint64_t submittedFrames{};
        std::uint64_t decodedFrames{};
        std::uint64_t droppedFrames{};
        bool hardwareActive{};
        std::string status;
    };

    // A small, H.264-first hardware decoder for the Moonlight common-c
    // callback interface. Compressed frames are copied out of the networking
    // callback immediately and decoded on a dedicated MTA thread. The output
    // is a D3D11 SRV on Vanta's existing device, avoiding a CPU readback.
    class HardwareVideoDecoder
    {
    public:
        HardwareVideoDecoder();
        ~HardwareVideoDecoder();

        HardwareVideoDecoder(const HardwareVideoDecoder&) = delete;
        HardwareVideoDecoder& operator=(const HardwareVideoDecoder&) = delete;

        [[nodiscard]] bool Initialize(ID3D11Device* device, std::string& error);
        void Shutdown();

        [[nodiscard]] int Configure(int videoFormat, int width, int height, int redrawRate, std::string& error);
        void Start();
        void Stop();
        [[nodiscard]] int Submit(PDECODE_UNIT decodeUnit);

        // The caller owns one COM reference and must release it after ImGui has
        // rendered the matching frame. It is always a BGRA D3D11 SRV.
        [[nodiscard]] ID3D11ShaderResourceView* AcquireFrameView() const;
        // Copies a newly presented BGRA frame into CPU memory for Vanta's
        // existing OpenCV capture consumers. This is intentionally on-demand:
        // the receiver page itself remains a zero-readback GPU preview.
        [[nodiscard]] bool CopyLatestFrameToBgra(
            std::uint64_t afterSequence,
            cv::Mat& output,
            std::uint64_t& sequence);
        [[nodiscard]] DecodedVideoStats GetStats() const;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> implementation_;
    };
}
