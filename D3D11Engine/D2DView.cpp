#include "pch.h"
#include "D2DView.h"
#include "D2DSubView.h"
#include "D2DEditorView.h"
#include "win32ClipboardWrapper.h"
#include "D2DSettingsDialog.h"
#include "Engine.h"
#include "GothicAPI.h"

#pragma comment(lib, "dwrite.lib")

const D2D1::ColorF GUI_Color1 = D2D1::ColorF( 0.4f, 0.4f, 0.4f, 1.0f );
const D2D1::ColorF GUI_Color2 = D2D1::ColorF( 0.2f, 0.2f, 0.2f, 1.0f );
const D2D1::ColorF GUI_Color3 = D2D1::ColorF( 0.1f, 0.1f, 0.1f, 1.0f );

const D2D1::ColorF ReflectColor1 = D2D1::ColorF( 0.616f * 0.8f, 0.87f * 0.8f, 0.98f * 0.8f, 1.0f );
const D2D1::ColorF ReflectColor2 = D2D1::ColorF( 0.22f, 0.54f, 0.725f, 1.0f );

const D2D1::ColorF DefBackgroundColor1 = D2D1::ColorF( 0.4f, 0.4f, 0.4f, 1.0f );
const D2D1::ColorF DefBackgroundColor2 = D2D1::ColorF( 0.0f, 0.0f, 0.0f, 1.0f );

D2DView::D2DView() {
    RenderTarget = nullptr;
    Brush = nullptr;
    MainSubView = nullptr;
    Factory = nullptr;

    GUIStyleLinearBrush = nullptr;
    RadialBrush = nullptr;
    LinearBrush = nullptr;
    LinearReflectBrushHigh = nullptr;
    LinearReflectBrush = nullptr;
    BackgroundBrush = nullptr;
    DefaultTextFormat = nullptr;
    TextFormatBig = nullptr;
    WriteFactory = nullptr;

    SettingsDialog = nullptr;
    EditorView = nullptr;
}

D2DView::~D2DView() {
    delete MainSubView;

    ReleaseRenderTargetResources();
    SAFE_RELEASE( TextFormatBig );
    SAFE_RELEASE( DefaultTextFormat );
    SAFE_RELEASE( WriteFactory );
    SAFE_RELEASE( RenderTarget );
    SAFE_RELEASE( Factory );
}

