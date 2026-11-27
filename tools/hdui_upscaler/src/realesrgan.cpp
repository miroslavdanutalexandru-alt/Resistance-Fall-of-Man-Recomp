// realesrgan implemented with ncnn library
// HD UI pack build: CPU path (gpuid -1), conservative GPU mode, stop at the first failed GPU submit.

#include "realesrgan.h"

#include <algorithm>
#include <vector>
#include <stdio.h>
#include <string.h>
#if _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#if NCNN_VULKAN

#include <algorithm>
#include <vector>

static const uint32_t realesrgan_preproc_spv_data[] = {
    #include "realesrgan_preproc.spv.hex.h"
};
static const uint32_t realesrgan_preproc_fp16s_spv_data[] = {
    #include "realesrgan_preproc_fp16s.spv.hex.h"
};
static const uint32_t realesrgan_preproc_int8s_spv_data[] = {
    #include "realesrgan_preproc_int8s.spv.hex.h"
};
static const uint32_t realesrgan_postproc_spv_data[] = {
    #include "realesrgan_postproc.spv.hex.h"
};
static const uint32_t realesrgan_postproc_fp16s_spv_data[] = {
    #include "realesrgan_postproc_fp16s.spv.hex.h"
};
static const uint32_t realesrgan_postproc_int8s_spv_data[] = {
    #include "realesrgan_postproc_int8s.spv.hex.h"
};

static const uint32_t realesrgan_preproc_tta_spv_data[] = {
    #include "realesrgan_preproc_tta.spv.hex.h"
};
static const uint32_t realesrgan_preproc_tta_fp16s_spv_data[] = {
    #include "realesrgan_preproc_tta_fp16s.spv.hex.h"
};
static const uint32_t realesrgan_preproc_tta_int8s_spv_data[] = {
    #include "realesrgan_preproc_tta_int8s.spv.hex.h"
};
static const uint32_t realesrgan_postproc_tta_spv_data[] = {
    #include "realesrgan_postproc_tta.spv.hex.h"
};
static const uint32_t realesrgan_postproc_tta_fp16s_spv_data[] = {
    #include "realesrgan_postproc_tta_fp16s.spv.hex.h"
};
static const uint32_t realesrgan_postproc_tta_int8s_spv_data[] = {
    #include "realesrgan_postproc_tta_int8s.spv.hex.h"
};


// A failed submit / wait usually means the driver reset the GPU (VK_ERROR_DEVICE_LOST). Sending more work to a lost
// device can escalate the driver fault, so the whole process ends here at once (exit code 3) instead of going on with
// the next tile or image. ncnn has already printed the Vulkan error code (e.g. "vkQueueSubmit failed -4").
static void gpu_check_or_exit(int ret, const char* what)
{
    if (ret == 0)
        return;

    fprintf(stderr, "GPU_STOP: %s failed %d (device lost?) - stopping, nothing more is sent to the GPU\n", what, ret);
    fflush(stderr);
    fflush(stdout);
#if _WIN32
    TerminateProcess(GetCurrentProcess(), 3);
#endif
    _exit(3);
}

static void gpu_submit_or_exit(ncnn::VkCompute& cmd)
{
    gpu_check_or_exit(cmd.submit_and_wait(), "submit_and_wait");
}
#endif // NCNN_VULKAN

RealESRGAN::RealESRGAN(int gpuid, bool _tta_mode, int num_threads, bool conservative)
{
#if NCNN_VULKAN
    vkdev = gpuid == -1 ? 0 : ncnn::get_gpu_device(gpuid);
    realesrgan_preproc = 0;
    realesrgan_postproc = 0;
#else
    (void)gpuid;
#endif

    // fp32 everywhere on the cpu (no fp16 / bf16 / int8 storage): the same maths as the reference model
    net.opt.use_vulkan_compute = false;
    net.opt.use_fp16_packed = false;
    net.opt.use_fp16_storage = false;
    net.opt.use_fp16_arithmetic = false;
    net.opt.use_bf16_storage = false;
    net.opt.use_bf16_packed = false;
    net.opt.use_int8_storage = false;
    net.opt.use_int8_arithmetic = false;
    net.opt.num_threads = std::max(1, num_threads);

#if NCNN_VULKAN
    if (vkdev)
    {
        net.opt.use_vulkan_compute = true;
        net.opt.use_fp16_packed = !conservative;
        net.opt.use_fp16_storage = !conservative;
        net.opt.use_int8_storage = !conservative;
        if (conservative)
        {
            net.opt.use_cooperative_matrix = false;
            net.opt.use_subgroup_ops = false;
            net.opt.use_shader_local_memory = false;
            net.opt.use_fp16_uniform = false;
            net.opt.use_int8_uniform = false;
        }
        net.set_vulkan_device(vkdev);
    }
#else
    (void)conservative;
#endif

    bicubic_2x = 0;
    bicubic_3x = 0;
    bicubic_4x = 0;
    tta_mode = _tta_mode;
}

