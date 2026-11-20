// realesrgan implemented with ncnn library
// HD UI pack build: adds a CPU path (gpuid -1, never touches Vulkan), a conservative GPU mode and stops the process
// at the first failed GPU submit instead of sending more work to a lost device.

#ifndef REALESRGAN_H
#define REALESRGAN_H

#include <string>

// ncnn
#include "net.h"
#include "gpu.h"
#include "layer.h"

class RealESRGAN
{
public:
    RealESRGAN(int gpuid, bool tta_mode = false, int num_threads = 1, bool conservative = false);
    ~RealESRGAN();

#if _WIN32
    int load(const std::wstring& parampath, const std::wstring& modelpath);
#else
    int load(const std::string& parampath, const std::string& modelpath);
#endif

    int process(const ncnn::Mat& inimage, ncnn::Mat& outimage) const;

    int process_cpu(const ncnn::Mat& inimage, ncnn::Mat& outimage) const;

public:
    // realesrgan parameters
    int scale;
    int tilesize;
    int prepadding;

private:
#if NCNN_VULKAN
    int process_gpu(const ncnn::Mat& inimage, ncnn::Mat& outimage) const;

    ncnn::VulkanDevice* vkdev;
    ncnn::Pipeline* realesrgan_preproc;
    ncnn::Pipeline* realesrgan_postproc;
#endif
    ncnn::Net net;
    ncnn::Layer* bicubic_2x;
    ncnn::Layer* bicubic_3x;
    ncnn::Layer* bicubic_4x;
    bool tta_mode;
};

#endif // REALESRGAN_H