/** Inits this d2d-view */
XRESULT D2DView::Init( const INT2& initialResolution, ID3D11Texture2D* rendertarget ) {
    if ( !rendertarget ) return XR_FAILED;
    static const GUID IID_IDXGIVkInteropSurface = { 0x5546CF8C, 0x77E7, 0x4341, { 0xB0, 0x5D, 0x8D, 0x4D, 0x50, 0x00, 0xE7, 0x7D } };

    typedef HRESULT( WINAPI* PFN_D2D1CreateFactory )(D2D1_FACTORY_TYPE factory_type, REFIID riid, const D2D1_FACTORY_OPTIONS* factory_options, void** factory);
    PFN_D2D1CreateFactory D2D1CreateFactoryFunc;
    HMODULE D2D1Library = NULL;

    // Load d2d1 dll that works with dxvk if necessary
    IUnknown* dxgiVKInterop = nullptr;
    HRESULT result = rendertarget->QueryInterface( IID_IDXGIVkInteropSurface, reinterpret_cast<void**>( &dxgiVKInterop ) );
    if ( SUCCEEDED( result ) ) {
        D2D1Library = LoadLibraryA( "system\\GD3D11\\Bin\\D2D1.dll" );
        if ( !D2D1Library ) { // Just in-case
            D2D1Library = LoadLibraryA( "GD3D11\\Bin\\D2D1.dll" );
        }
        dxgiVKInterop->Release();
    }

    // Create factory
    D2D1_FACTORY_OPTIONS options;
    options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
#ifndef PUBLIC_RELEASE
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    if ( !D2D1Library ) {
        char dllBuf[MAX_PATH];
        GetSystemDirectoryA( dllBuf, MAX_PATH );
        strcat_s( dllBuf, MAX_PATH, "\\D2D1.dll" );

        D2D1Library = LoadLibraryA( dllBuf );
    }

    if ( D2D1Library ) {
        D2D1CreateFactoryFunc = reinterpret_cast<PFN_D2D1CreateFactory>(GetProcAddress( D2D1Library, "D2D1CreateFactory" ));
        if ( D2D1CreateFactoryFunc ) {
            D2D1CreateFactoryFunc( D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options, reinterpret_cast<void**>( &Factory ) );
        }
    }

    if ( !Factory ) {
        LogError() << "Failed to create ID2D1Factory!";
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<IDXGISurface2> dxgiBackbuffer;
    rendertarget->QueryInterface( dxgiBackbuffer.GetAddressOf() );

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat( DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED ) );
    if ( !dxgiBackbuffer || FAILED( Factory->CreateDxgiSurfaceRenderTarget( dxgiBackbuffer.Get(), props, &RenderTarget ) ) ) {
        // Copy downloadlink of the platform update to clipboard
        clipput( "https://www.microsoft.com/en-us/download/details.aspx?id=36805" );

        LogWarnBox() << "Failed to share D3D11-Surface with D2D. If you are running on Windows 7, you may just need to install "
            "the latest platform-update, which enables you to use DXGI 1.2.\n"
            "You can get it here: https://www.microsoft.com/en-us/download/details.aspx?id=36805 \n"
            "This will not crash the Renderer, but you will have to continue without editor-features.\n"
            "\nThe link has been copied to your clipboard.";
        PrepareResize();
        dxgiBackbuffer.Reset();
        Factory->Release();
        Factory = nullptr;
        return XR_FAILED;
    }

    if ( FAILED( InitResources() ) ) {
        PrepareResize();
        return XR_FAILED;
    }
    return XR_SUCCESS;
}

