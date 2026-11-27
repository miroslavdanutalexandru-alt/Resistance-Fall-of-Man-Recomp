/*
 * D3D12 feature-check structs and enum values newer than some Windows SDKs.
 *
 * The renderer asks the driver at run time for OPTIONS8 (unaligned BC
 * textures), OPTIONS13 (alpha blend factor) and OPTIONS14 (independent
 * back-face stencil masks), and builds DEPTH_STENCIL2 pipeline subobjects.
 * OPTIONS14 / DEPTH_STENCIL_DESC2 first appear in the Windows 11 24H2 SDK
 * (10.0.26100), OPTIONS13 / ALPHA_FACTOR in 10.0.22621. Building against an
 * older SDK must not drop the features: the driver, not the SDK, decides.
 *
 * So these are local copies under their own names, with the exact layout and
 * values of d3d12.h 10.0.26100 (they are part of the D3D12 ABI and never
 * change). Every SDK gets the same code; where the SDK has the originals the
 * static asserts below check the copies against them.
 */
#ifndef RSX_D3D12_COMPAT_H
#define RSX_D3D12_COMPAT_H

#include <stddef.h>
#include <d3d12.h>

/* Before 10.0.20348 the C declarations of ID3D12 methods that return a struct
 * (GetCPUDescriptorHandleForHeapStart, ID3D12Resource::GetDesc, ...) had the
 * wrong calling convention; d3d12.h has defined D3D12_SDK_VERSION since 20348. */
#ifndef D3D12_SDK_VERSION
#error "This Windows SDK is too old for the D3D12 renderer: install Windows SDK 10.0.20348 or newer (Visual Studio Installer > Individual components; the newest, 10.0.26100, is recommended)"
#endif

#define RSX_D3D12_FEATURE_OPTIONS8              ((D3D12_FEATURE)36)
#define RSX_D3D12_FEATURE_OPTIONS13             ((D3D12_FEATURE)42)
#define RSX_D3D12_FEATURE_OPTIONS14             ((D3D12_FEATURE)43)
#define RSX_D3D12_SUBOBJECT_DEPTH_STENCIL2      ((D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)26)
#define RSX_D3D12_BLEND_ALPHA_FACTOR            ((D3D12_BLEND)20)
#define RSX_D3D12_BLEND_INV_ALPHA_FACTOR        ((D3D12_BLEND)21)

typedef struct rsx_d3d12_options8 {
    BOOL UnalignedBlockTexturesSupported;
} rsx_d3d12_options8;

typedef struct rsx_d3d12_options13 {
    BOOL UnrestrictedBufferTextureCopyPitchSupported;
    BOOL UnrestrictedVertexElementAlignmentSupported;
    BOOL InvertedViewportHeightFlipsYSupported;
    BOOL InvertedViewportDepthFlipsZSupported;
    BOOL TextureCopyBetweenDimensionsSupported;
    BOOL AlphaBlendFactorSupported;
} rsx_d3d12_options13;

typedef struct rsx_d3d12_options14 {
    BOOL AdvancedTextureOpsSupported;
    BOOL WriteableMSAATexturesSupported;
    BOOL IndependentFrontAndBackStencilRefMaskSupported;
} rsx_d3d12_options14;

typedef struct rsx_d3d12_stencilop_desc1 {
    D3D12_STENCIL_OP StencilFailOp;
    D3D12_STENCIL_OP StencilDepthFailOp;
    D3D12_STENCIL_OP StencilPassOp;
    D3D12_COMPARISON_FUNC StencilFunc;
    UINT8 StencilReadMask;
    UINT8 StencilWriteMask;
} rsx_d3d12_stencilop_desc1;

typedef struct rsx_d3d12_depth_stencil_desc2 {
    BOOL DepthEnable;
    D3D12_DEPTH_WRITE_MASK DepthWriteMask;
    D3D12_COMPARISON_FUNC DepthFunc;
    BOOL StencilEnable;
    rsx_d3d12_stencilop_desc1 FrontFace;
    rsx_d3d12_stencilop_desc1 BackFace;
    BOOL DepthBoundsTestEnable;
} rsx_d3d12_depth_stencil_desc2;

