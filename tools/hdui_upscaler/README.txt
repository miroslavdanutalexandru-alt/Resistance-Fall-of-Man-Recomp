HD UI pack upscaler (run by HDUIPack.exe, "Create HD UI pack" in the launcher)
=============================================================================

bin\realesrgan-ncnn-vulkan.exe  Real-ESRGAN on a Vulkan graphics card: the default, ONE run for all textures with its
                                own tile size (the way the Bloodborne PC port's Lantern Engine runs Real-ESRGAN, which
                                works on an RTX 5070; the hundreds of short runs with tiny tiles older versions made
                                ended in driver resets there). At the first GPU failure (GPU_STOP below, a lost device,
                                no progress for 90 s, a crash) HDUIPack.exe ends it at once, makes the rest with the
                                CPU build and writes tools\upscaler\gpu_fault.txt, so later runs use the CPU (until it
                                is deleted, or with --use-gpu).
bin\realesrgan-ncnn-cpu.exe     The same on the CPU only (the fallback, and --cpu). It has no Vulkan / graphics code at
                                all, so it cannot reach the graphics card or its driver.
bin\vcomp140.dll                Microsoft Visual C++ OpenMP runtime (redistributable), used by both.
models\realesr-animevideov3-x4  The "fast" model (the CPU default), from Real-ESRGAN (BSD-3-Clause, models\LICENSE.*).
                                Taken unchanged from realesrgan-ncnn-vulkan-20220424-windows.zip (SHA-256
                                abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d):
                                  .param 850a248e7c14c27e5bd8cf7265113a9441036a7db63963bb8aa5169d788a435e
                                  .bin   548a36f9c3f4ab8da56cd3b13badf23968bee207b396dad14d04b830e5f2ab2d
                                The GPU default "x4plus" (realesrgan-x4plus, same licence, 32 MB) is not bundled:
                                HDUIPack.exe downloads that zip once on the user's PC (SHA-256 above), extracts only
                                models/realesrgan-x4plus.param (35330ececcea33b6c397a72548e788d5d53becee4734c50b7fada36e89f10a86)
                                and .bin (713ee713b0353afaa27976f0563a64a5043bd70b9bd8936c2e26e25ebcdbcddf) into
                                tools\upscaler\models and deletes the zip. The "remacri" / "ultrasharp" models
                                (--model; CC BY-NC-SA 4.0) are downloaded the same way, checked against their SHA-256.

HDUIPack.exe checks the SHA-256 of every file here before it runs one (project\hd_ui_pack.cpp: upscaler_cpu,
upscaler_gpu, upscaler_omp, models[]).

Sources
-------
src\ is xinntao/Real-ESRGAN-ncnn-vulkan (MIT) at 37026f49824c5cf84062e7c6a5dd71445dcf610f (v0.2.0 is the latest
release, 2022), with win32dirent.h / filesystem_utils.h / wic_image.h from nihui/realsr-ncnn-vulkan (MIT) at
b7f890ee2704ccea76c73d9fd4d5b3298dd1beca, built against Tencent/ncnn (BSD-3-Clause) release 20261020, commit
7e36d25cb153092ba87a60530455c7376fedf452, with its glslang submodule at 780b6a209132dc74e99458f62ebe5fa7e57c41cf
(Vulkan build only). Changes to the upstream sources:
  - a CPU build (REALESRGAN_CPU_ONLY, ncnn with NCNN_VULKAN=OFF): -g -1 runs on the CPU; no Vulkan code is linked
  - -c (conservative GPU mode): fp32 storage, no fp16 / int8 storage, no cooperative matrix / subgroup ops / shader
    local memory; HDUIPack.exe passes it only with --safe
  - GPU_STOP: the first failed vkQueueSubmit / vkWaitForFences (a lost device) prints "GPU_STOP: ... (device lost?)"
    and ends the program with exit code 3, so nothing more is sent to the GPU
  - WebP support removed (only PNG is used); ncnn built with only the layers the models need
The licences are the LICENSE.* files here (ncnn's lists its own third-party parts).

Rebuilding
----------
  powershell -ExecutionPolicy Bypass -File tools\hdui_upscaler\build.ps1 [-Work <folder>]
Needs git, CMake, Ninja, Visual Studio 2022 C++ build tools and the Vulkan SDK. It clones ncnn at the pinned commit
into -Work (default tools\hdui_upscaler\_build, ignored by git), builds both upscalers, copies them and vcomp140.dll to
bin\ and prints their SHA-256 for hd_ui_pack.cpp (all three ports).