/** Creates brushes belonging to the current render target */
HRESULT D2DView::InitRenderTargetResources() {
    ReleaseRenderTargetResources();
    if ( !RenderTarget ) return E_FAIL;

    HRESULT hr = RenderTarget->CreateSolidColorBrush( D2D1::ColorF( D2D1::ColorF::White ), &Brush );
    if ( FAILED( hr ) ) return hr;

    // Create a linear gradient.
    D2D1_GRADIENT_STOP stops[4];
    stops[0].color = D2D1::ColorF( 0, 0, 0, 1 );
    stops[0].position = 0.0f;

    stops[1].color = D2D1::ColorF( 0, 0, 0, 0.3f );
    stops[1].position = 0.5f;

    stops[2].color = D2D1::ColorF( 0, 0, 0, 0.1f );
    stops[2].position = 0.7f;

    stops[3].color = D2D1::ColorF( 0, 0, 0, 0 );
    stops[3].position = 1.0f;

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> pGradientStops;
    hr = RenderTarget->CreateGradientStopCollection(
        stops,
        ARRAYSIZE( stops ),
        pGradientStops.ReleaseAndGetAddressOf()
    );
    if ( FAILED( hr ) ) return hr;

    if ( pGradientStops ) {
        hr = RenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F( 100, 0 ),
                D2D1::Point2F( 100, 200 )
            ),
            D2D1::BrushProperties(),
            pGradientStops.Get(),
            &LinearBrush
        );
        if ( FAILED( hr ) ) return hr;

        hr = RenderTarget->CreateRadialGradientBrush( D2D1::RadialGradientBrushProperties( D2D1::Point2F( 0, 0 ), D2D1::Point2F( 0, 0 ), 1, 1 ), pGradientStops.Get(), &RadialBrush );
        if ( FAILED( hr ) ) return hr;

        pGradientStops.Reset();
    }

    D2D1_GRADIENT_STOP GUISstops[3];
    GUISstops[0].color = D2D1::ColorF( GUI_Color1.r, GUI_Color1.g, GUI_Color1.b, GUI_Color1.a );
    GUISstops[0].position = 0.0f;

    GUISstops[1].color = D2D1::ColorF( GUI_Color2.r, GUI_Color2.g, GUI_Color2.b, GUI_Color2.a );
    GUISstops[1].position = 0.5f;

    GUISstops[2].color = D2D1::ColorF( GUI_Color3.r, GUI_Color3.g, GUI_Color3.b, GUI_Color3.a );
    GUISstops[2].position = 1.0f;

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> pGUI_S_GradientStops;
    hr = RenderTarget->CreateGradientStopCollection(
        GUISstops,
        ARRAYSIZE( GUISstops ),
        pGUI_S_GradientStops.ReleaseAndGetAddressOf()
    );
    if ( FAILED( hr ) ) return hr;

    if ( pGUI_S_GradientStops ) {
        hr = RenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F( 100, 0 ),
                D2D1::Point2F( 100, 200 )
            ),
            D2D1::BrushProperties(),
            pGUI_S_GradientStops.Get(),
            &GUIStyleLinearBrush
        );
        if ( FAILED( hr ) ) return hr;

        pGUI_S_GradientStops.Reset();
    }

    D2D1_GRADIENT_STOP Reflectstops[4];
    Reflectstops[0].color = D2D1::ColorF( ReflectColor1.r, ReflectColor1.g, ReflectColor1.b, ReflectColor1.a );
    Reflectstops[0].position = 0.0f;

    Reflectstops[1].color = D2D1::ColorF( ReflectColor1.r, ReflectColor1.g, ReflectColor1.b, ReflectColor1.a );
    Reflectstops[1].position = 0.50f;

    Reflectstops[2].color = D2D1::ColorF( ReflectColor2.r, ReflectColor2.g, ReflectColor2.b, ReflectColor2.a );
    Reflectstops[2].position = 0.501f;

    Reflectstops[3].color = D2D1::ColorF( ReflectColor2.r, ReflectColor2.g, ReflectColor2.b, ReflectColor2.a );
    Reflectstops[3].position = 1.0f;

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> pReflectGradientStops;
    hr = RenderTarget->CreateGradientStopCollection(
        Reflectstops,
        ARRAYSIZE( Reflectstops ),
        pReflectGradientStops.ReleaseAndGetAddressOf()
    );
    if ( FAILED( hr ) ) return hr;

    if ( pReflectGradientStops ) {
        hr = RenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F( 100, 0 ),
                D2D1::Point2F( 100, 200 )
            ),
            D2D1::BrushProperties(),
            pReflectGradientStops.Get(),
            &LinearReflectBrush
        );
        if ( FAILED( hr ) ) return hr;

        pReflectGradientStops.Reset();
    }

    Reflectstops[0].color = D2D1::ColorF( ReflectColor2.r, ReflectColor2.g, ReflectColor2.b, ReflectColor2.a );
    Reflectstops[0].position = 0.0f;

    Reflectstops[1].color = D2D1::ColorF( ReflectColor1.r, ReflectColor1.g, ReflectColor1.b, ReflectColor1.a );
    Reflectstops[1].position = 0.30f;

    Reflectstops[2].color = D2D1::ColorF( ReflectColor2.r, ReflectColor2.g, ReflectColor2.b, ReflectColor2.a );
    Reflectstops[2].position = 0.301f;

    Reflectstops[3].color = D2D1::ColorF( ReflectColor2.r, ReflectColor2.g, ReflectColor2.b, ReflectColor2.a );
    Reflectstops[3].position = 1.0f;

    hr = RenderTarget->CreateGradientStopCollection(
        Reflectstops,
        ARRAYSIZE( Reflectstops ),
        pReflectGradientStops.ReleaseAndGetAddressOf()
    );
    if ( FAILED( hr ) ) return hr;

    if ( pReflectGradientStops ) {
        hr = RenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F( 100, 0 ),
                D2D1::Point2F( 100, 200 )
            ),
            D2D1::BrushProperties(),
            pReflectGradientStops.Get(),
            &LinearReflectBrushHigh
        );
        if ( FAILED( hr ) ) return hr;

        pReflectGradientStops.Reset();
    }

    D2D1_GRADIENT_STOP bgrstops[2];
    bgrstops[0].color = DefBackgroundColor1;
    bgrstops[0].position = -0.5f;

    bgrstops[1].color = DefBackgroundColor2;
    bgrstops[1].position = 1.5f;

    hr = RenderTarget->CreateGradientStopCollection(
        bgrstops,
        ARRAYSIZE( bgrstops ),
        pGradientStops.ReleaseAndGetAddressOf()
    );
    if ( FAILED( hr ) ) return hr;

    if ( pGradientStops ) {
        hr = RenderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F( 100, 0 ),
                D2D1::Point2F( 100, 200 )
            ),
            D2D1::BrushProperties(),
            pGradientStops.Get(),
            &BackgroundBrush
        );
        if ( FAILED( hr ) ) return hr;

        pGradientStops.Reset();
    }

    return Brush && LinearBrush && RadialBrush && GUIStyleLinearBrush
        && LinearReflectBrush && LinearReflectBrushHigh && BackgroundBrush ? S_OK : E_FAIL;
}