#define RSX_D3D12_SAME_(a, b) (sizeof(a) == sizeof(b))
#define RSX_D3D12_AT_(a, b, f) (offsetof(a, f) == offsetof(b, f))
#define RSX_D3D12_CHECK_(n, c) typedef char rsx_d3d12_compat_check_##n[(c) ? 1 : -1]

/* d3d12.h's D3D12_SDK_VERSION: 616 in 10.0.26100 (602 in 10.0.22621, 5 / 3 / none before). */
#if defined(D3D12_SDK_VERSION) && D3D12_SDK_VERSION >= 616
RSX_D3D12_CHECK_(o8, D3D12_FEATURE_D3D12_OPTIONS8 == 36
    && RSX_D3D12_SAME_(rsx_d3d12_options8, D3D12_FEATURE_DATA_D3D12_OPTIONS8));
RSX_D3D12_CHECK_(o13, D3D12_FEATURE_D3D12_OPTIONS13 == 42
    && RSX_D3D12_SAME_(rsx_d3d12_options13, D3D12_FEATURE_DATA_D3D12_OPTIONS13)
    && RSX_D3D12_AT_(rsx_d3d12_options13, D3D12_FEATURE_DATA_D3D12_OPTIONS13, AlphaBlendFactorSupported));
RSX_D3D12_CHECK_(blend, D3D12_BLEND_ALPHA_FACTOR == 20 && D3D12_BLEND_INV_ALPHA_FACTOR == 21);RSX_D3D12_CHECK_(o14, D3D12_FEATURE_D3D12_OPTIONS14 == 43
    && RSX_D3D12_SAME_(rsx_d3d12_options14, D3D12_FEATURE_DATA_D3D12_OPTIONS14)
    && RSX_D3D12_AT_(rsx_d3d12_options14, D3D12_FEATURE_DATA_D3D12_OPTIONS14, IndependentFrontAndBackStencilRefMaskSupported));
RSX_D3D12_CHECK_(so, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2 == 26);
RSX_D3D12_CHECK_(sop, RSX_D3D12_SAME_(rsx_d3d12_stencilop_desc1, D3D12_DEPTH_STENCILOP_DESC1)
    && RSX_D3D12_AT_(rsx_d3d12_stencilop_desc1, D3D12_DEPTH_STENCILOP_DESC1, StencilFunc)
    && RSX_D3D12_AT_(rsx_d3d12_stencilop_desc1, D3D12_DEPTH_STENCILOP_DESC1, StencilReadMask)
    && RSX_D3D12_AT_(rsx_d3d12_stencilop_desc1, D3D12_DEPTH_STENCILOP_DESC1, StencilWriteMask));
RSX_D3D12_CHECK_(ds2, RSX_D3D12_SAME_(rsx_d3d12_depth_stencil_desc2, D3D12_DEPTH_STENCIL_DESC2)
    && RSX_D3D12_AT_(rsx_d3d12_depth_stencil_desc2, D3D12_DEPTH_STENCIL_DESC2, StencilEnable)
    && RSX_D3D12_AT_(rsx_d3d12_depth_stencil_desc2, D3D12_DEPTH_STENCIL_DESC2, FrontFace)
    && RSX_D3D12_AT_(rsx_d3d12_depth_stencil_desc2, D3D12_DEPTH_STENCIL_DESC2, BackFace)
    && RSX_D3D12_AT_(rsx_d3d12_depth_stencil_desc2, D3D12_DEPTH_STENCIL_DESC2, DepthBoundsTestEnable));
#endif
#undef RSX_D3D12_SAME_
#undef RSX_D3D12_AT_
#undef RSX_D3D12_CHECK_

#endif /* RSX_D3D12_COMPAT_H */
