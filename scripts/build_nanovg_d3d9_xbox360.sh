#!/usr/bin/env bash
set -euo pipefail

# XDK fxc emits xvs_3_0/xps_3_0 microcode when given vs_3_0/ps_3_0.
# Set XDK_FXC to the XDK fxc executable (WSL accepts a mounted .exe path).
: "${XDK_FXC:?Set XDK_FXC to the Xbox 360 XDK fxc executable}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
shader_dir="$script_dir/../library/borealis/library/lib/extern/nanovg_d3d/src"
cd "$shader_dir"

"$XDK_FXC" /D XBOX360=1 /Fh D3D9PixelShader.h /Vn g_ps30_D3D9PixelShader_Main \
    /T ps_3_0 /E D3D9PixelShader_Main D3D9PixelShader.hlsl
"$XDK_FXC" /D XBOX360=1 /Fh D3D9PixelShaderAA.h /Vn g_ps30_D3D9PixelShaderAA_Main \
    /T ps_3_0 /E D3D9PixelShaderAA_Main D3D9PixelShaderAA.hlsl
"$XDK_FXC" /D XBOX360=1 /Fh D3D9VertexShader.h /Vn g_vs30_D3D9VertexShader_Main \
    /T vs_3_0 /E D3D9VertexShader_Main D3D9VertexShader.hlsl

echo "Generated Xenon D3D9 shader headers in $shader_dir"