RealESRGAN::~RealESRGAN()
{
#if NCNN_VULKAN
    // cleanup preprocess and postprocess pipeline
    {
        delete realesrgan_preproc;
        delete realesrgan_postproc;
    }
#endif

    if (bicubic_2x)
    {
        bicubic_2x->destroy_pipeline(net.opt);
        delete bicubic_2x;
    }

    if (bicubic_3x)
    {
        bicubic_3x->destroy_pipeline(net.opt);
        delete bicubic_3x;
    }

    if (bicubic_4x)
    {
        bicubic_4x->destroy_pipeline(net.opt);
        delete bicubic_4x;
    }
}

#if _WIN32
int RealESRGAN::load(const std::wstring& parampath, const std::wstring& modelpath)
#else
int RealESRGAN::load(const std::string& parampath, const std::string& modelpath)
#endif
{
#if _WIN32
    {
        FILE* fp = _wfopen(parampath.c_str(), L"rb");
        if (!fp)
        {
            fwprintf(stderr, L"_wfopen %ls failed\n", parampath.c_str());
            return -1;
        }

        int ret = net.load_param(fp);

        fclose(fp);

        if (ret != 0)
        {
            fwprintf(stderr, L"load_param %ls failed\n", parampath.c_str());
            return -1;
        }
    }
    {
        FILE* fp = _wfopen(modelpath.c_str(), L"rb");
        if (!fp)
        {
            fwprintf(stderr, L"_wfopen %ls failed\n", modelpath.c_str());
            return -1;
        }

        int ret = net.load_model(fp);

        fclose(fp);

        if (ret != 0)
        {
            fwprintf(stderr, L"load_model %ls failed\n", modelpath.c_str());
            return -1;
        }
    }
#else
    if (net.load_param(parampath.c_str()) != 0 || net.load_model(modelpath.c_str()) != 0)
        return -1;
#endif

#if NCNN_VULKAN
    // initialize preprocess and postprocess pipeline
    if (vkdev)
    {
        std::vector<ncnn::vk_specialization_type> specializations(1);
#if _WIN32
        specializations[0].i = 1;
#else
        specializations[0].i = 0;
#endif

        realesrgan_preproc = new ncnn::Pipeline(vkdev);
        realesrgan_preproc->set_optimal_local_size_xyz(32, 32, 3);

        realesrgan_postproc = new ncnn::Pipeline(vkdev);
        realesrgan_postproc->set_optimal_local_size_xyz(32, 32, 3);

        if (tta_mode)
        {
            if (net.opt.use_fp16_storage && net.opt.use_int8_storage)
                realesrgan_preproc->create(realesrgan_preproc_tta_int8s_spv_data, sizeof(realesrgan_preproc_tta_int8s_spv_data), specializations);
            else if (net.opt.use_fp16_storage)
                realesrgan_preproc->create(realesrgan_preproc_tta_fp16s_spv_data, sizeof(realesrgan_preproc_tta_fp16s_spv_data), specializations);
            else
                realesrgan_preproc->create(realesrgan_preproc_tta_spv_data, sizeof(realesrgan_preproc_tta_spv_data), specializations);

            if (net.opt.use_fp16_storage && net.opt.use_int8_storage)
                realesrgan_postproc->create(realesrgan_postproc_tta_int8s_spv_data, sizeof(realesrgan_postproc_tta_int8s_spv_data), specializations);
            else if (net.opt.use_fp16_storage)
                realesrgan_postproc->create(realesrgan_postproc_tta_fp16s_spv_data, sizeof(realesrgan_postproc_tta_fp16s_spv_data), specializations);
            else
                realesrgan_postproc->create(realesrgan_postproc_tta_spv_data, sizeof(realesrgan_postproc_tta_spv_data), specializations);
        }
        else
        {
            if (net.opt.use_fp16_storage && net.opt.use_int8_storage)
                realesrgan_preproc->create(realesrgan_preproc_int8s_spv_data, sizeof(realesrgan_preproc_int8s_spv_data), specializations);
            else if (net.opt.use_fp16_storage)
                realesrgan_preproc->create(realesrgan_preproc_fp16s_spv_data, sizeof(realesrgan_preproc_fp16s_spv_data), specializations);
            else
                realesrgan_preproc->create(realesrgan_preproc_spv_data, sizeof(realesrgan_preproc_spv_data), specializations);

            if (net.opt.use_fp16_storage && net.opt.use_int8_storage)
                realesrgan_postproc->create(realesrgan_postproc_int8s_spv_data, sizeof(realesrgan_postproc_int8s_spv_data), specializations);
            else if (net.opt.use_fp16_storage)
                realesrgan_postproc->create(realesrgan_postproc_fp16s_spv_data, sizeof(realesrgan_postproc_fp16s_spv_data), specializations);
            else
                realesrgan_postproc->create(realesrgan_postproc_spv_data, sizeof(realesrgan_postproc_spv_data), specializations);
        }
    }
#endif

    // bicubic 2x/3x/4x for alpha channel
    ncnn::Layer** bicubic[3] = { &bicubic_2x, &bicubic_3x, &bicubic_4x };
    for (int i = 0; i < 3; i++)
    {
        ncnn::Layer* layer = ncnn::create_layer("Interp");
#if NCNN_VULKAN
        layer->vkdev = vkdev;
#endif

        ncnn::ParamDict pd;
        pd.set(0, 3);// bicubic
        pd.set(1, (float)(i + 2));
        pd.set(2, (float)(i + 2));
        layer->load_param(pd);

        layer->create_pipeline(net.opt);
        *bicubic[i] = layer;
    }

    return 0;
}

