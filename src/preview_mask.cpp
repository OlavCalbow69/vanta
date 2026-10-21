#include "preview_mask.hpp"
#include "logger.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <map>
#include <string>
using Microsoft::WRL::ComPtr;
namespace
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11PixelShader> shader;
    ComPtr<ID3D11SamplerState> sampler;
    struct MaskTexture
    {
        cv::Mat immutableMask;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
    };
    std::map<std::string,MaskTexture> textures;
    void BindMask(const ImDrawList*,const ImDrawCmd*)
    {context->PSSetShader(shader.Get(),nullptr,0);auto* point=sampler.Get();context->PSSetSamplers(0,1,&point);}
}
namespace vanta
{
    void SetMaskPreviewDevice(ID3D11Device* selectedDevice,ID3D11DeviceContext* selectedContext)
    {
        textures.clear();shader.Reset();sampler.Reset();context=selectedContext;device=selectedDevice;
        if(!device)return;
        constexpr char source[]=R"(
            struct Input{float4 position:SV_POSITION;float4 color:COLOR0;float2 uv:TEXCOORD0;};
            SamplerState sampler0:register(s0);Texture2D mask0:register(t0);
            float4 main(Input input):SV_Target{return float4(input.color.rgb,input.color.a*mask0.Sample(sampler0,input.uv).r);}
        )";
        ComPtr<ID3DBlob> code,errors;
        HRESULT hr=D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"main","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(SUCCEEDED(hr))hr=device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader);
        D3D11_SAMPLER_DESC sampling{};sampling.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampling.AddressU=sampling.AddressV=sampling.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampling.MaxLOD=D3D11_FLOAT32_MAX;
        if(SUCCEEDED(hr))hr=device->CreateSamplerState(&sampling,&sampler);
        if(FAILED(hr))log::Warning("Mask preview shader unavailable: 0x%08lX",static_cast<unsigned long>(hr));
    }
    void DrawPreviewMask(ImDrawList* draw,const char* key,const cv::Mat& mask,const ImVec2& minimum,const ImVec2& maximum,ImU32 color)
    {
        if(!device || !shader || !sampler || mask.empty() || mask.type()!=CV_8UC1)return;
        auto& cached=textures[key];
        if(!cached.texture || cached.immutableMask.size()!=mask.size())
        {
            cached={};D3D11_TEXTURE2D_DESC desc{};desc.Width=UINT(mask.cols);desc.Height=UINT(mask.rows);
            desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8_UNORM;
            desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            if(FAILED(device->CreateTexture2D(&desc,nullptr,&cached.texture)) ||
                FAILED(device->CreateShaderResourceView(cached.texture.Get(),nullptr,&cached.view)))return;
        }
        if(cached.immutableMask.data!=mask.data)
        {context->UpdateSubresource(cached.texture.Get(),0,nullptr,mask.data,UINT(mask.step),0);cached.immutableMask=mask;}
        draw->AddCallback(BindMask,nullptr);
        draw->AddImage(reinterpret_cast<ImTextureID>(cached.view.Get()),minimum,maximum,{0,0},{1,1},color);
        draw->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
    }
}