/** Releases brushes before their render target */
void D2DView::ReleaseRenderTargetResources() {
    SAFE_RELEASE( LinearReflectBrush );
    SAFE_RELEASE( LinearReflectBrushHigh );
    SAFE_RELEASE( Brush );
    SAFE_RELEASE( GUIStyleLinearBrush );
    SAFE_RELEASE( LinearBrush );
    SAFE_RELEASE( RadialBrush );
    SAFE_RELEASE( BackgroundBrush );
}

/** Creates the view's persistent controls and text resources */
HRESULT D2DView::InitResources() {
    HRESULT hr = InitRenderTargetResources();
    if ( FAILED( hr ) ) return hr;

    hr = DWriteCreateFactory( DWRITE_FACTORY_TYPE_SHARED, __uuidof(WriteFactory), reinterpret_cast<IUnknown**>(&WriteFactory) );
    if ( FAILED( hr ) || !WriteFactory ) return FAILED( hr ) ? hr : E_FAIL;

    // create the DWrite text format
    hr = WriteFactory->CreateTextFormat(
        L"Arial",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14,
        L"",
        &DefaultTextFormat );
    if ( FAILED( hr ) || !DefaultTextFormat ) return FAILED( hr ) ? hr : E_FAIL;

    DefaultTextFormat->SetTextAlignment( DWRITE_TEXT_ALIGNMENT_CENTER );
    DefaultTextFormat->SetParagraphAlignment( DWRITE_PARAGRAPH_ALIGNMENT_CENTER );

    hr = WriteFactory->CreateTextFormat(
        L"Arial",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        16,
        L"",
        &TextFormatBig );
    if ( FAILED( hr ) || !TextFormatBig ) return FAILED( hr ) ? hr : E_FAIL;

    MainSubView = new D2DSubView( this, nullptr );
    MainSubView->SetRect( D2D1::RectF( 0, 0, RenderTarget->GetSize().width, RenderTarget->GetSize().height ) );

    EditorView = new D2DEditorView( this, MainSubView );
    EditorView->SetRect( D2D1::RectF( 0, 0, RenderTarget->GetSize().width, RenderTarget->GetSize().height ) );

    SettingsDialog = new D2DSettingsDialog( this, MainSubView );
    SettingsDialog->SetHidden( true );

    return XR_SUCCESS;
}

/** Draws the view */
void D2DView::Render( float deltaTime ) {
    if ( !RenderTarget || !EditorView || !MainSubView ) return;
    if ( !EditorView->IsHidden() ) {
        RenderTarget->BeginDraw();

        // Draw sub-views
        MainSubView->Draw( D2D1::RectF( 0, 0, RenderTarget->GetSize().width, RenderTarget->GetSize().height ), deltaTime );

        RenderTarget->EndDraw();
    }
}

/** Updates the view */
void D2DView::Update( float deltaTime ) {
    CheckDeadMessageBoxes();

    MainSubView->Update( deltaTime );
}

/** Releases all resources needed to resize this view */
XRESULT D2DView::PrepareResize() {
    if ( MainSubView ) MainSubView->PrepareResize();
    ReleaseRenderTargetResources();
    SAFE_RELEASE( RenderTarget );

    return XR_SUCCESS;
}

