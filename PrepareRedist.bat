@ECHO OFF
SET "TARGET_DIR=%1"


CD "%~dp0"

mkdir "%TARGET_DIR%\GD3D11\shaders"
Xcopy "D3D11Engine\Shaders\*" "%TARGET_DIR%\GD3D11\shaders" /y /s

mkdir "%TARGET_DIR%\GD3D11\shaders\CSFFT"
COPY /y "D3D11Engine\CSFFT\*.hlsl" "%TARGET_DIR%\GD3D11\shaders\CSFFT"

Xcopy "blobs\data" "%TARGET_DIR%\GD3D11\data\" /y /s
Xcopy "blobs\Meshes" "%TARGET_DIR%\GD3D11\Meshes\" /y /s
Xcopy "blobs\Textures" "%TARGET_DIR%\GD3D11\Textures\" /y /s


Xcopy "blobs\libs\*" "%TARGET_DIR%\" /y /s

mkdir "%TARGET_DIR%\GD3D11\Bin"
COPY /y "packages\Microsoft.Direct3D.DXC.1.8.2502.7\build\native\bin\x86\dxcompiler.dll" "%TARGET_DIR%\GD3D11\Bin\"
COPY /y "packages\Microsoft.Direct3D.DXC.1.8.2502.7\build\native\bin\x86\dxil.dll" "%TARGET_DIR%\GD3D11\Bin\"
COPY /y "packages\Microsoft.Direct3D.DXC.1.8.2502.7\LICENSE-MS.txt" "%TARGET_DIR%\GD3D11\Bin\LICENSE-DXC-MS.txt"
COPY /y "packages\Microsoft.Direct3D.DXC.1.8.2502.7\LICENSE-LLVM.txt" "%TARGET_DIR%\GD3D11\Bin\LICENSE-DXC-LLVM.txt"
