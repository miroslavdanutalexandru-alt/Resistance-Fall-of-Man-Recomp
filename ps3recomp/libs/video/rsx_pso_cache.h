/* rsx_pso_cache.h -- persistent D3D12 pipeline library (ID3D12PipelineLibrary) for the guest pipelines.
 *
 * Every guest pipeline the D3D12 backend creates goes through rsx_pso_cache_create: a pipeline already in
 * shadercache\pso_library_<program>.bin is loaded from the driver's compiled blob (no driver compile), a new
 * one is created and stored. The library is written back by a background thread when it has grown (at most
 * every 30 s), after the start-up precompile and at shutdown, through a .tmp file and a rename.
 *
 * The file is used only when it was written by this build of the exe (PE time stamp + image size) for this
 * GPU and driver (the backend's pso_gpu_stamp: vendor, device, subsystem, revision, driver version, name);
 * a damaged, truncated or mismatched file is ignored (logged) and replaced. The adapter LUID is recorded for
 * the log only: Windows assigns a new one at every boot. RSX_PSO_LIBRARY=0 turns the library off. */
#pragma once
#ifdef _WIN32
#include <stddef.h>
#include <stdint.h>
#include <d3d12.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File checks (pure, used by the loader and the tests). */
enum {
    RSX_PSOLIB_OK = 0,
    RSX_PSOLIB_EMPTY,          /* no file / zero bytes */
    RSX_PSOLIB_CORRUPT,        /* short, bad magic / version / size / checksum */
    RSX_PSOLIB_OTHER_GPU,      /* another GPU or driver */
    RSX_PSOLIB_OTHER_BUILD     /* written by another build of the exe */
};
#define RSX_PSOLIB_HEADER_BYTES 64u
const char* rsx_pso_lib_verdict_text(int verdict);
uint64_t rsx_pso_lib_hash(const void* p, size_t n, uint64_t h);   /* FNV-1a 64, h = 0: the standard basis */
/* The file for payload (the Serialize blob): header + payload; returns malloc'ed bytes (*out_n), NULL on OOM. */
uint8_t* rsx_pso_lib_wrap(const void* payload, size_t payload_n, uint64_t gpu_hash, uint64_t build_hash,
                          uint64_t luid, size_t* out_n);
int rsx_pso_lib_check(const void* file, size_t n, uint64_t gpu_hash, uint64_t build_hash,
                      size_t* payload_off, size_t* payload_n);
uint64_t rsx_pso_lib_build_hash(void);   /* this exe: PE TimeDateStamp + SizeOfImage */

/* gpu_stamp: the backend's GPU / driver text (any text works; "" = unknown). Returns the pipelines in the
 * loaded library (0 also when empty / off). saver: 1 = background saver thread. Call once, after the device. */
int rsx_pso_cache_init(ID3D12Device* dev, const char* path, const char* gpu_stamp, uint64_t luid);
int rsx_pso_cache_init_ex(ID3D12Device* dev, const char* path, const char* gpu_stamp, uint64_t luid,
                          uint64_t build_hash, int saver);
int rsx_pso_cache_active(void);
/* CreateGraphicsPipelineState through the library (plain creation when the library is off). */
HRESULT rsx_pso_cache_create(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                             ID3D12PipelineState** out);
/* Pipelines built with root signature rs get tag mixed into their library names (a root-signature layout
 * change must not reuse blobs built for the old one); tag 0 / no call = names unchanged. */
void rsx_pso_cache_root_tag(ID3D12RootSignature* rs, uint32_t tag);
/* Writes the file when pipelines were added; 1 = written. Waits for a save already in flight (saver thread), so
 * a save just before ExitProcess always leaves the newest library on disk. */
int rsx_pso_cache_save(void);
void rsx_pso_cache_request_save(void);   /* wakes the saver thread (e.g. after the start-up precompile) */
void rsx_pso_cache_shutdown(void);       /* final save, releases the library */
void rsx_pso_cache_stats(long* loaded, long* stored, long* created);
extern volatile LONG rsx_pso_cache_test_write_delay_ms;   /* tests only */

#ifdef __cplusplus
}
#endif
#endif