/** Resizes this d2d-view */
XRESULT D2DView::Resize( const INT2& initialResolution, ID3D11Texture2D* rendertarget ) {
    PrepareResize();
    if ( !Factory || !rendertarget || !MainSubView || !EditorView ) return XR_FAILED;

    Microsoft::WRL::ComPtr<IDXGISurface2> dxgiBackbuffer;
    HRESULT hr = rendertarget->QueryInterface( dxgiBackbuffer.GetAddressOf() );

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties( D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat( DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED ) );
    if ( FAILED( hr ) || !dxgiBackbuffer.Get() ) {
        return XR_FAILED;
    }
    hr = Factory->CreateDxgiSurfaceRenderTarget( dxgiBackbuffer.Get(), props, &RenderTarget );
    if ( FAILED( hr ) || !RenderTarget || FAILED( InitRenderTargetResources() ) ) {
        PrepareResize();
        return XR_FAILED;
    }

    MainSubView->SetRect( D2D1::RectF( 0, 0, RenderTarget->GetSize().width, RenderTarget->GetSize().height ) );
    EditorView->SetRect( D2D1::RectF( 0, 0, RenderTarget->GetSize().width, RenderTarget->GetSize().height ) );

    return XR_SUCCESS;
}

void D2DView::ShrinkRect( D2D1_RECT_F* Rect, float Offset ) {
    Rect->left += Offset;
    Rect->top += Offset;

    Rect->right -= Offset;
    Rect->bottom -= Offset;
}

void D2DView::DrawSmoothShadow( const D2D1_RECT_F* Rectangle,
    float Size,
    float Opacity,
    bool bDoNotShrink,
    float RectShrink,
    bool bLeft,
    bool bTop,
    bool bRight,
    bool bBottom ) {

    ID2D1RadialGradientBrush* RadBrush = RadialBrush;
    ID2D1LinearGradientBrush* LinBrush = LinearBrush;

    D2D1_RECT_F TempRect = *Rectangle;

    if ( Size > 0 && !bDoNotShrink ) {
        ShrinkRect( &TempRect, RectShrink );
    }


    D2D1_RECT_F Rect = TempRect;



    LinBrush->SetOpacity( Opacity );
    RadBrush->SetOpacity( Opacity );

    //Draw the edges (left edge)
    if ( bLeft ) {
        Rect.right = Rect.left;
        Rect.left -= Size;
        LinBrush->SetEndPoint( D2D1::Point2F( Rect.left, 0 ) );
        LinBrush->SetStartPoint( D2D1::Point2F( Rect.right, 0 ) );
        RenderTarget->FillRectangle( &Rect, LinBrush );
    }

    //Right edge
    if ( bRight ) {
        Rect = TempRect;
        Rect.left = Rect.right;
        Rect.right += Size;
        LinBrush->SetEndPoint( D2D1::Point2F( Rect.right, 0 ) );
        LinBrush->SetStartPoint( D2D1::Point2F( Rect.left, 0 ) );
        RenderTarget->FillRectangle( &Rect, LinBrush );
    }
    //Upper edge
    if ( bTop ) {
        Rect = TempRect;
        Rect.bottom = Rect.top;
        Rect.top -= Size;
        LinBrush->SetEndPoint( D2D1::Point2F( 0, Rect.top ) );
        LinBrush->SetStartPoint( D2D1::Point2F( 0, Rect.bottom ) );
        RenderTarget->FillRectangle( &Rect, LinBrush );
    }
    //Bottom edge
    if ( bBottom ) {
        Rect = TempRect;
        Rect.top = Rect.bottom;
        Rect.bottom += Size;
        LinBrush->SetEndPoint( D2D1::Point2F( 0, Rect.bottom ) );
        LinBrush->SetStartPoint( D2D1::Point2F( 0, Rect.top ) );
        RenderTarget->FillRectangle( &Rect, LinBrush );
    }
    RadBrush->SetRadiusX( Size );
    RadBrush->SetRadiusY( Size );
    RadialBrush->SetOpacity( Opacity );

    if ( Size > 0 ) {
        if ( bLeft && bTop ) {
            //Draw upper left corner
            Rect = TempRect;
            Rect.bottom = Rect.top;
            Rect.right = Rect.left;
            Rect.left -= Size;
            Rect.top -= Size;

            RadBrush->SetCenter( D2D1::Point2F( Rect.right, Rect.bottom ) );
            RenderTarget->FillRectangle( Rect, RadBrush );
        }

        if ( bRight && bTop ) {
            //Draw upper right corner
            Rect = TempRect;
            Rect.bottom = Rect.top;
            Rect.left = Rect.right;
            Rect.right += Size;
            Rect.top -= Size;

            RadBrush->SetCenter( D2D1::Point2F( Rect.left, Rect.bottom ) );
            RenderTarget->FillRectangle( Rect, RadBrush );
        }

        if ( bRight && bBottom ) {
            //Draw bottom right corner
            Rect = TempRect;
            Rect.top = Rect.bottom;
            Rect.left = Rect.right;
            Rect.right += Size;
            Rect.bottom += Size;

            RadBrush->SetCenter( D2D1::Point2F( Rect.left, Rect.top ) );
            RenderTarget->FillRectangle( Rect, RadBrush );
        }

        if ( bLeft && bBottom ) {
            //Draw bottom left corner
            Rect = TempRect;
            Rect.top = Rect.bottom;
            Rect.right = Rect.left;
            Rect.left -= Size;
            Rect.bottom += Size;

            RadBrush->SetCenter( D2D1::Point2F( Rect.right, Rect.top ) );
            RenderTarget->FillRectangle( Rect, RadBrush );
        }
    }
}