int RealESRGAN::process(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
#if NCNN_VULKAN
    if (vkdev)
        return process_gpu(inimage, outimage);
#endif
    return process_cpu(inimage, outimage);
}

#if NCNN_VULKAN
int RealESRGAN::process_gpu(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = tilesize;
    const int TILE_SIZE_Y = tilesize;

    ncnn::VkAllocator* blob_vkallocator = vkdev->acquire_blob_allocator();
    ncnn::VkAllocator* staging_vkallocator = vkdev->acquire_staging_allocator();

    ncnn::Option opt = net.opt;
    opt.blob_vkallocator = blob_vkallocator;
    opt.workspace_vkallocator = blob_vkallocator;
    opt.staging_vkallocator = staging_vkallocator;

    // each tile 100x100
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    const size_t in_out_tile_elemsize = opt.use_fp16_storage ? 2u : 4u;

    //#pragma omp parallel for num_threads(2)
    for (int yi = 0; yi < ytiles; yi++)
    {
        const int tile_h_nopad = std::min((yi + 1) * TILE_SIZE_Y, h) - yi * TILE_SIZE_Y;

        int in_tile_y0 = std::max(yi * TILE_SIZE_Y - prepadding, 0);
        int in_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y + prepadding, h);

        ncnn::Mat in;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            in = ncnn::Mat(w, (in_tile_y1 - in_tile_y0), (unsigned char*)pixeldata + in_tile_y0 * w * channels, (size_t)channels, 1);
        }
        else
        {
            if (channels == 3)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGR2RGB, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGB, w, (in_tile_y1 - in_tile_y0));
#endif
            }
            if (channels == 4)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_BGRA2RGBA, w, (in_tile_y1 - in_tile_y0));
