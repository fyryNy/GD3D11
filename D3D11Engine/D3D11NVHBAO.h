#pragma once
#include "pch.h"

class GFSDK_SSAO_Context_D3D11;
class D3D11NVHBAO {
public:
    D3D11NVHBAO();
    ~D3D11NVHBAO();

    /** Initializes the library */
    XRESULT Init();

    /** Renders the HBAO-Effect onto the given RTV */
    XRESULT Render( ID3D11RenderTargetView* pOutputColorRTV );
private:
    struct ContextDeleter {
        void operator()( GFSDK_SSAO_Context_D3D11* context ) const noexcept;
    };
    using ContextPtr = std::unique_ptr<GFSDK_SSAO_Context_D3D11, ContextDeleter>;

    /** Nvidia HBAO+ context */
    ContextPtr AOContext;
};