/** Processes a window-message. Return false to stop the message from going to children */
bool D2DView::OnWindowMessage( HWND hWnd, unsigned int msg, WPARAM wParam, LPARAM lParam ) {
    if ( !MainSubView )
        return false;

    return MainSubView->OnWindowMessage( hWnd, msg, wParam, lParam, D2D1::RectF( 0, 0, RenderTarget->GetSize().width, RenderTarget->GetSize().height ) );
}

float D2DView::GetLabelTextWidth( IDWriteTextLayout* layout, size_t length ) {
    DWRITE_CLUSTER_METRICS* m = new DWRITE_CLUSTER_METRICS[length];
    UINT32 lc;

    if ( !layout ) {
        return 0.0f;
    }

    layout->GetClusterMetrics( m, length, &lc );

    float width = 0.0f;

    for ( size_t i = 0; i < length; i++ ) {
        width += m[i].width;
    }
    delete[] m;

    return width;
}

float D2DView::GetTextHeight( IDWriteTextLayout* layout, size_t length ) {
    unsigned int i = 0;
    float y = 0;
    for ( i = 0; i < length; i++ ) {
        float ty;

        layout->GetFontSize( i, &ty );
        if ( ty > y ) { y = ty; }
    }

    return y;
}

/** Returns the current cursor position */
POINT D2DView::GetCursorPosition() {
    return Engine::GAPI->GetCursorPosition();
}

/** Adds a message box */
void D2DView::AddMessageBox( const std::string& caption, const std::string& message, D2DMessageBoxCallback callback, void* userdata, ED2D_MB_TYPE type ) {
    D2DMessageBox* box = new D2DMessageBox( this, MainSubView, type );

    box->SetCallback( callback, userdata );
    box->SetHeaderText( caption );
    box->SetMessage( message );

    // Free the mouse
    Engine::GAPI->SetEnableGothicInput( false );

    MessageBoxes.push_back( box );
}

/** Checks dead message boxes and removes them */
void D2DView::CheckDeadMessageBoxes() {
    // FIXME
    for ( auto it = MessageBoxes.begin(); it != MessageBoxes.end();) {
        // Delete the messagebox if it is hidden
        if ( (*it)->IsHidden() ) {
            MainSubView->DeleteChild( *it );

            it = MessageBoxes.erase( it );

            // Capture mouse again
            Engine::GAPI->SetEnableGothicInput( true );
        } else {
            ++it;
        }
    }
}
