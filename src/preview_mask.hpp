#pragma once
#include <opencv2/core.hpp>
#include "imgui.h"
struct ID3D11Device;
struct ID3D11DeviceContext;
namespace vanta
{
    void SetMaskPreviewDevice(ID3D11Device* device,ID3D11DeviceContext* context);
    void DrawPreviewMask(ImDrawList* draw,const char* key,const cv::Mat& mask,
        const ImVec2& minimum,const ImVec2& maximum,ImU32 color);
}