#else
                in = ncnn::Mat::from_pixels(pixeldata + in_tile_y0 * w * channels, ncnn::Mat::PIXEL_RGBA, w, (in_tile_y1 - in_tile_y0));
#endif
            }
        }

        ncnn::VkCompute cmd(vkdev);

        // upload
        ncnn::VkMat in_gpu;
        {
            cmd.record_clone(in, in_gpu, opt);

            if (xtiles > 1)
            {
                gpu_submit_or_exit(cmd);
                cmd.reset();
            }
        }

        int out_tile_y0 = std::max(yi * TILE_SIZE_Y, 0);
        int out_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h);

        ncnn::VkMat out_gpu;
        if (opt.use_fp16_storage && opt.use_int8_storage)
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, (size_t)channels, 1, blob_vkallocator);
        }
        else
        {
            out_gpu.create(w * scale, (out_tile_y1 - out_tile_y0) * scale, channels, (size_t)4u, 1, blob_vkallocator);
        }

        for (int xi = 0; xi < xtiles; xi++)
        {
            const int tile_w_nopad = std::min((xi + 1) * TILE_SIZE_X, w) - xi * TILE_SIZE_X;

            if (tta_mode)
            {
                // preproc
                ncnn::VkMat in_tile_gpu[8];
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding;

                    in_tile_gpu[0].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[1].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[2].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[3].create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[4].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[5].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[6].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);
                    in_tile_gpu[7].create(tile_y1 - tile_y0, tile_x1 - tile_x0, 3, in_out_tile_elemsize, 1, blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu[0];
                    bindings[2] = in_tile_gpu[1];
                    bindings[3] = in_tile_gpu[2];
                    bindings[4] = in_tile_gpu[3];
                    bindings[5] = in_tile_gpu[4];
                    bindings[6] = in_tile_gpu[5];
                    bindings[7] = in_tile_gpu[6];
                    bindings[8] = in_tile_gpu[7];
                    bindings[9] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu[0].w;
                    constants[4].i = in_tile_gpu[0].h;
                    constants[5].i = in_tile_gpu[0].cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu[0].w;
                    dispatcher.h = in_tile_gpu[0].h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realesrgan_preproc, bindings, constants, dispatcher);
                }

                // realesrgan
                ncnn::VkMat out_tile_gpu[8];
                for (int ti = 0; ti < 8; ti++)
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(blob_vkallocator);
                    ex.set_workspace_vkallocator(blob_vkallocator);
                    ex.set_staging_vkallocator(staging_vkallocator);

                    ex.input("data", in_tile_gpu[ti]);

                    gpu_check_or_exit(ex.extract("output", out_tile_gpu[ti], cmd), "extract");

                    {
                        gpu_submit_or_exit(cmd);
                        cmd.reset();
                    }
                }

                ncnn::VkMat out_alpha_tile_gpu;
                if (channels == 4)
                {
                    if (scale == 1)
                    {
                        out_alpha_tile_gpu = in_alpha_tile_gpu;
                    }
                    if (scale == 2)
                    {
                        bicubic_2x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 3)
                    {
                        bicubic_3x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 4)
                    {
                        bicubic_4x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                }

                // postproc
                {
                    std::vector<ncnn::VkMat> bindings(10);
                    bindings[0] = out_tile_gpu[0];
                    bindings[1] = out_tile_gpu[1];
                    bindings[2] = out_tile_gpu[2];
                    bindings[3] = out_tile_gpu[3];
                    bindings[4] = out_tile_gpu[4];
                    bindings[5] = out_tile_gpu[5];
                    bindings[6] = out_tile_gpu[6];
                    bindings[7] = out_tile_gpu[7];
                    bindings[8] = out_alpha_tile_gpu;
                    bindings[9] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = out_tile_gpu[0].w;
                    constants[1].i = out_tile_gpu[0].h;
                    constants[2].i = out_tile_gpu[0].cstep;
                    constants[3].i = out_gpu.w;
                    constants[4].i = out_gpu.h;
                    constants[5].i = out_gpu.cstep;
                    constants[6].i = xi * TILE_SIZE_X * scale;
                    constants[7].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[8].i = prepadding * scale;
                    constants[9].i = prepadding * scale;
                    constants[10].i = channels;
                    constants[11].i = out_alpha_tile_gpu.w;
                    constants[12].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realesrgan_postproc, bindings, constants, dispatcher);
                }
            }
            else
            {
                // preproc
                ncnn::VkMat in_tile_gpu;
                ncnn::VkMat in_alpha_tile_gpu;
                {
                    // crop tile
                    int tile_x0 = xi * TILE_SIZE_X - prepadding;
                    int tile_x1 = std::min((xi + 1) * TILE_SIZE_X, w) + prepadding;
                    int tile_y0 = yi * TILE_SIZE_Y - prepadding;
                    int tile_y1 = std::min((yi + 1) * TILE_SIZE_Y, h) + prepadding;

                    in_tile_gpu.create(tile_x1 - tile_x0, tile_y1 - tile_y0, 3, in_out_tile_elemsize, 1, blob_vkallocator);

                    if (channels == 4)
                    {
                        in_alpha_tile_gpu.create(tile_w_nopad, tile_h_nopad, 1, in_out_tile_elemsize, 1, blob_vkallocator);
                    }

                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = in_gpu;
                    bindings[1] = in_tile_gpu;
                    bindings[2] = in_alpha_tile_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = in_gpu.w;
                    constants[1].i = in_gpu.h;
                    constants[2].i = in_gpu.cstep;
                    constants[3].i = in_tile_gpu.w;
                    constants[4].i = in_tile_gpu.h;
                    constants[5].i = in_tile_gpu.cstep;
                    constants[6].i = prepadding;
                    constants[7].i = prepadding;
                    constants[8].i = xi * TILE_SIZE_X;
                    constants[9].i = std::min(yi * TILE_SIZE_Y, prepadding);
                    constants[10].i = channels;
                    constants[11].i = in_alpha_tile_gpu.w;
                    constants[12].i = in_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = in_tile_gpu.w;
                    dispatcher.h = in_tile_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realesrgan_preproc, bindings, constants, dispatcher);
                }

                // realesrgan
                ncnn::VkMat out_tile_gpu;
                {
                    ncnn::Extractor ex = net.create_extractor();

                    ex.set_blob_vkallocator(blob_vkallocator);
                    ex.set_workspace_vkallocator(blob_vkallocator);
                    ex.set_staging_vkallocator(staging_vkallocator);

                    ex.input("data", in_tile_gpu);

                    gpu_check_or_exit(ex.extract("output", out_tile_gpu, cmd), "extract");
                }

                ncnn::VkMat out_alpha_tile_gpu;
                if (channels == 4)
                {
                    if (scale == 1)
                    {
                        out_alpha_tile_gpu = in_alpha_tile_gpu;
                    }
                    if (scale == 2)
                    {
                        bicubic_2x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 3)
                    {
                        bicubic_3x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                    if (scale == 4)
                    {
                        bicubic_4x->forward(in_alpha_tile_gpu, out_alpha_tile_gpu, cmd, opt);
                    }
                }

                // postproc
                {
                    std::vector<ncnn::VkMat> bindings(3);
                    bindings[0] = out_tile_gpu;
                    bindings[1] = out_alpha_tile_gpu;
                    bindings[2] = out_gpu;

                    std::vector<ncnn::vk_constant_type> constants(13);
                    constants[0].i = out_tile_gpu.w;
                    constants[1].i = out_tile_gpu.h;
                    constants[2].i = out_tile_gpu.cstep;
                    constants[3].i = out_gpu.w;
                    constants[4].i = out_gpu.h;
                    constants[5].i = out_gpu.cstep;
                    constants[6].i = xi * TILE_SIZE_X * scale;
                    constants[7].i = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    constants[8].i = prepadding * scale;
                    constants[9].i = prepadding * scale;
                    constants[10].i = channels;
                    constants[11].i = out_alpha_tile_gpu.w;
                    constants[12].i = out_alpha_tile_gpu.h;

                    ncnn::VkMat dispatcher;
                    dispatcher.w = std::min(TILE_SIZE_X * scale, out_gpu.w - xi * TILE_SIZE_X * scale);
                    dispatcher.h = out_gpu.h;
                    dispatcher.c = channels;

                    cmd.record_pipeline(realesrgan_postproc, bindings, constants, dispatcher);
                }
            }

            if (xtiles > 1)
            {
                gpu_submit_or_exit(cmd);
                cmd.reset();
            }

            fprintf(stderr, "%.2f%%\n", (float)(yi * xtiles + xi) / (ytiles * xtiles) * 100);
        }

        // download
        {
            ncnn::Mat out;

            if (opt.use_fp16_storage && opt.use_int8_storage)
            {
                out = ncnn::Mat(out_gpu.w, out_gpu.h, (unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, (size_t)channels, 1);
            }

            cmd.record_clone(out_gpu, out, opt);

            gpu_submit_or_exit(cmd);

            if (!(opt.use_fp16_storage && opt.use_int8_storage))
            {
                if (channels == 3)
                {
#if _WIN32
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGB2BGR);
#else
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGB);
#endif
                }
                if (channels == 4)
                {
#if _WIN32
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGBA2BGRA);
#else
                    out.to_pixels((unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels, ncnn::Mat::PIXEL_RGBA);
#endif
                }
            }
        }
    }

    vkdev->reclaim_blob_allocator(blob_vkallocator);
    vkdev->reclaim_staging_allocator(staging_vkallocator);

    return 0;
}
#endif // NCNN_VULKAN

// cpu path, after realsr-ncnn-vulkan's process_cpu (MIT, nihui); same tiling, padding and alpha handling as the gpu
// path without tta
int RealESRGAN::process_cpu(const ncnn::Mat& inimage, ncnn::Mat& outimage) const
{
    const unsigned char* pixeldata = (const unsigned char*)inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = tilesize;
    const int TILE_SIZE_Y = tilesize;

    ncnn::Option opt = net.opt;

    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    for (int yi = 0; yi < ytiles; yi++)
    {
        const int tile_h_nopad = std::min((yi + 1) * TILE_SIZE_Y, h) - yi * TILE_SIZE_Y;

        int in_tile_y0 = std::max(yi * TILE_SIZE_Y - prepadding, 0);
        int in_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y + prepadding, h);

        for (int xi = 0; xi < xtiles; xi++)
        {
            const int tile_w_nopad = std::min((xi + 1) * TILE_SIZE_X, w) - xi * TILE_SIZE_X;

            int in_tile_x0 = std::max(xi * TILE_SIZE_X - prepadding, 0);
            int in_tile_x1 = std::min((xi + 1) * TILE_SIZE_X + prepadding, w);

            // crop tile
            ncnn::Mat in;
            if (channels == 3)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels_roi(pixeldata, ncnn::Mat::PIXEL_BGR2RGB, w, h, in_tile_x0, in_tile_y0, in_tile_x1 - in_tile_x0, in_tile_y1 - in_tile_y0);
#else
                in = ncnn::Mat::from_pixels_roi(pixeldata, ncnn::Mat::PIXEL_RGB, w, h, in_tile_x0, in_tile_y0, in_tile_x1 - in_tile_x0, in_tile_y1 - in_tile_y0);
#endif
            }
            else if (channels == 4)
            {
#if _WIN32
                in = ncnn::Mat::from_pixels_roi(pixeldata, ncnn::Mat::PIXEL_BGRA2RGBA, w, h, in_tile_x0, in_tile_y0, in_tile_x1 - in_tile_x0, in_tile_y1 - in_tile_y0);
#else
                in = ncnn::Mat::from_pixels_roi(pixeldata, ncnn::Mat::PIXEL_RGBA, w, h, in_tile_x0, in_tile_y0, in_tile_x1 - in_tile_x0, in_tile_y1 - in_tile_y0);
#endif
            }
            else
            {
                fprintf(stderr, "unsupported channel count %d\n", channels);
                return -1;
            }

            // split alpha and preproc
            ncnn::Mat in_tile;
            ncnn::Mat in_alpha_tile;
            {
                in_tile.create(in.w, in.h, 3);
                for (int q = 0; q < 3; q++)
                {
                    const float* ptr = in.channel(q);
                    float* outptr = in_tile.channel(q);

                    for (int i = 0; i < in.w * in.h; i++)
                    {
                        *outptr++ = *ptr++ * (1 / 255.f);
                    }
                }

                if (channels == 4)
                {
                    in_alpha_tile = in.channel_range(3, 1).clone();
                }
            }

            // border padding (replicate, like the gpu preproc shader)
            {
                int pad_top = std::max(prepadding - yi * TILE_SIZE_Y, 0);
                int pad_bottom = std::max(std::min((yi + 1) * TILE_SIZE_Y + prepadding - h, prepadding), 0);
                int pad_left = std::max(prepadding - xi * TILE_SIZE_X, 0);
                int pad_right = std::max(std::min((xi + 1) * TILE_SIZE_X + prepadding - w, prepadding), 0);

                ncnn::Mat in_tile_padded;
                ncnn::copy_make_border(in_tile, in_tile_padded, pad_top, pad_bottom, pad_left, pad_right, ncnn::BORDER_REPLICATE, 0.f, opt);
                in_tile = in_tile_padded;
            }

            ncnn::Mat out_tile;
            {
                ncnn::Extractor ex = net.create_extractor();

                ex.input("data", in_tile);

                int ret = ex.extract("output", out_tile);
                if (ret != 0 || out_tile.empty())
                {
                    fprintf(stderr, "cpu inference failed %d\n", ret);
                    return -1;
                }
            }

            ncnn::Mat out_alpha_tile;
            if (channels == 4)
            {
                if (scale == 1)
                    out_alpha_tile = in_alpha_tile;
                else if (scale == 2)
                    bicubic_2x->forward(in_alpha_tile, out_alpha_tile, opt);
                else if (scale == 3)
                    bicubic_3x->forward(in_alpha_tile, out_alpha_tile, opt);
                else if (scale == 4)
                    bicubic_4x->forward(in_alpha_tile, out_alpha_tile, opt);
            }

            // postproc and merge alpha
            ncnn::Mat out;
            {
                out.create(tile_w_nopad * scale, tile_h_nopad * scale, channels);
                // the padded tile always starts prepadding pixels before this tile
                const int off_y = prepadding * scale;
                const int off_x = prepadding * scale;
                for (int q = 0; q < 3; q++)
                {
                    float* outptr = out.channel(q);

                    for (int i = 0; i < out.h; i++)
                    {
                        const float* ptr = out_tile.channel(q).row(i + off_y) + off_x;

                        for (int j = 0; j < out.w; j++)
                        {
                            float v = *ptr++ * 255.f + 0.5f;
                            *outptr++ = std::min(std::max(v, 0.f), 255.f);
                        }
                    }
                }

                if (channels == 4)
                {
                    const int aoff_y = (yi * TILE_SIZE_Y - in_tile_y0) * scale;
                    const int aoff_x = (xi * TILE_SIZE_X - in_tile_x0) * scale;
                    float* outptr = out.channel(3);

                    for (int i = 0; i < out.h; i++)
                    {
                        const float* ptr = out_alpha_tile.row(i + aoff_y) + aoff_x;

                        for (int j = 0; j < out.w; j++)
                        {
                            float v = *ptr++ + 0.5f;
                            *outptr++ = std::min(std::max(v, 0.f), 255.f);
                        }
                    }
                }
            }

            {
                unsigned char* dst = (unsigned char*)outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels + xi * scale * TILE_SIZE_X * channels;
                if (channels == 3)
                {
#if _WIN32
                    out.to_pixels(dst, ncnn::Mat::PIXEL_RGB2BGR, w * scale * channels);
#else
                    out.to_pixels(dst, ncnn::Mat::PIXEL_RGB, w * scale * channels);
#endif
                }
                else
                {
#if _WIN32
                    out.to_pixels(dst, ncnn::Mat::PIXEL_RGBA2BGRA, w * scale * channels);
#else
                    out.to_pixels(dst, ncnn::Mat::PIXEL_RGBA, w * scale * channels);
#endif
                }
            }

            fprintf(stderr, "%.2f%%\n", (float)(yi * xtiles + xi + 1) / (ytiles * xtiles) * 100);
        }
    }

    return 0;
}