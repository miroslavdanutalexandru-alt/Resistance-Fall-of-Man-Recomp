// HDUIPack.exe: makes the HD UI pack (mods\hd_ui) on this PC from the game files, for the launcher's "Create HD UI pack"
// button. Nothing to install: it reads the game's archives itself and runs the Real-ESRGAN ncnn upscaler that comes with
// the port (tools\hdui_upscaler: built from its src\ against ncnn 20261020, SHA-256 checked before every run): on the
// graphics card (Vulkan) with Real-ESRGAN's own realesrgan-x4plus model (downloaded once from Real-ESRGAN's release,
// SHA-256 checked, into tools\upscaler\models), and on the processor (realesrgan-ncnn-cpu.exe has no Vulkan code) when
// the graphics card cannot be used, with the fast model that comes with the port.
//
// One source for the Resistance 1 / 2 / 3 ports (identical file in each; same flags, output, folders and pack format).
// Everything game-specific is in the port's game backend header, hd_ui_game_r1.h / hd_ui_game_r2.h / hd_ui_game_r3.h
// (whichever is next to this file; HDUI_GAME_HEADER overrides):
//   1. Reads every menu / HUD texture asset in the game's archives (hdgame::scan_archives) and computes, from the
//      archive bytes, the key the game's texture upload computes for it (with the renderer's own key code: R1
//      ps3recomp/libs/video/r1_hd_ui.h, R2 / R3 rpcs3/Emu/RSX/Common/r2_hd_ui.h).
//   2. Upscales them 4x with the AI model (alpha separately, so edges stay clean), scales the result to 2x..4x the
//      original (4096 at most), builds every mip level and encodes them in the game texture's own format (DXT1/3/5 or
//      A8R8G8B8) as <key>_<W>x<H>_<format>_m<mips>.dds; a loose image (Source::image: R2's Scaleform external images,
//      which the game may upload in another layout) as <key>_<W>x<H>_<format>_m<mips>_image_x<scale>.dds, the files
//      the renderer's loose-image lookup is limited to.
//   3. Commits the pack every few textures by building the new folder beside it (mods\.hd_ui.stage: a dot folder,
//      which nothing loads; the same drive, so it can be renamed) and renaming it into place: an interrupted run
//      never leaves half-written or scratch files where the game loads replacements. Running it again
//      continues where it stopped (pack.tsv lists what was made with which model).
//
// GPU and CPU. Every texture goes through ONE upscaler process (-i <folder> -o <folder>, the upscaler's own tile size and
// threads), the way other ports run the same upscaler on the same cards without trouble. Earlier versions started one
// process per 1-4 textures (hundreds of Vulkan device creations) with forced 32 / 64 px tiles and the remacri model; on
// an RTX 5070 those runs ended in driver resets (nvlddmkm 13 / 153) and a restart. Inputs smaller than 64 px are padded
// (edge pixels repeated) and cropped back.
// A watchdog ends the upscaler at its first Vulkan error ("vkQueueSubmit failed -4" = device lost, or the upscaler's own
// "GPU_STOP: ... device lost?", after which it ends itself), when it makes no progress, or when it ends without having
// made every picture; nothing more is sent to the GPU then, and the textures not made yet are made on the CPU. Such a
// failure, or a GPU run the PC did not survive (the marker tools\upscaler\upscaler_running.txt older than the last boot,
// with an unexpected shutdown logged at the next start of Windows, or the log not readable), is written to
// tools\upscaler\gpu_fault.txt (the marker and the upscaler's log are kept beside it as upscaler_crashed.*): later runs
// use the CPU until that file is deleted or --use-gpu is given (a run with --use-gpu that makes everything on the GPU
// deletes it). --cpu: the CPU only. --status prints where the next run upscales.
//
// Output for the launcher (one per line): "@progress <phase> <done> <total>", "@eta <seconds left>", "@error <text>",
// "@summary <text>", "@warn <text>", "@gpu gpu|cpu <text>" (--status: gpu = the graphics card first, cpu = the processor).
// Exit code: 0 = pack ready, 2 = failed (an @error line says why).
//
// The pack holds art made from your copy of the game: it is for this PC only, do not share it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <winevt.h>
#pragma comment(lib, "wevtapi.lib")

#if defined(HDUI_GAME_HEADER)
#include HDUI_GAME_HEADER
#elif __has_include("hd_ui_game_r1.h")
#include "hd_ui_game_r1.h"
#elif __has_include("hd_ui_game_r2.h")
#include "hd_ui_game_r2.h"
#elif __has_include("hd_ui_game_r3.h")
#include "hd_ui_game_r3.h"
#else
#error "no HD UI pack game backend (hd_ui_game_r<N>.h) next to hd_ui_pack.cpp"
#endif
#include "miniz.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace hdpack
{
	// ------------------------------------------------------------------------------------------------ upscaler and models
	// tools\hdui_upscaler\bin (built by tools\hdui_upscaler\build.ps1 from its src\: xinntao's Real-ESRGAN-ncnn-vulkan
	// 0.2.0, patched, with ncnn 20261020 = commit 7e36d25). A file that does not match is never run.
	struct Pinned
	{
		const char* name;
		const char* sha;
	};
	constexpr Pinned upscaler_cpu = { "realesrgan-ncnn-cpu.exe", "87d64acf7ca426ca3bc190171b59145f53bad837d61d8f1027bdb2c54b7b1bf5" };
	constexpr Pinned upscaler_gpu = { "realesrgan-ncnn-vulkan.exe", "03fe961cc792ef776cdc23a7c88b588860396a3cebca3effd9b44b909fd76e3d" };
	constexpr Pinned upscaler_omp = { "vcomp140.dll", "55aba23cdcd6484fbb06f4155b8ca75adfce7a881f10afd0c49457165e677164" };   // OpenMP runtime
	// What older versions downloaded into --tools: the 2022 Real-ESRGAN release (its exe is never run again; its models
	// are kept where they are wanted). The same zip, from Real-ESRGAN's own release, is where realesrgan-x4plus comes from.
	constexpr const char* legacy_dir = "realesrgan";
	constexpr const char* legacy_zip = "realesrgan-ncnn-vulkan-20220424-windows.zip";
	constexpr const char* official_zip_url = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
	constexpr const char* official_zip_sha = "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d";
	constexpr const char* upscayl_models = "https://raw.githubusercontent.com/upscayl/upscayl/a00d55fee90e0f9435d5eaa86e76700df8199af8/resources/models/";

	enum class From { bundled, official_zip, upscayl };
	struct Model
	{
		const char* name;     // --model
		const char* file;     // models\<file>.param / .bin
		const char* param_sha;
		const char* bin_sha;
		From from;            // bundled: tools\hdui_upscaler\models; else downloaded into <tools>\models
		double cpu_px_s;      // input pixels a second on the CPU (Ryzen 5 5600X, 6 threads): for the time estimate
	};
	// x4plus: Real-ESRGAN's general model realesrgan-x4plus (BSD-3-Clause; the default on the GPU, extracted from
	// Real-ESRGAN's realesrgan-ncnn-vulkan-20220424 release zip). fast: its compact realesr-animevideov3 (BSD-3-Clause,
	// from the same zip, comes with the port), about 30x quicker, the default on the CPU. remacri: the ESRGAN model the
	// Bloodborne "AI upscaled UI" mods used; ultrasharp: sharper (both CC BY-NC-SA, downloaded from the Upscayl repository
	// at a pinned commit).
	constexpr Model models[] = {
		{ "x4plus", "realesrgan-x4plus", "35330ececcea33b6c397a72548e788d5d53becee4734c50b7fada36e89f10a86", "713ee713b0353afaa27976f0563a64a5043bd70b9bd8936c2e26e25ebcdbcddf", From::official_zip, 5000 },
		{ "fast", "realesr-animevideov3-x4", "850a248e7c14c27e5bd8cf7265113a9441036a7db63963bb8aa5169d788a435e", "548a36f9c3f4ab8da56cd3b13badf23968bee207b396dad14d04b830e5f2ab2d", From::bundled, 150000 },
		{ "remacri", "remacri-4x", "859ecba5b3592ecf3e76c93bed65e9f627b5236dd696aae5a84ecf8c93ab65ce", "a43be595c0d743314c30b50fe7ef188be0c61cc55c46ce81adb79ba4b3c3fb7a", From::upscayl, 5300 },
		{ "ultrasharp", "ultrasharp-4x", "0136ca83686809a8f17f7111f11b951e8db93610e24b7f4137c9ffe4dbc4a806", "fb3e279d40d4cddb44db4e684d59e68d0aa39852c8cc14dc3f23ccc7e6eee9c1", From::upscayl, 5300 },
	};
	constexpr const char* gpu_default_model = "x4plus";
	constexpr const char* cpu_default_model = "fast";
	inline const Model* find_model(const std::string& name)
	{
		const std::string n = name == "realesrgan-x4plus" ? "x4plus" : name == "animevideov3" ? "fast" : name;
		for (const Model& m : models)
			if (n == m.name) return &m;
		return nullptr;
	}

	constexpr uint32_t target_side = 4096;  // the HD texture's longer side at most

	// How the upscaler runs. On the GPU: one run for every texture, the upscaler's own tile size (0: by the card's memory)
	// and threads; --safe: conservative mode (-c: fp32, no fp16 / int8 / cooperative matrix / subgroup ops), 64 px tiles
	// and one image at a time (-j 1:1:1), still one run. On the CPU: 200 px tiles (a tile is one parallel job for all
	// threads), 8 textures per run (a run can be cancelled and continued; the model is loaded once per run).
	struct Settings
	{
		const char* name;
		int tile;          // -t (0: the upscaler's choice)
		size_t batch;      // textures per upscaler run (0: all in one)
		bool conservative; // -c -j 1:1:1
	};
	constexpr Settings normal_settings = { "normal", 0, 0, false };
	constexpr Settings safe_settings = { "safe", 64, 0, true };
	constexpr Settings cpu_settings = { "cpu", 200, 8, false };
	constexpr uint32_t min_side = 64;        // inputs are padded to at least this a side
	constexpr unsigned stall_seconds = 90;   // no new log line or output file for this long: the upscaler is ended
	constexpr unsigned cpu_stall_seconds = 900;   // the same on the CPU (a 200 px tile of a large model on a slow CPU)
	constexpr unsigned cpu_probe_seconds = 300;   // the CPU test image's whole run at most
	constexpr const char* running_marker = "upscaler_running.txt";   // in --tools while an upscaler runs on the GPU
	constexpr const char* fault_file = "gpu_fault.txt";              // in --tools: an earlier GPU run failed: the CPU is used
	constexpr const char* crashed_marker = "upscaler_crashed.txt";   // the marker of a run the PC may not have survived
	constexpr const char* crashed_log = "upscaler_crashed.log";       // the upscaler's log of a failed / crashed GPU run
	constexpr const char* unavailable_file = "gpu_unavailable.txt";   // in --tools: no usable GPU last time (not a fault)

	// ------------------------------------------------------------------------------------------------ output
	inline void out(const char* f, ...)
	{
		va_list ap;
		va_start(ap, f);
		std::vprintf(f, ap);
		va_end(ap);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}

	// ------------------------------------------------------------------------------------------------ archive textures
	using hdgame::Scan;
	using hdgame::Source;
	using hdgame::TexInfo;

	// How much larger the HD texture is: 2..4, the longer side 4096 at most; 0 = too large to gain anything. Where the
	// renderer needs a DXT texture's HD size in whole 4x4 blocks (hdgame::dxt_whole_blocks: R1's D3D12 creates no
	// other), 3x needs both sides a multiple of 4 and 2x both even -- 4x always is.
	inline uint32_t pick_scale(uint32_t w, uint32_t h, uint32_t fmt = 0)
	{
		uint32_t s = std::min<uint32_t>(4, target_side / std::max(w, h));
		while (hdgame::dxt_whole_blocks && s >= 2 && hdui::is_dxt(fmt) && (((w * s) & 3) || ((h * s) & 3))) --s;
		return s >= 2 ? s : 0;
	}

	inline std::string pack_name(uint64_t key, uint32_t w, uint32_t h, uint32_t fmt, uint32_t mips, uint32_t scale, bool image = false)
	{
		return hdui::sfmt("%016llx_%ux%u_%s_m%u", static_cast<unsigned long long>(key), w * scale, h * scale, hdui::fmt_name(fmt), mips) +
			(image ? hdui::sfmt("_image_x%u.dds", scale) : std::string(".dds"));
	}

	inline std::string lower(std::string s)
	{
		for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	// Every packable texture of the game's archives by key; those too large to gain anything are counted as skipped
	inline Scan scan_archives(const fs::path& usrdir, bool progress)
	{
		Scan sc = hdgame::scan_archives(usrdir, [&](unsigned d, unsigned t) { if (progress) out("@progress read %u %u", d, t); });
		for (auto it = sc.textures.begin(); it != sc.textures.end();)
			if (!pick_scale(it->second.tex.w, it->second.tex.h, it->second.tex.fmt))
			{
				sc.skipped[pick_scale(it->second.tex.w, it->second.tex.h) ? "no 2x..4x size of whole DXT blocks" : "already 2048 or larger"]++;
				it = sc.textures.erase(it);
			}
			else ++it;
		return sc;
	}

	// ------------------------------------------------------------------------------------------------ pixels
	using Rgba = std::vector<uint8_t>;

	inline void c565(uint16_t c, uint8_t* o)
	{
		o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
		o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
		o[2] = uint8_t((c & 31) * 255 / 31);
	}

	inline void decode_color(const uint8_t* b, bool dxt1, uint8_t px[16][4])
	{
		const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
		uint8_t pal[4][4] = {};
		c565(c0, pal[0]);
		c565(c1, pal[1]);
		pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
		if (!dxt1 || c0 > c1)
		{
			for (int k = 0; k < 3; ++k)
			{
				pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
				pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
			}
		}
		else
		{
			for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
			pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
		}
		const uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
		for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
	}

	// Level 0 of a texture in guest format -> RGBA
	inline Rgba decode(const uint8_t* d, const TexInfo& t)
	{
		const uint32_t w = t.w, h = t.h, fmt = t.fmt;
		Rgba img(size_t(w) * h * 4);
		if (!hdui::is_dxt(fmt))
		{
			hdgame::decode_argb(d, t, img.data());
			return img;
		}
		const uint32_t bb = hdui::block_bytes(fmt), row = hdgame::dxt_row_bytes(t);
		for (uint32_t by = 0; by < (h + 3) / 4; ++by)
			for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx)
			{
				const uint8_t* b = d + size_t(by) * row + size_t(bx) * bb;
				uint8_t px[16][4];
				if (fmt == hdui::GCM_DXT1) decode_color(b, true, px);
				else decode_color(b + 8, false, px);
				if (fmt == hdui::GCM_DXT23)
					for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
				else if (fmt == hdui::GCM_DXT45)
				{
					uint8_t a[8] = { b[0], b[1] };
					if (a[0] > a[1]) for (int k = 1; k < 7; ++k) a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1] + 3) / 7);
					else
					{
						for (int k = 1; k < 5; ++k) a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1] + 2) / 5);
						a[6] = 0;
						a[7] = 255;
					}
					uint64_t bits = 0;
					for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
					for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
				}
				for (int i = 0; i < 16; ++i)
				{
					const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
					if (x < w && y < h) std::memcpy(&img[(size_t(y) * w + x) * 4], px[i], 4);
				}
			}
		return img;
	}

	inline bool has_alpha(const Rgba& img)
	{
		for (size_t i = 3; i < img.size(); i += 4)
			if (img[i] != 255) return true;
		return false;
	}

	// RGB of fully transparent texels from their visible neighbours, so the upscaler draws no dark fringe at edges
	inline std::vector<uint8_t> bleed_rgb(const Rgba& img, uint32_t w, uint32_t h)
	{
		std::vector<uint8_t> rgb(size_t(w) * h * 3);
		std::vector<uint8_t> known(size_t(w) * h);
		uint64_t sum[3] = {};
		size_t nk = 0;
		for (size_t i = 0; i < size_t(w) * h; ++i)
		{
			for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = img[i * 4 + k];
			known[i] = img[i * 4 + 3] != 0;
			if (known[i])
			{
				for (int k = 0; k < 3; ++k) sum[k] += img[i * 4 + k];
				nk++;
			}
		}
		if (nk == size_t(w) * h) return rgb;
		for (int pass = 0; pass < 16; ++pass)
		{
			std::vector<uint8_t> next = known;
			bool any = false;
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					const size_t i = size_t(y) * w + x;
					if (known[i]) continue;
					uint32_t s[3] = {}, n = 0;
					const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
					for (int k = 0; k < 4; ++k)
					{
						const int nx = int(x) + dx[k], ny = int(y) + dy[k];
						if (nx < 0 || ny < 0 || nx >= int(w) || ny >= int(h)) continue;
						const size_t j = size_t(ny) * w + nx;
						if (!known[j]) continue;
						for (int c = 0; c < 3; ++c) s[c] += rgb[j * 3 + c];
						n++;
					}
					if (!n) continue;
					for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = uint8_t(s[c] / n);
					next[i] = 1;
					any = true;
				}
			known.swap(next);
			if (!any) break;
		}
		for (size_t i = 0; i < size_t(w) * h; ++i)
			if (!known[i])
				for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = nk ? uint8_t(sum[k] / nk) : 0;
		return rgb;
	}

	// The 4x upscaler output (RGB, and alpha as grey when the texture has alpha) -> RGBA at `ow` x `oh`
	inline Rgba combine(const std::vector<uint8_t>& rgb4, const std::vector<uint8_t>* alpha4, uint32_t w4, uint32_t h4, uint32_t ow, uint32_t oh, bool binary_alpha)
	{
		std::vector<uint8_t> rgb(size_t(ow) * oh * 3), a;
		if (ow == w4 && oh == h4) rgb = rgb4;
		else stbir_resize_uint8_srgb(rgb4.data(), int(w4), int(h4), 0, rgb.data(), int(ow), int(oh), 0, STBIR_RGB);
		if (alpha4)
		{
			a.resize(size_t(ow) * oh);
			if (ow == w4 && oh == h4) a = *alpha4;
			else stbir_resize_uint8_linear(alpha4->data(), int(w4), int(h4), 0, a.data(), int(ow), int(oh), 0, STBIR_1CHANNEL);
		}
		Rgba img(size_t(ow) * oh * 4);
		for (size_t i = 0; i < size_t(ow) * oh; ++i)
		{
			for (int k = 0; k < 3; ++k) img[i * 4 + k] = rgb[i * 3 + k];
			uint8_t al = alpha4 ? a[i] : 255;
			if (binary_alpha) al = al >= 128 ? 255 : 0;
			img[i * 4 + 3] = al;
		}
		return img;
	}

	// The upscaled texture (W x H) against its source (w x h), the first reduced to the second's size: the mean alpha
	// difference, and the mean colour difference where the source is visible. A picture of another texture or GPU garbage
	// (seen in packs from GPU runs that lost the device: alpha off by 65..116, colour by 36..253) is refused; the
	// model's own changes stay far below that (x4plus over a whole pack: 17 / 26 at most).
	inline bool like_source(const Rgba& hd, uint32_t W, uint32_t H, const Rgba& src, uint32_t w, uint32_t h)
	{
		if (!w || !h || W < w || H < h || hd.size() < size_t(W) * H * 4 || src.size() < size_t(w) * h * 4) return false;
		const uint32_t sx = W / w, sy = H / h;
		double ea = 0, ec = 0;
		size_t nc = 0;
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x)
			{
				uint32_t s[4] = {};
				for (uint32_t dy = 0; dy < sy; ++dy)
					for (uint32_t dx = 0; dx < sx; ++dx)
						for (int c = 0; c < 4; ++c) s[c] += hd[((size_t(y) * sy + dy) * W + size_t(x) * sx + dx) * 4 + c];
				const uint8_t* o = &src[(size_t(y) * w + x) * 4];
				const double n = double(sx) * sy;
				ea += std::abs(s[3] / n - o[3]);
				if (o[3] >= 128)
				{
					ec += (std::abs(s[0] / n - o[0]) + std::abs(s[1] / n - o[1]) + std::abs(s[2] / n - o[2])) / 3;
					nc++;
				}
			}
		ea /= double(w) * h;
		if (nc) ec /= double(nc);
		return ea <= 40 && ec <= 60;
	}

	inline Rgba half(const Rgba& img, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh)
	{
		Rgba o(size_t(nw) * nh * 4);
		for (uint32_t y = 0; y < nh; ++y)
			for (uint32_t x = 0; x < nw; ++x)
				for (int c = 0; c < 4; ++c)
				{
					uint32_t s = 0;
					for (uint32_t dy = 0; dy < 2; ++dy)
						for (uint32_t dx = 0; dx < 2; ++dx)
							s += img[(size_t(std::min(y * 2 + dy, h - 1)) * w + std::min(x * 2 + dx, w - 1)) * 4 + c];
					o[(size_t(y) * nw + x) * 4 + c] = uint8_t((s + 2) / 4);
				}
		return o;
	}

	inline uint16_t to565(const uint8_t* p) { return uint16_t(((p[0] * 31 + 127) / 255) << 11 | ((p[1] * 63 + 127) / 255) << 5 | ((p[2] * 31 + 127) / 255)); }

	// DXT1 block with transparent texels: 3-colour mode (colour0 <= colour1), index 3 = transparent
	inline void dxt1_punch(uint8_t* o, const uint8_t px[16][4])
	{
		uint8_t opq[16][4];
		uint32_t s[3] = {}, n = 0;
		for (int i = 0; i < 16; ++i)
			if (px[i][3] >= 128)
			{
				for (int k = 0; k < 3; ++k) s[k] += px[i][k];
				n++;
			}
		if (!n)
		{
			std::memset(o, 0, 4);
			std::memset(o + 4, 0xFF, 4);
			return;
		}
		for (int i = 0; i < 16; ++i)
		{
			std::memcpy(opq[i], px[i], 4);
			if (px[i][3] < 128)
				for (int k = 0; k < 3; ++k) opq[i][k] = uint8_t(s[k] / n);
			opq[i][3] = 255;
		}
		uint8_t tmp[8];
		stb_compress_dxt_block(tmp, &opq[0][0], 0, STB_DXT_HIGHQUAL);
		uint16_t c0 = uint16_t(tmp[0] | (tmp[1] << 8)), c1 = uint16_t(tmp[2] | (tmp[3] << 8));
		if (c0 > c1) std::swap(c0, c1);
		uint8_t pal[3][3];
		c565(c0, pal[0]);
		c565(c1, pal[1]);
		for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
		uint32_t idx = 0;
		for (int i = 0; i < 16; ++i)
		{
			uint32_t best = 3;
			if (px[i][3] >= 128)
			{
				int bd = INT32_MAX;
				for (uint32_t p = 0; p < 3; ++p)
				{
					int d = 0;
					for (int k = 0; k < 3; ++k) d += (int(px[i][k]) - pal[p][k]) * (int(px[i][k]) - pal[p][k]);
					if (d < bd) bd = d, best = p;
				}
			}
			idx |= best << (2 * i);
		}
		o[0] = uint8_t(c0);
		o[1] = uint8_t(c0 >> 8);
		o[2] = uint8_t(c1);
		o[3] = uint8_t(c1 >> 8);
		std::memcpy(o + 4, &idx, 4);
	}

	// One level RGBA -> the game texture's format (DXT blocks / B,G,R,A bytes for the DDS)
	inline std::vector<uint8_t> encode(const Rgba& img, uint32_t w, uint32_t h, uint32_t fmt)
	{
		if (!hdui::is_dxt(fmt))
		{
			std::vector<uint8_t> o(img.size());
			for (size_t i = 0; i < img.size(); i += 4)
			{
				o[i + 0] = img[i + 2];
				o[i + 1] = img[i + 1];
				o[i + 2] = img[i + 0];
				o[i + 3] = img[i + 3];
			}
			return o;
		}
		const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = hdui::block_bytes(fmt);
		std::vector<uint8_t> o(size_t(bw) * bh * bb);
		for (uint32_t by = 0; by < bh; ++by)
			for (uint32_t bx = 0; bx < bw; ++bx)
			{
				uint8_t px[16][4];
				bool transparent = false;
				for (int i = 0; i < 16; ++i)
				{
					const uint32_t x = std::min(bx * 4 + (i & 3), w - 1), y = std::min(by * 4 + (i >> 2), h - 1);
					std::memcpy(px[i], &img[(size_t(y) * w + x) * 4], 4);
					transparent |= px[i][3] < 128;
				}
				uint8_t* b = &o[(size_t(by) * bw + bx) * bb];
				if (fmt == hdui::GCM_DXT1)
				{
					if (transparent) dxt1_punch(b, px);
					else stb_compress_dxt_block(b, &px[0][0], 0, STB_DXT_HIGHQUAL);
				}
				else if (fmt == hdui::GCM_DXT45) stb_compress_dxt_block(b, &px[0][0], 1, STB_DXT_HIGHQUAL);
				else
				{
					std::memset(b, 0, 8);
					for (int i = 0; i < 16; ++i) b[i / 2] |= uint8_t(((px[i][3] * 15 + 127) / 255) << ((i & 1) * 4));
					stb_compress_dxt_block(b + 8, &px[0][0], 0, STB_DXT_HIGHQUAL);
				}
			}
		return o;
	}

	// Every level of the HD texture, as the DDS file the runtime loads (the renderer's DDS check accepts it)
	inline std::vector<uint8_t> make_dds(const Rgba& top, uint32_t w, uint32_t h, uint32_t fmt, uint32_t mips, uint32_t scale)
	{
		const uint32_t W = w * scale, H = h * scale;
		std::vector<uint8_t> file = hdui::dds_header(W, H, fmt, mips);
		Rgba cur = top;
		uint32_t cw = W, ch = H;
		for (uint32_t l = 0; l < mips; ++l)
		{
			const uint32_t lw = hdui::level_dim(w, scale, l), lh = hdui::level_dim(h, scale, l);
			if (l) cur = half(cur, cw, ch, lw, lh);
			cw = lw;
			ch = lh;
			const auto e = encode(cur, lw, lh, fmt);
			file.insert(file.end(), e.begin(), e.end());
		}
		return file;
	}

	// ------------------------------------------------------------------------------------------------ files
	inline bool read_file(const fs::path& p, std::vector<uint8_t>& d)
	{
		FILE* f = _wfopen(p.c_str(), L"rb");
		if (!f) return false;
		std::fseek(f, 0, SEEK_END);
		const long n = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		d.resize(n > 0 ? size_t(n) : 0);
		const bool ok = d.empty() || std::fread(d.data(), 1, d.size(), f) == d.size();
		std::fclose(f);
		return ok;
	}
	inline bool write_file(const fs::path& p, const void* d, size_t n)
	{
		FILE* f = _wfopen(p.c_str(), L"wb");
		if (!f) return false;
		const bool ok = std::fwrite(d, 1, n, f) == n;
		return (std::fclose(f) == 0) && ok;
	}

	// pack.tsv: key, file, model, source size/format, version (what was made, for skipping it next time). Version 2: the
	// upscaled picture was checked against its source (like_source); rows without one are from before that check.
	constexpr unsigned pack_version = 2;
	struct Made
	{
		std::string file, model, source;
		unsigned version = pack_version;
	};
	// A row an earlier version made with a model other than fast / x4plus: the GPU pipeline before version 2 (remacri by
	// default, many short runs, some of them on a lost device) committed pictures nothing checked, and some are another
	// texture's or garbage (dark squares on HUD frame corners): made again. fast (the CPU) and x4plus (the single GPU run
	// with its stop at the first Vulkan error) were made right.
	inline bool stale_row(const Made& m)
	{
		return m.version < 2 && m.model != "fast" && m.model != "x4plus";
	}
	// A row without a file: the texture's upscaled picture did not match it on the CPU either (like_source), so the game
	// keeps its original texture; later runs leave it alone (--force tries it again)
	constexpr const char* refused_model = "refused";
	inline bool refused_row(const Made& m) { return m.model == refused_model; }
	inline std::map<uint64_t, Made> read_manifest(const fs::path& p)
	{
		std::map<uint64_t, Made> m;
		FILE* f = _wfopen(p.c_str(), L"r");
		if (!f) return m;
		char line[1024];
		while (std::fgets(line, sizeof line, f))
		{
			std::string l = line;
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
			std::vector<std::string> c;
			size_t s = 0;
			for (size_t i = 0; i <= l.size(); ++i)
				if (i == l.size() || l[i] == '\t') c.push_back(l.substr(s, i - s)), s = i + 1;
			uint64_t k;
			if (c.size() >= 4 && hdui::key_of_name(c[0], k))
				m[k] = Made{ c[1], c[2], c[3], c.size() >= 5 && c[4].size() > 1 && c[4][0] == 'v' ? unsigned(std::strtoul(c[4].c_str() + 1, nullptr, 10)) : 1u };
		}
		std::fclose(f);
		return m;
	}

	// Folders beside the pack folder `out` (same parent, so on the same drive: a folder rename cannot cross drives, and
	// --out / --tools may be on different ones): mods\hd_ui -> mods\.hd_ui.<what>. The leading dot keeps the game's
	// mod loader, the launcher's mods list and the HD UI pack loader away from them.
	inline fs::path beside_pack(const fs::path& out, const wchar_t* what)
	{
		const fs::path o = out.has_filename() ? out : out.parent_path();
		return o.parent_path() / (std::wstring(L".") + o.filename().wstring() + L"." + what);
	}
	inline fs::path stage_dir(const fs::path& out) { return beside_pack(out, L"stage"); }
	inline fs::path removed_dir(const fs::path& out) { return beside_pack(out, L"removed"); }

	// Replaces the pack folder `out` atomically: `stage` (stage_dir(out): beside it, never inside it) gets hard links
	// to the files kept from the old pack plus the new files, then is renamed into place (the old pack goes to
	// `stage`.old first). Scratch never enters `out`. A NewFile without a name drops the key's old file: its row becomes
	// `made` when that is a refused row, else it is removed (the game uses the original texture).
	struct NewFile
	{
		uint64_t key;
		std::string name;
		std::vector<uint8_t> data;
		Made made;
	};
	inline bool commit_pack(const fs::path& out, const fs::path& stage, std::vector<NewFile>& files, std::map<uint64_t, Made>& manifest, std::string& why)
	{
		std::error_code ec;
		fs::remove_all(stage, ec);
		fs::create_directories(stage, ec);
		if (ec) { why = "cannot create " + stage.string(); return false; }
		std::set<uint64_t> replaced;
		for (const auto& f : files) replaced.insert(f.key);
		if (fs::is_directory(out, ec))
			for (const auto& e : fs::directory_iterator(out, ec))
			{
				const std::string name = e.path().filename().string();
				if (!e.is_regular_file() || name == "pack.tsv" || name.empty() || name[0] == '.') continue;
				uint64_t k;
				if (hdui::key_of_name(e.path().stem().string(), k) && replaced.count(k)) continue;   // the old file of a key made again
				if (!CreateHardLinkW((stage / e.path().filename()).c_str(), e.path().c_str(), nullptr))
					fs::copy_file(e.path(), stage / e.path().filename(), fs::copy_options::overwrite_existing, ec);
			}
		for (auto& f : files)
		{
			if (f.name.empty())
			{
				if (refused_row(f.made)) manifest[f.key] = f.made;
				else manifest.erase(f.key);
				continue;
			}
			if (!write_file(stage / f.name, f.data.data(), f.data.size())) { why = "cannot write " + (stage / f.name).string() + " (disk full?)"; return false; }
			manifest[f.key] = f.made;
			f.data.clear();
			f.data.shrink_to_fit();
		}
		std::string tsv = "# HD UI pack made on this PC from your game files by HDUIPack.exe: do not share it\n";
		for (const auto& [k, m] : manifest)
			if (refused_row(m) || (!m.file.empty() && fs::is_regular_file(stage / m.file, ec)))
				tsv += hdui::sfmt("%016llx\t", static_cast<unsigned long long>(k)) + m.file + "\t" + m.model + "\t" + m.source + hdui::sfmt("\tv%u\n", m.version);
		if (!write_file(stage / "pack.tsv", tsv.data(), tsv.size())) { why = "cannot write pack.tsv"; return false; }
		const fs::path old = fs::path(stage).concat(".old");
		fs::remove_all(old, ec);
		fs::create_directories(out.parent_path(), ec);
		if (fs::exists(out, ec) && !MoveFileExW(out.c_str(), old.c_str(), 0))
		{
			why = "cannot replace " + out.string() + " (is the game running?)";
			return false;
		}
		if (!MoveFileExW(stage.c_str(), out.c_str(), 0))
		{
			MoveFileExW(old.c_str(), out.c_str(), 0);
			why = "cannot move the new pack into " + out.string();
			return false;
		}
		fs::remove_all(old, ec);
		return true;
	}

	// An earlier run killed between the two renames of commit_pack (the launcher's Cancel ends it at any point): the
	// staged pack is complete (pack.tsv is written last), so it goes into place; if that rename fails, the old one goes
	// back. The leftovers are only deleted once `out` exists: when neither rename works (a file held open by an
	// antivirus, the indexer, Explorer ...) both folders are kept for the next run and false is returned.
	inline bool recover_pack(const fs::path& out, const fs::path& stage, std::string& why)
	{
		std::error_code ec;
		const fs::path old = fs::path(stage).concat(".old");
		if (!fs::exists(out, ec))
		{
			const bool staged = fs::is_regular_file(stage / "pack.tsv", ec), have_old = fs::exists(old, ec);
			bool back = staged && MoveFileExW(stage.c_str(), out.c_str(), 0);
			DWORD err = back ? 0 : GetLastError();
			if (!back && have_old) { back = MoveFileExW(old.c_str(), out.c_str(), 0); if (!back) err = GetLastError(); }
			if ((staged || have_old) && !back)
			{
				why = hdui::sfmt("cannot put the HD UI pack back into %s (error %lu: a file in %s is in use; close the game and "
					"Explorer windows showing it, then try again)", out.string().c_str(), static_cast<unsigned long>(err),
					(have_old ? old : stage).string().c_str());
				return false;
			}
		}
		fs::remove_all(old, ec);
		fs::remove_all(stage, ec);
		return true;
	}

	// ------------------------------------------------------------------------------------------------ downloads
	inline std::string sha256_file(const fs::path& p)
	{
		BCRYPT_ALG_HANDLE alg = nullptr;
		BCRYPT_HASH_HANDLE h = nullptr;
		std::string hex;
		if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
		if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0)
		{
			if (FILE* f = _wfopen(p.c_str(), L"rb"))
			{
				std::vector<uint8_t> buf(1 << 20);
				size_t n;
				while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), ULONG(n), 0);
				std::fclose(f);
				uint8_t dig[32];
				if (BCryptFinishHash(h, dig, 32, 0) == 0)
					for (uint8_t b : dig) hex += hdui::sfmt("%02x", b);
			}
			BCryptDestroyHash(h);
		}
		BCryptCloseAlgorithmProvider(alg, 0);
		return hex;
	}

	inline bool download(const std::string& url, const fs::path& dest, const char* sha, std::string& why)
	{
		std::error_code ec;
		if (fs::is_regular_file(dest, ec) && sha256_file(dest) == sha) return true;
		out("@progress download 0 0");
		out("downloading %s", url.c_str());
		fs::create_directories(dest.parent_path(), ec);
		const fs::path part = fs::path(dest).concat(".part");
		fs::remove(part, ec);
		const std::wstring wurl(url.begin(), url.end());
		if (URLDownloadToFileW(nullptr, wurl.c_str(), part.c_str(), 0, nullptr) != S_OK)
		{
			why = "could not download " + url + " (no internet connection?)";
			return false;
		}
		const std::string got = sha256_file(part);
		if (got != sha)
		{
			fs::remove(part, ec);
			why = dest.filename().string() + ": the download's SHA-256 is " + got + ", not the expected " + sha + "; not used";
			return false;
		}
		fs::remove(dest, ec);
		fs::rename(part, dest, ec);
		if (ec) { why = "cannot write " + dest.string(); return false; }
		return true;
	}

	// Exit codes of a process Windows could not load (a DLL missing or damaged): the install is broken, not the GPU
	inline bool is_loader_error(int rc)
	{
		const uint32_t c = uint32_t(rc);
		return rc == -1 || c == 0xC0000135u /* DLL not found */ || c == 0xC000007Bu /* bad image */ || c == 0xC0000142u /* DLL init failed */ ||
			c == 0xC0000139u /* entry point not found */;
	}

	// A file of tools\hdui_upscaler that must be there unchanged
	inline bool check_pinned(const fs::path& p, const char* sha, std::string& why)
	{
		std::error_code ec;
		if (!fs::is_regular_file(p, ec))
		{
			why = p.string() + " is missing (it comes with the port: get the port's files again)";
			return false;
		}
		const std::string got = sha256_file(p);
		if (got == sha) return true;
		why = p.string() + " is not the file that comes with the port (SHA-256 " + got + "), so it is not run (get the port's files again)";
		return false;
	}

	// Extracts models\<file>.param / .bin of a Real-ESRGAN release zip (checked) into `md`; true: both are there now
	inline bool extract_official(const fs::path& zip, const Model& m, const fs::path& md, std::string& why)
	{
		std::vector<uint8_t> z;
		if (sha256_file(zip) != official_zip_sha || !read_file(zip, z))
		{
			why = zip.string() + " is not Real-ESRGAN's release zip";
			return false;
		}
		mz_zip_archive za = {};
		if (!mz_zip_reader_init_mem(&za, z.data(), z.size(), 0))
		{
			why = "cannot read " + zip.string();
			return false;
		}
		bool ok = true;
		std::error_code ec;
		fs::create_directories(md, ec);
		for (const auto& [ext, sha] : { std::pair<const char*, const char*>{ ".param", m.param_sha }, { ".bin", m.bin_sha } })
		{
			const std::string entry = std::string("models/") + m.file + ext;
			size_t n = 0;
			void* d = mz_zip_reader_extract_file_to_heap(&za, entry.c_str(), &n, 0);
			const fs::path to = md / (std::string(m.file) + ext), part = fs::path(to).concat(".part");
			ok = ok && d && write_file(part, d, n) && sha256_file(part) == sha;
			if (d) mz_free(d);
			if (ok)
			{
				fs::remove(to, ec);
				fs::rename(part, to, ec);
				ok = !ec;
			}
			fs::remove(part, ec);
		}
		mz_zip_reader_end(&za);
		if (!ok) why = "the model " + std::string(m.file) + " in " + zip.string() + " is not the expected file";
		return ok;
	}

	// The upscaler for this run (tools\hdui_upscaler\bin, checked) and the model's folder: tools\hdui_upscaler\models
	// for the bundled one, <tools>\models for one downloaded the first time (taken from where older versions put it or
	// from the release zip they downloaded, if it is still there). The 2022 Real-ESRGAN exe those versions used is deleted.
	inline bool ensure_upscaler(const fs::path& tools, const fs::path& updir, const Model& m, bool gpu, fs::path& exe, fs::path& model_dir, std::string& why)
	{
		const Pinned& up = gpu ? upscaler_gpu : upscaler_cpu;
		if (!check_pinned(updir / "bin" / up.name, up.sha, why) || !check_pinned(updir / "bin" / upscaler_omp.name, upscaler_omp.sha, why)) return false;
		exe = updir / "bin" / up.name;
		std::error_code ec;
		const fs::path md = tools / "models", old = tools / legacy_dir / "models";
		const auto have = [&](const Model& o)
		{
			return sha256_file(md / (std::string(o.file) + ".param")) == o.param_sha && sha256_file(md / (std::string(o.file) + ".bin")) == o.bin_sha;
		};
		for (const Model& o : models)
			if (o.from != From::bundled)
				for (const auto& [ext, sha] : { std::pair<const char*, const char*>{ ".param", o.param_sha }, { ".bin", o.bin_sha } })
				{
					const fs::path from = old / (std::string(o.file) + ext), to = md / (std::string(o.file) + ext);
					if (fs::is_regular_file(from, ec) && !fs::is_regular_file(to, ec) && sha256_file(from) == sha)
					{
						fs::create_directories(md, ec);
						fs::rename(from, to, ec);
					}
				}
		if (fs::is_regular_file(tools / legacy_zip, ec))
			for (const Model& o : models)
				if (o.from == From::official_zip && !have(o)) extract_official(tools / legacy_zip, o, md, why);
		why.clear();
		fs::remove_all(tools / legacy_dir, ec);
		fs::remove_all(fs::path(tools / legacy_dir).concat(".tmp"), ec);
		fs::remove(tools / legacy_zip, ec);
		if (m.from == From::bundled)
		{
			model_dir = updir / "models";
			return check_pinned(model_dir / (std::string(m.file) + ".param"), m.param_sha, why) &&
				check_pinned(model_dir / (std::string(m.file) + ".bin"), m.bin_sha, why);
		}
		model_dir = md;
		if (m.from == From::official_zip)
		{
			if (have(m)) return true;
			const fs::path zip = tools / (std::string(legacy_zip) + ".download");
			const bool ok = download(official_zip_url, zip, official_zip_sha, why) && extract_official(zip, m, md, why);
			fs::remove(zip, ec);
			return ok;
		}
		return download(std::string(upscayl_models) + m.file + ".param", md / (std::string(m.file) + ".param"), m.param_sha, why) &&
			download(std::string(upscayl_models) + m.file + ".bin", md / (std::string(m.file) + ".bin"), m.bin_sha, why);
	}

	// ------------------------------------------------------------------------------------------------ upscaler runs
	struct Job
	{
		HANDLE job = nullptr;
		Job()
		{
			// the upscaler dies with this process (the launcher's Cancel ends only this one)
			job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
			li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
		}
		~Job() { if (job) CloseHandle(job); }
	};

	inline size_t count_files(const fs::path& d)
	{
		size_t n = 0;
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(d, ec)) n += e.is_regular_file(ec);
		return n;
	}

	// The first line of the upscaler's output (trimmed) for which `match(lower-cased line)` holds; empty: none
	template <typename Match>
	inline bool upscaler_line(const std::string& s, Match match, std::string* line)
	{
		size_t b = 0;
		while (b < s.size())
		{
			size_t e = s.find_first_of("\r\n", b);
			if (e == std::string::npos) e = s.size();
			std::string l = s.substr(b, e - b);
			const size_t f = l.find_first_not_of(" \t");
			l = f == std::string::npos ? std::string() : l.substr(f);
			if (match(lower(l)))
			{
				if (line) *line = l;
				return true;
			}
			b = e + 1;
		}
		return false;
	}
	// The upscaler found no Vulkan GPU it can use: ncnn could not create its Vulkan instance or list the GPUs
	// ("vkCreateInstance failed -9" = VK_ERROR_INCOMPATIBLE_DRIVER), and realesrgan then says "invalid gpu device" and
	// ends with -1. Nothing reached a GPU: no driver fault.
	inline bool upscaler_no_gpu(const std::string& s, std::string* line = nullptr)
	{
		return upscaler_line(s, [](const std::string& l)
		{
			return l.compare(0, 16, "vkcreateinstance") == 0 || l.compare(0, 26, "vkenumeratephysicaldevices") == 0 ||
				l.find("invalid gpu device") != std::string::npos;
		}, line);
	}
	// The GPU was lost while the upscaler used it ("vkQueueSubmit failed -4": VK_ERROR_DEVICE_LOST, the driver reset the
	// GPU). ncnn carries on after one, submitting to the lost device again for every tile and image: on an RTX 5070 that
	// turned one driver reset into a chain of them that froze the PC. Other Vulkan errors (no GPU, out of memory) are
	// not driver faults. `line`: the line that says so.
	inline bool upscaler_gpu_error(const std::string& s, std::string* line = nullptr)
	{
		return upscaler_line(s, [](const std::string& l)
		{
			if (l.find("device lost") != std::string::npos || l.find("device_lost") != std::string::npos || l.compare(0, 9, "gpu_stop:") == 0)
				return true;   // GPU_STOP: our upscaler build stopped itself at a failed GPU submission
			if (l.compare(0, 2, "vk") != 0 || l.compare(0, 16, "vkcreateinstance") == 0 || l.compare(0, 26, "vkenumeratephysicaldevices") == 0)
				return false;
			const size_t f = l.find(" failed -4");
			return f != std::string::npos && (f + 10 == l.size() || !std::isdigit(static_cast<unsigned char>(l[f + 10])));
		}, line);
	}

	// cpu_threads > 0: on the CPU (-g -1) with that many threads; else on the GPU (`gpu`, -1: the upscaler's choice, the
	// first discrete card), conservative: -c -j 1:1:1. tile 0: the upscaler's own tile size. model_dir: the folder of
	// the model's files (empty: "models" next to the exe).
	inline std::wstring upscaler_cmdline(const fs::path& exe, const std::string& model, int gpu, int tile, const fs::path& in, const fs::path& outp,
		const fs::path& model_dir = {}, int cpu_threads = 0, bool conservative = false)
	{
		std::wstring cl = L"\"" + exe.wstring() + L"\" -i \"" + in.wstring() + L"\" -o \"" + outp.wstring() + L"\" -n " +
			std::wstring(model.begin(), model.end()) + L" -s 4 -f png -m \"" + (model_dir.empty() ? exe.parent_path() / "models" : model_dir).wstring() + L"\"";
		if (tile > 0) cl += L" -t " + std::to_wstring(std::max(32, tile));
		if (cpu_threads > 0) return cl + L" -j 1:" + std::to_wstring(cpu_threads) + L":1 -g -1";
		if (gpu >= 0) cl += L" -g " + std::to_wstring(gpu);
		return conservative ? cl + L" -j 1:1:1 -c" : cl;
	}

	// The bytes of a file from `from` on, while another process may still be writing it
	inline std::string read_from(const fs::path& p, uint64_t from)
	{
		std::string s;
		HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (f == INVALID_HANDLE_VALUE) return s;
		LARGE_INTEGER size = {}, at = {};
		at.QuadPart = LONGLONG(from);
		if (GetFileSizeEx(f, &size) && uint64_t(size.QuadPart) > from && SetFilePointerEx(f, at, nullptr, FILE_BEGIN))
		{
			s.resize(size_t(std::min<uint64_t>(uint64_t(size.QuadPart) - from, 1 << 20)));
			DWORD n = 0;
			if (!ReadFile(f, s.data(), DWORD(s.size()), &n, nullptr)) n = 0;
			s.resize(n);
		}
		CloseHandle(f);
		return s;
	}
	// The size of a file another process is writing (the directory entry fs::file_size reads can lag behind)
	inline uint64_t live_size(const fs::path& p)
	{
		HANDLE f = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (f == INVALID_HANDLE_VALUE) return 0;
		LARGE_INTEGER size = {};
		const bool ok = GetFileSizeEx(f, &size) != 0;
		CloseHandle(f);
		return ok ? uint64_t(size.QuadPart) : 0;
	}

	// run_upscaler's own results (the upscaler was ended by the watchdog)
	enum : int { up_gpu_error = -2, up_stalled = -3, up_timeout = -4, up_aborted = -5 };

	// Runs realesrgan-ncnn on a file or folder; while it runs, `tick(files in the output folder)` is called.
	// Its console output goes to `log`. Returns its exit code, -1: could not start, or (after ending it) up_gpu_error:
	// its output showed a Vulkan error, up_stalled: no new output line or file for `stall_s` seconds, up_timeout: still
	// running after `max_s` seconds (0: no limit), up_aborted: `abort` was set. model_dir / cpu_threads / conservative: as
	// upscaler_cmdline (on the CPU it runs at below-normal priority, so the PC stays usable).
	template <typename Tick>
	int run_upscaler(Job& job, const fs::path& exe, const std::string& model, int gpu, int tile, const fs::path& in, const fs::path& outp,
		const fs::path& log, const fs::path& watch, Tick tick, unsigned stall_s = stall_seconds, unsigned max_s = 0, std::string* gpu_line = nullptr,
		const fs::path& model_dir = {}, int cpu_threads = 0, bool conservative = false, const std::atomic<bool>* abort = nullptr)
	{
		std::wstring cl = upscaler_cmdline(exe, model, gpu, tile, in, outp, model_dir, cpu_threads, conservative);
		const uint64_t log0 = live_size(log);   // the log is appended to
		SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
		HANDLE lf = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		STARTUPINFOW si = { sizeof si };
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = si.hStdError = lf;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		PROCESS_INFORMATION pi = {};
		// inherited by the upscaler: a missing / damaged DLL ends it with a loader exit code instead of a modal
		// "vcomp140.dll was not found" box nobody sees behind the launcher
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
		const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED | (cpu_threads > 0 ? BELOW_NORMAL_PRIORITY_CLASS : 0), nullptr,
			exe.parent_path().c_str(), &si, &pi);
		if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
		if (!ok) return -1;
		if (job.job) AssignProcessToJobObject(job.job, pi.hProcess);
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
		size_t last = SIZE_MAX;
		uint64_t log_seen = log0;
		const auto start = std::chrono::steady_clock::now();
		auto progress = start;
		int verdict = 0;
		const auto check_log = [&]
		{
			const uint64_t n = live_size(log);
			if (n == log_seen) return false;
			log_seen = n;
			std::string line;
			if (upscaler_gpu_error(read_from(log, log0), &line))
			{
				verdict = up_gpu_error;
				if (gpu_line) *gpu_line = line;
			}
			return true;
		};
		while (WaitForSingleObject(pi.hProcess, 250) == WAIT_TIMEOUT)
		{
			const auto now = std::chrono::steady_clock::now();
			const size_t n = count_files(watch);
			if (n != last) { tick(last = n); progress = now; }
			if (check_log()) progress = now;
			if (!verdict && stall_s && now - progress > std::chrono::seconds(stall_s)) verdict = up_stalled;
			if (!verdict && max_s && now - start > std::chrono::seconds(max_s)) verdict = up_timeout;
			if (!verdict && abort && abort->load()) verdict = up_aborted;
			if (verdict)
			{
				// nothing more goes to the GPU: the upscaler (and anything it started) is ended at once
				if (!job.job || !TerminateJobObject(job.job, 1)) TerminateProcess(pi.hProcess, 1);
				WaitForSingleObject(pi.hProcess, 15000);
				break;
			}
		}
		if (!verdict) check_log();   // it may have reported the error and ended (with exit code 0) between two looks
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		return verdict ? verdict : int(code);
	}

	// The GPU the upscaler reported: "[0 NVIDIA GeForce RTX 5070]  queueC=..." -> "NVIDIA GeForce RTX 5070"
	inline std::string gpu_name_in(const std::string& log)
	{
		const size_t b = log.find('['), e = b == std::string::npos ? b : log.find(']', b);
		if (b == std::string::npos || e == std::string::npos) return {};
		std::string n = log.substr(b + 1, e - b - 1);
		const size_t sp = n.find(' ');
		return sp == std::string::npos ? n : n.substr(sp + 1);
	}

	inline std::string tail(const fs::path& p, size_t n)
	{
		std::vector<uint8_t> d;
		read_file(p, d);
		std::string s(d.begin(), d.end());
		std::string r;
		for (char c : s.size() > n ? s.substr(s.size() - n) : s)
			if (c != '\r') r += c == '\n' ? ' ' : c;
		return r;
	}

	// Upscales a small image with the run's settings: proves the upscaler and the model work before the long run on the
	// CPU (the GPU run needs none: it falls back to the CPU itself; on a GPU it would say whether a Vulkan GPU is there). gpu_name: what it reported. broken:
	// the upscaler did not load at all (damaged files), so the GPU was not tried. fault: the GPU / driver failed (a Vulkan
	// error, a hang): `why` says so. model_dir / cpu_threads: as upscaler_cmdline (a CPU run never has a fault).
	inline bool probe_gpu(Job& job, const fs::path& exe, const std::string& model, int gpu, const fs::path& work, std::string& gpu_name, std::string& why, bool& broken,
		int tile = safe_settings.tile, bool* fault = nullptr, const fs::path& model_dir = {}, int cpu_threads = 0)
	{
		broken = false;
		if (fault) *fault = false;
		std::error_code ec;
		fs::create_directories(work, ec);
		constexpr int side = 48;
		uint8_t px[side * side * 3];
		for (int i = 0; i < side * side * 3; ++i) px[i] = uint8_t(i * 7);
		const fs::path in = work / "probe.png", o = work / "probe_x4.png", log = work / "probe.log";
		fs::remove(o, ec);
		fs::remove(log, ec);
		stbi_write_png(in.string().c_str(), side, side, 3, px, side * 3);
		std::string gpu_line;
		const unsigned limit = cpu_threads > 0 ? cpu_probe_seconds : stall_seconds;
		const int rc = run_upscaler(job, exe, model, gpu, tile, in, o, log, work, [](size_t) {}, limit, limit, &gpu_line, model_dir, cpu_threads);
		std::vector<uint8_t> d;
		read_file(log, d);
		const std::string s(d.begin(), d.end());
		if (cpu_threads <= 0) gpu_name = gpu_name_in(s);
		int iw = 0, ih = 0, ic = 0;
		stbi_info(o.string().c_str(), &iw, &ih, &ic);
		if (rc == 0 && iw == side * 4 && ih == side * 4) return true;
		if (cpu_threads > 0)
		{
			broken = is_loader_error(rc);
			why = rc == -1 ? "the upscaler could not start or failed: " + tail(log, 300) :
				broken ? hdui::sfmt("the upscaler in %s did not start (exit code 0x%08X: a file of it is missing or damaged)", exe.parent_path().string().c_str(), uint32_t(rc)) :
				rc == up_stalled || rc == up_timeout ? hdui::sfmt("the upscaler did not finish a small test image in %u seconds on the CPU", limit) :
				hdui::sfmt("the upscaler failed on a small test image (exit %d): ", rc) + tail(log, 300);
			return false;
		}
		const std::string no_gpu_why = "no Vulkan GPU the AI upscaler can use was found (it needs a Vulkan 1.1 graphics card and an up-to-date "
			"driver). Upscaler said: " + tail(log, 300);
		// it ran and found no GPU (realesrgan then ends with -1, which looks like a loader failure): nothing reached a
		// GPU, so neither a damaged install nor a driver fault
		if (rc != up_gpu_error && upscaler_no_gpu(s))
		{
			why = no_gpu_why;
			return false;
		}
		broken = is_loader_error(rc);
		if (broken)
		{
			why = hdui::sfmt("the upscaler in %s did not start (exit code 0x%08X: a file of it is missing or damaged)", exe.parent_path().string().c_str(), uint32_t(rc));
			return false;
		}
		if (rc == up_gpu_error || rc == up_stalled || rc == up_timeout)
		{
			if (fault) *fault = true;
			why = rc == up_gpu_error ? "the graphics driver failed on a small test image (" + gpu_line + ")" :
				hdui::sfmt("a small test image was not done after %u seconds (the GPU or its driver hung)", limit);
			return false;
		}
		why = rc == -1 ? "the upscaler could not start" : no_gpu_why;
		return false;
	}

	// ------------------------------------------------------------------------------------------------ earlier failures
	inline uint64_t unix_now() { return uint64_t(std::time(nullptr)); }
	inline uint64_t boot_unix() { return unix_now() - GetTickCount64() / 1000; }

	// gpu_fault.txt: "gpu\t<what happened>": an earlier run failed on the graphics card (older versions wrote their
	// settings name first). Any content counts.
	struct Fault
	{
		bool any = false;
		std::string why;
	};
	inline Fault read_fault(const fs::path& tools)
	{
		Fault f;
		std::vector<uint8_t> d;
		if (!read_file(tools / fault_file, d)) return f;
		f.any = true;
		std::string s(d.begin(), d.end());
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		const size_t t = s.find('\t');
		f.why = t == std::string::npos ? s : s.substr(t + 1);
		return f;
	}
	inline void write_fault(const fs::path& tools, const std::string& why)
	{
		std::error_code ec;
		fs::create_directories(tools, ec);
		const std::string s = "gpu\t" + why + "\n";
		write_file(tools / fault_file, s.data(), s.size());
	}

	// Whether Windows went down without a clean shutdown while the upscaler ran (`since`, unix seconds: the marker's
	// time). Windows logs that at its next start, as Kernel-Power 41 ("rebooted without cleanly shutting down": a freeze,
	// a hard reset, a power cut) or EventLog 6008 ("the previous system shutdown was unexpected"). Only the first start
	// after `since` (Kernel-General 12 / EventLog 6005) counts: a 41 / 6008 up to boot_log_window seconds after it. A
	// later one (days after a clean restart that left the marker) is about another shutdown.
	struct BootEvent
	{
		uint64_t time;   // unix seconds
		bool start;      // Windows started (12 / 6005); otherwise an unexpected shutdown was logged (41 / 6008)
	};
	constexpr uint64_t boot_log_window = 300;
	// 1: it went down unexpectedly, 0: it did not (a normal restart, shutdown or sign-out), -1: the start after `since`
	// is not in `events` (the log was cleared or wrapped): not known
	inline int unexpected_shutdown_in(const std::vector<BootEvent>& events, uint64_t since)
	{
		uint64_t first = UINT64_MAX;
		for (const BootEvent& e : events)
			if (e.start && e.time >= since) first = std::min(first, e.time);
		if (first == UINT64_MAX) return -1;
		for (const BootEvent& e : events)
			if (!e.start && e.time >= since && e.time <= first + boot_log_window) return 1;
		return 0;
	}
	// The same, from Windows' System log. -1 also: it could not be read.
	inline int unexpected_shutdown_since(uint64_t since)
	{
		const time_t t = time_t(since);
		tm u = {};
		if (gmtime_s(&u, &t)) return -1;
		wchar_t q[768];
		swprintf(q, 768, L"*[System[((Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41) or (Provider[@Name='EventLog'] and "
			L"(EventID=6008 or EventID=6005)) or (Provider[@Name='Microsoft-Windows-Kernel-General'] and EventID=12)) and "
			L"TimeCreated[@SystemTime>='%04d-%02d-%02dT%02d:%02d:%02d.000Z']]]",
			u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
		EVT_HANDLE h = EvtQuery(nullptr, L"System", q, EvtQueryChannelPath | EvtQueryForwardDirection);
		if (!h) return -1;
		EVT_HANDLE ctx = EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
		std::vector<BootEvent> events;
		std::vector<uint8_t> buf(4096);
		uint64_t first = UINT64_MAX;
		bool ok = ctx != nullptr;
		while (ok && events.size() < 4096)
		{
			EVT_HANDLE ev[32];
			DWORD n = 0;
			if (!EvtNext(h, 32, ev, 5000, 0, &n))
			{
				ok = GetLastError() == ERROR_NO_MORE_ITEMS;
				break;
			}
			for (DWORD i = 0; i < n; ++i)
			{
				DWORD used = 0, props = 0;
				BOOL r = EvtRender(ctx, ev[i], EvtRenderEventValues, DWORD(buf.size()), buf.data(), &used, &props);
				if (!r && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
				{
					buf.resize(used);
					r = EvtRender(ctx, ev[i], EvtRenderEventValues, DWORD(buf.size()), buf.data(), &used, &props);
				}
				if (r && props > EvtSystemTimeCreated)
				{
					const EVT_VARIANT* v = reinterpret_cast<const EVT_VARIANT*>(buf.data());
					const uint16_t id = v[EvtSystemEventID].UInt16Val;
					const uint64_t ft = v[EvtSystemTimeCreated].FileTimeVal;
					const uint64_t ut = ft > 116444736000000000ull ? (ft - 116444736000000000ull) / 10000000ull : 0;
					const BootEvent e{ ut, id == 12 || id == 6005 };
					if (e.start && ut >= since) first = std::min(first, ut);
					events.push_back(e);
				}
				EvtClose(ev[i]);
			}
			// in time order: past the first start's window nothing more counts
			if (first != UINT64_MAX && !events.empty() && events.back().time > first + boot_log_window) break;
		}
		if (ctx) EvtClose(ctx);
		EvtClose(h);
		return ok ? unexpected_shutdown_in(events, since) : -1;
	}
	using ShutdownCheck = int (*)(uint64_t since);

	// What the previous GPU run left behind when it did not end by itself.
	//   - An upscaler log in the work folder (cleared when a run ends) that shows the GPU lost.
	//   - The marker of a GPU run that never returned, written before the PC last started (a marker from this boot is a
	//     Cancel or a killed run). A crash if Windows logged an unexpected shutdown at its next start, and also when that
	//     cannot be told (the System log cleared, wrapped or not readable, or the marker unreadable): not knowing counts
	//     as a crash. A normal restart, shutdown or sign-out with the launcher open is none.
	struct Crash
	{
		std::string why;             // empty: none
		bool stale_marker = false;   // the marker of an earlier boot is there
	};
	inline Crash previous_crash(const fs::path& tools, uint64_t boot, ShutdownCheck shutdown_check = unexpected_shutdown_since)
	{
		Crash c;
		std::vector<uint8_t> d;
		uint64_t t = 0;
		const bool marker = read_file(tools / running_marker, d);
		if (marker) t = std::strtoull(std::string(d.begin(), d.end()).c_str(), nullptr, 10);
		for (const fs::path& p : { tools / "work" / "upscaler.log", tools / "work" / "probe" / "probe.log" })
		{
			std::string line;
			if (read_file(p, d) && upscaler_gpu_error(std::string(d.begin(), d.end()), &line))
			{
				c.why = "the AI upscaler failed on the graphics card with " + line;
				c.stale_marker = marker;
				return c;
			}
		}
		if (!marker || (t && t + 30 >= boot)) return c;
		c.stale_marker = true;
		const int down = t ? shutdown_check(t) : -1;
		if (down > 0) c.why = "the PC stopped or restarted unexpectedly while the AI upscaler was running on the graphics card";
		else if (down < 0)
			c.why = "the AI upscaler was running on the graphics card when Windows last started, and Windows' log does not tell "
				"whether the PC crashed then";
		return c;
	}

	// Where the next run upscales, after what earlier runs left in `tools`
	struct Plan
	{
		bool fault = false;   // gpu_fault.txt is there or a crash was found now: the CPU unless --use-gpu
		std::string note;     // why
		std::string crash;    // a crash of the previous run found now (written to gpu_fault.txt when the run starts)
		bool stale_marker = false;
	};
	inline Plan plan_gpu(const fs::path& tools, uint64_t boot, ShutdownCheck shutdown_check = unexpected_shutdown_since)
	{
		Plan p;
		const Fault f = read_fault(tools);
		const Crash c = previous_crash(tools, boot, shutdown_check);
		p.crash = c.why;
		p.stale_marker = c.stale_marker;
		p.fault = f.any || !c.why.empty();
		if (p.fault)
			p.note = "An earlier run of the AI upscaler on the graphics card failed (" +
				(!c.why.empty() ? c.why : !f.why.empty() ? f.why : "see " + (tools / fault_file).string()) + ").";
		return p;
	}
	// The run after a fault keeps the marker and the upscaler's log that showed it (upscaler_crashed.*), beside
	// gpu_fault.txt; the work folder is cleared by every run
	inline void keep_crash_evidence(const fs::path& tools)
	{
		std::error_code ec;
		if (fs::is_regular_file(tools / running_marker, ec))
		{
			fs::remove(tools / crashed_marker, ec);
			fs::rename(tools / running_marker, tools / crashed_marker, ec);
		}
		for (const fs::path& p : { tools / "work" / "upscaler.log", tools / "work" / "probe" / "probe.log" })
			if (fs::is_regular_file(p, ec))
			{
				fs::remove(tools / crashed_log, ec);
				fs::rename(p, tools / crashed_log, ec);
				break;
			}
	}
	// A run with --use-gpu that made everything on the graphics card: the earlier failure no longer holds later runs to
	// the CPU. true: there was one.
	inline bool forget_fault(const fs::path& tools)
	{
		std::error_code ec;
		return fs::remove(tools / fault_file, ec);
	}

	// ------------------------------------------------------------------------------------------------ CPU or GPU
	struct Backend
	{
		bool gpu = false;   // the graphics card first (the CPU for what it does not make); else the CPU only
		std::string note;   // why the CPU, or the GPU again after a failure
	};
	// The graphics card unless --cpu, or an earlier run failed there (gpu_fault.txt / a crash found now) and --use-gpu
	// was not given
	inline Backend choose_backend(bool use_gpu, bool use_cpu, const Plan& plan, const fs::path& tools)
	{
		Backend b;
		if (use_cpu) return b;
		if (plan.fault && !use_gpu)
		{
			b.note = plan.note + " So it runs on the CPU (slower). After a graphics driver update, delete " + (tools / fault_file).string() +
				" (or run HDUIPack.exe --use-gpu) to use the graphics card again.";
			return b;
		}
		b.gpu = true;
		if (plan.fault) b.note = plan.note + " --use-gpu: the graphics card is tried again; if it fails, the rest is made on the CPU.";
		return b;
	}
	// CPU threads for the upscaler: about the physical cores (more gained nothing on a 6-core / 12-thread CPU), at most 16
	inline int cpu_threads(unsigned logical)
	{
		return int(std::clamp(logical / 2, 1u, 16u));
	}

	// ------------------------------------------------------------------------------------------------ pictures
	// One texture's input picture(s) for the upscaler: in\<key>.png (+ <key>_alpha.png), at least min_side a side
	// (padded around the texture with its edge pixels), cropped back to the texture after it
	struct Pic
	{
		std::string key;
		bool alpha = false;
		uint32_t w = 0, h = 0;                     // the texture
		uint32_t pw = 0, ph = 0, px = 0, py = 0;   // the padded picture and where the texture is in it
	};
	inline Pic pic_layout(const std::string& key, uint32_t w, uint32_t h, bool alpha)
	{
		Pic p;
		p.key = key;
		p.alpha = alpha;
		p.w = w;
		p.h = h;
		p.pw = std::max(w, min_side);
		p.ph = std::max(h, min_side);
		p.px = (p.pw - w) / 2;
		p.py = (p.ph - h) / 2;
		return p;
	}
	// `src` (the texture's size, `ch` bytes a pixel) in the padded picture, its edge pixels repeated around it
	inline std::vector<uint8_t> pad_edge(const std::vector<uint8_t>& src, const Pic& p, int ch)
	{
		std::vector<uint8_t> o(size_t(p.pw) * p.ph * ch);
		for (uint32_t y = 0; y < p.ph; ++y)
		{
			const uint32_t sy = uint32_t(std::clamp(int(y) - int(p.py), 0, int(p.h) - 1));
			for (uint32_t x = 0; x < p.pw; ++x)
			{
				const uint32_t sx = uint32_t(std::clamp(int(x) - int(p.px), 0, int(p.w) - 1));
				std::memcpy(&o[(size_t(y) * p.pw + x) * ch], &src[(size_t(sy) * p.w + sx) * ch], ch);
			}
		}
		return o;
	}
	// rgb: 3 bytes a pixel; alpha (with p.alpha): 1 byte a pixel
	inline bool write_pic(const Pic& p, const fs::path& in, const std::vector<uint8_t>& rgb, const std::vector<uint8_t>& alpha)
	{
		const std::vector<uint8_t> c = pad_edge(rgb, p, 3);
		if (!stbi_write_png((in / (p.key + ".png")).string().c_str(), int(p.pw), int(p.ph), 3, c.data(), int(p.pw) * 3)) return false;
		if (!p.alpha) return true;
		const std::vector<uint8_t> a = pad_edge(alpha, p, 1);
		std::vector<uint8_t> a3(a.size() * 3);
		for (size_t i = 0; i < a.size(); ++i) a3[i * 3] = a3[i * 3 + 1] = a3[i * 3 + 2] = a[i];
		return stbi_write_png((in / (p.key + "_alpha.png")).string().c_str(), int(p.pw), int(p.ph), 3, a3.data(), int(p.pw) * 3) != 0;
	}
	// The upscaled picture(s) of `p` in `up`, cropped to the texture (4w x 4h): rgb 3 bytes, alpha 1 byte a pixel.
	// false: missing, not complete or not 4x the input.
	inline bool load_pic(const Pic& p, const fs::path& up, std::vector<uint8_t>& rgb, std::vector<uint8_t>& alpha)
	{
		const auto load = [&](const std::string& name, int ch, std::vector<uint8_t>& o)
		{
			int w = 0, h = 0, c = 0;
			stbi_uc* d = stbi_load((up / name).string().c_str(), &w, &h, &c, ch);
			const bool ok = d && uint32_t(w) == p.pw * 4 && uint32_t(h) == p.ph * 4;
			if (ok)
			{
				o.resize(size_t(p.w) * 4 * p.h * 4 * ch);
				for (uint32_t y = 0; y < p.h * 4; ++y)
					std::memcpy(&o[size_t(y) * p.w * 4 * ch], d + ((size_t(p.py) * 4 + y) * p.pw * 4 + size_t(p.px) * 4) * ch, size_t(p.w) * 4 * ch);
			}
			if (d) stbi_image_free(d);
			return ok;
		};
		return load(p.key + ".png", 3, rgb) && (!p.alpha || load(p.key + "_alpha.png", 1, alpha));
	}
	inline void remove_pic(const Pic& p, const fs::path& in, const fs::path& up)
	{
		std::error_code ec;
		for (const fs::path& d : { in, up })
		{
			fs::remove(d / (p.key + ".png"), ec);
			fs::remove(d / (p.key + "_alpha.png"), ec);
		}
	}

	// ------------------------------------------------------------------------------------------------ the GPU run
	struct GpuRun
	{
		int rc = 0;                // run_upscaler's result
		std::string line;          // the Vulkan error it saw
		size_t made = 0;
		bool aborted = false;      // `done` asked to stop (its error is printed)
		std::vector<char> done;    // per picture
	};
	// Every picture of `in` in ONE upscaler run on the GPU. While it runs (and once more after it), done(i) is called for
	// each picture whose output files are there and no longer growing: 1 = used, 0 = not usable (yet), -1 = stop all.
	// tick(pictures made) after each look. Nothing is sent to the GPU after a Vulkan error, a stall or an abort: the
	// watchdog ends the upscaler.
	template <typename Done, typename Tick>
	GpuRun gpu_pass(Job& job, const fs::path& exe, const std::string& model, int gpu, const Settings& st, const fs::path& in, const fs::path& up,
		const fs::path& log, const fs::path& model_dir, const std::vector<Pic>& pics, Done done, Tick tick, unsigned stall_s = stall_seconds)
	{
		GpuRun r;
		r.done.assign(pics.size(), 0);
		std::atomic<bool> ended{ false }, abort{ false };
		std::thread th([&]
		{
			r.rc = run_upscaler(job, exe, model, gpu, st.tile, in, up, log, up, [](size_t) {}, stall_s, 0, &r.line, model_dir, 0, st.conservative, &abort);
			ended = true;
		});
		struct Stop
		{
			std::thread& t;
			std::atomic<bool>& abort;
			~Stop()
			{
				abort = true;
				if (t.joinable()) t.join();
			}
		} stop{ th, abort };
		std::vector<uint64_t> seen(pics.size(), 0);
		for (;;)
		{
			const bool last = ended.load();
			if (last) th.join();
			for (size_t i = 0; i < pics.size(); ++i)
			{
				if (r.done[i]) continue;
				const uint64_t a = live_size(up / (pics[i].key + ".png")), b = pics[i].alpha ? live_size(up / (pics[i].key + "_alpha.png")) : 1;
				if (!a || !b) continue;
				if (!last && seen[i] != a + b)
				{
					seen[i] = a + b;
					continue;
				}
				const int d = done(i);
				if (d < 0)
				{
					r.aborted = true;
					return r;
				}
				if (d > 0)
				{
					r.done[i] = 1;
					r.made++;
				}
			}
			tick(r.made);
			if (last) return r;
			Sleep(500);
		}
	}
	// What a GPU run that did not make every picture means. fault: the graphics card / driver failed (a Vulkan error, a
	// hang, a crash of the upscaler, no output at all): recorded, later runs use the CPU. Otherwise (no Vulkan GPU, the
	// upscaler did not load, a few pictures did not come out) nothing reached the driver in a bad way. what: empty =
	// everything was made.
	struct GpuOutcome
	{
		bool fault = false;
		bool unavailable = false;   // no Vulkan GPU / the upscaler did not start: nothing ran on the graphics card
		std::string what;
	};
	inline GpuOutcome gpu_outcome(int rc, const std::string& log, size_t made, size_t total, const std::string& line)
	{
		GpuOutcome g;
		std::string l;
		const bool vk = upscaler_gpu_error(log, &l);
		if (rc == 0 && made >= total && !vk) return g;
		g.fault = true;
		if (rc == up_gpu_error || vk) g.what = rc == up_gpu_error && !line.empty() ? line : l;
		else if (rc == up_stalled) g.what = hdui::sfmt("no progress for %u seconds", stall_seconds);
		else if (upscaler_no_gpu(log, &l))
		{
			g.fault = false;
			g.unavailable = true;
			g.what = "no Vulkan graphics card the upscaler can use: " + l;
		}
		else if (is_loader_error(rc))
		{
			g.fault = false;
			g.unavailable = true;
			g.what = hdui::sfmt("the GPU upscaler did not start (exit code 0x%08X)", uint32_t(rc));
		}
		else if (rc != 0) g.what = hdui::sfmt("the upscaler ended with exit code %d", rc);
		else if (made == 0) g.what = "the upscaler ended without making any picture";
		else
		{
			g.fault = false;
			g.what = hdui::sfmt("%zu of %zu pictures did not come out", total - made, total);
		}
		return g;
	}

	// upscaler_running.txt exists only while the upscaler runs on the GPU: if the PC does not survive it, the next run
	// sees it (a CPU run cannot stop the graphics driver, so it leaves no marker)
	struct Busy
	{
		fs::path p;
		explicit Busy(const fs::path& tools) : p(tools / running_marker)
		{
			const std::string s = std::to_string(unix_now()) + "\tgpu\n";
			write_file(p, s.data(), s.size());
		}
		~Busy() { std::error_code e; fs::remove(p, e); }
	};

	// gpu_unavailable.txt is there (no usable GPU last time) and the pack has textures the CPU made with the fast model:
	// one small test image on the graphics card (the probe, with its watchdog and the running marker) tells whether it
	// works now (a new driver, another card). gpu_ok: the file is deleted, those textures are made again on it. fault: as
	// a failed GPU run (gpu_fault.txt, upscaler_crashed.log): the CPU from now on. unavailable: the file stays.
	enum class Recheck { gpu_ok, unavailable, fault };
	inline Recheck recheck_gpu(Job& job, const fs::path& exe, const std::string& model, int gpu, const fs::path& tools, const fs::path& work,
		const fs::path& model_dir, std::string& why)
	{
		std::string name;
		bool broken = false, fault = false, ok = false;
		{
			Busy busy(tools);
			ok = probe_gpu(job, exe, model, gpu, work, name, why, broken, safe_settings.tile, &fault, model_dir);
		}
		std::error_code ec;
		// the probe's log must not outlive this run: the next one would take a Vulkan error in it for a new crash (a PC
		// that does not survive the probe leaves it, and the marker, as evidence)
		struct Gone
		{
			fs::path p;
			~Gone() { std::error_code e; fs::remove_all(p, e); }
		} gone{ work };
		if (ok)
		{
			fs::remove(tools / unavailable_file, ec);
			return Recheck::gpu_ok;
		}
		if (!fault) return Recheck::unavailable;
		write_fault(tools, why);
		fs::remove(tools / crashed_log, ec);
		fs::rename(work / "probe.log", tools / crashed_log, ec);
		return Recheck::fault;
	}

	// Without --model: whether this run remakes the textures an earlier run made on the CPU with the fast model. Only
	// when the graphics card is used and its upscaler and model are in place (`gpu_ready`: not when their download
	// failed), and no earlier run found no usable GPU there (gpu_unavailable.txt, ignored with --use-gpu): else every run
	// would make the whole pack again on the CPU.
	inline bool remake_cpu_textures(bool gpu_ready, bool use_gpu, const fs::path& tools)
	{
		std::error_code ec;
		return gpu_ready && (use_gpu || !fs::exists(tools / unavailable_file, ec));
	}

	// What run() does with a texture that has row `m` (nullptr: none) in pack.tsv: a texture made with another model is
	// kept unless --model (`model`) asks for this one (or --force); without --model, a run on the GPU remakes what the CPU
	// made with the fast model when the GPU could not be used (`remake_fast`); a stale row (an unchecked picture of an
	// earlier version's GPU pipeline) is always made again; a refused one is left alone unless --force
	enum class Row { make, keep, leave };
	inline Row row_plan(const Made* m, const fs::path& out, bool force, const char* model, bool remake_fast)
	{
		std::error_code ec;
		if (force || !m) return Row::make;
		if (refused_row(*m)) return Row::leave;
		const bool keep = !stale_row(*m) && !m->file.empty() && fs::is_regular_file(out / m->file, ec) &&
			(model ? m->model == model : !(remake_fast && m->model == cpu_default_model));
		return keep ? Row::keep : Row::make;
	}

	// ------------------------------------------------------------------------------------------------ main flow
	struct Options
	{
		fs::path root, usrdir, out, tools, upscaler;   // upscaler: tools\hdui_upscaler (bin\, models\)
		std::string model;   // empty: x4plus on the GPU, fast on the CPU
		int gpu = -1;
		bool use_gpu = false, use_cpu = false, force = false, list = false, dry = false, remove = false, safe = false, status = false;
		size_t limit = 0;
	};

	inline fs::path find_usrdir(const fs::path& root, const fs::path& given) { return hdgame::find_usrdir(root, given); }

	inline int run(const Options& o)
	{
		std::error_code ec;
		const fs::path stage = stage_dir(o.out), work = o.tools / "work";
		const Plan plan = plan_gpu(o.tools, boot_unix());
		const Backend be = choose_backend(o.use_gpu, o.use_cpu, plan, o.tools);
		if (o.status)
		{
			out("@gpu %s %s", be.gpu ? "gpu" : "cpu", be.note.c_str());
			return 0;
		}
		if (!o.dry && !o.list)
		{
			// kept for later runs: the fault, and the marker / log that showed it (the work folder is cleared below); a
			// marker of an earlier boot that was no crash (a clean restart or sign-out during a run) is only cleared
			if (!plan.crash.empty())
			{
				write_fault(o.tools, plan.crash);
				keep_crash_evidence(o.tools);
			}
			else if (plan.stale_marker) fs::remove(o.tools / running_marker, ec);
		}
		std::string rwhy;
		// where older versions staged it (a pack they could not move in, e.g. from another drive, is left there)
		recover_pack(o.out, o.tools / "hd_ui.stage", rwhy);
		fs::remove_all(o.tools / "hd_ui.removed", ec);
		fs::remove_all(removed_dir(o.out), ec);   // a --remove ended while deleting
		rwhy.clear();
		const bool recovered = recover_pack(o.out, stage, rwhy);
		if (o.remove)
		{
			if (!recovered)
			{
				// nothing to put back: the pack is being removed (whatever is still in use stays for the next run)
				fs::remove_all(stage, ec);
				fs::remove_all(fs::path(stage).concat(".old"), ec);
			}
			fs::remove_all(work, ec);
			if (fs::exists(o.out, ec))
			{
				const fs::path gone = removed_dir(o.out);
				fs::remove_all(gone, ec);
				if (!MoveFileExW(o.out.c_str(), gone.c_str(), 0)) { out("@error cannot remove %s (is the game running?)", o.out.string().c_str()); return 2; }
				fs::remove_all(gone, ec);
			}
			out("@summary The HD UI pack was removed.");
			return 0;
		}
		if (!recovered) { out("@error %s", rwhy.c_str()); return 2; }
		const Model* model = find_model(o.model.empty() ? (be.gpu ? gpu_default_model : cpu_default_model) : o.model);
		if (!model) { out("@error unknown model %s (x4plus, fast, remacri or ultrasharp)", o.model.c_str()); return 2; }
		// on the CPU after the GPU failed: the model asked for, else the fast one (x4plus is about 30x slower on the CPU)
		const Model* cpu_model = o.model.empty() ? find_model(cpu_default_model) : model;
		if (o.usrdir.empty())
		{
			out("@error %s", hdgame::text_no_game().c_str());
			return 2;
		}

		const Scan sc = scan_archives(o.usrdir, !o.list);
		if (o.list)
		{
			for (const auto& [k, s] : sc.textures)
				out("%016llx\t%u\t%u\t%s\t%u\t%s\t%s", static_cast<unsigned long long>(k), s.tex.w, s.tex.h, hdui::fmt_name(s.tex.fmt), s.tex.mips,
					fs::path(s.archive).filename().string().c_str(), s.header.c_str());
			return 0;
		}
		if (sc.textures.empty())
		{
			out("@error %s", hdgame::text_no_textures(o.usrdir, sc.archives).c_str());
			return 2;
		}
		auto manifest = read_manifest(o.out / "pack.tsv");
		// the GPU upscaler and its model (downloaded the first time); without them everything is made on the CPU
		fs::path gpu_exe, gpu_model_dir;
		std::string gpu_why;
		bool gpu_ready = be.gpu && (o.dry || ensure_upscaler(o.tools, o.upscaler, *model, true, gpu_exe, gpu_model_dir, gpu_why));
		Job job;
		if (gpu_ready && !o.dry && o.model.empty() && !o.use_gpu && !o.force && fs::exists(o.tools / unavailable_file, ec) &&
			std::any_of(manifest.begin(), manifest.end(), [](const auto& m) { return m.second.model == cpu_default_model; }))
		{
			out("checking whether the graphics card can be used now (it could not last time)");
			std::string w;
			switch (recheck_gpu(job, gpu_exe, model->file, o.gpu, o.tools, work / "probe", gpu_model_dir, w))
			{
			case Recheck::gpu_ok:
				out("the graphics card works now: the textures made on the CPU are made again with %s", model->name);
				break;
			case Recheck::fault:
				out("@warn The AI upscaler failed on the graphics card with a small test image (%s): later runs use the CPU (delete "
					"tools\\upscaler\\%s or run HDUIPack.exe --use-gpu to try it again).", w.c_str(), fault_file);
				gpu_ready = false;
				gpu_why = "it failed with a small test image";
				break;
			default:
				out("the graphics card still cannot be used (%s): the textures made on the CPU are kept", w.c_str());
				break;
			}
		}
		const bool remake_fast = o.model.empty() && remake_cpu_textures(gpu_ready, o.use_gpu, o.tools);
		std::vector<const Source*> todo;
		unsigned kept = 0, left = 0;
		for (const auto& [k, s] : sc.textures)
		{
			const auto it = manifest.find(k);
			Row r = row_plan(it == manifest.end() ? nullptr : &it->second, o.out, o.force, o.model.empty() ? nullptr : model->name, remake_fast);
			// a loose image made before its files were tagged (pack_name) is made again: the renderer would not look for it
			if (r == Row::keep && s.image && it->second.file.find("_image_x") == std::string::npos) r = Row::make;
			if (r == Row::leave) left++;
			else if (r == Row::keep) kept++;
			else todo.push_back(&s);
		}
		// small art (icons, prompts, badges) first: an interrupted run already has the most visible part done
		std::stable_sort(todo.begin(), todo.end(), [](const Source* a, const Source* b) { return size_t(a->tex.w) * a->tex.h < size_t(b->tex.w) * b->tex.h; });
		if (o.limit && todo.size() > o.limit) todo.resize(o.limit);
		out("%zu menu / HUD textures in %u assets of %u archives; %u already in the pack, %zu to make with %s%s",
			sc.textures.size(), sc.assets, sc.archives, kept, todo.size(), (be.gpu && !gpu_ready ? cpu_model : model)->name,
			left ? hdui::sfmt(" (%u the upscaler could not make: their original textures are used; --force tries them again)", left).c_str() : "");
		for (const auto& [why, n] : sc.skipped) out("  not packed: %u (%s)", n, why.c_str());
		const int threads = cpu_threads(std::max(1u, std::thread::hardware_concurrency()));
		if (!be.note.empty()) out("@warn %s", be.note.c_str());
		double px_total = 0;
		for (const Source* s : todo) px_total += double(s->tex.w) * s->tex.h;
		// about 1.5x the colour pixels: the alpha of most UI art goes through the model too
		const auto cpu_minutes = [&](const Model& m, double px) { return std::max(1.0, px * 1.5 / m.cpu_px_s / 60); };
		if (be.gpu)
			out("on the graphics card: one upscaler run for all %zu textures (%s); what it does not make is made on the CPU with %s "
				"(about %.0f min for all on a 6-core CPU)", todo.size(), o.safe ? "--safe: conservative mode, 64 px tiles, one image at a time" :
				"the upscaler's own tile size", cpu_model->name, cpu_minutes(*cpu_model, px_total));
		else
			out("on the CPU (%d threads, %d px tiles, %zu textures per upscaler run): about %.0f min on a 6-core CPU", threads, cpu_settings.tile,
				cpu_settings.batch, cpu_minutes(*model, px_total));
		if (o.dry) return 0;
		if (todo.empty())
		{
			out("@summary The HD UI pack is up to date: %u textures in %s.", kept, o.out.string().c_str());
			return 0;
		}

		fs::remove_all(work, ec);
		struct Cleanup
		{
			fs::path p;
			~Cleanup() { std::error_code e; fs::remove_all(p, e); }
		} cleanup{ work };
		const fs::path in = work / "in", up = work / "up", log = work / "upscaler.log";
		std::string why;
		const auto t0 = std::chrono::steady_clock::now();
		unsigned made = 0, failed = 0;
		uint64_t bytes = 0;
		const unsigned total = unsigned(todo.size());
		// the upscaler's time goes with the pixel count (small textures go first), so the ETA is weighted by pixels, per
		// phase (GPU / CPU)
		double px_done = 0, phase_px0 = 0;
		auto phase_t0 = t0;
		unsigned shown = UINT_MAX;
		const auto progress = [&](unsigned d)
		{
			if (d == shown) return;
			shown = d;
			out("@progress upscale %u %u", std::min(d, total), total);
			const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_t0).count(), pd = px_done - phase_px0;
			if (pd > 0 && d < total && el >= 3) out("@eta %u", unsigned(el * (px_total - px_done) / pd + 0.5));
		};
		const auto new_phase = [&]
		{
			phase_t0 = std::chrono::steady_clock::now();
			phase_px0 = px_done;
		};
		// a texture's input picture(s) in `in`
		const auto prepare = [&](const Source& s, Pic& pic)
		{
			std::vector<uint8_t> tb;
			if (!hdgame::read_texel(s, tb)) return false;
			const Rgba img = decode(tb.data() + s.tex.data_off, s.tex);
			pic = pic_layout(hdui::sfmt("%016llx", static_cast<unsigned long long>(s.tex.key)), s.tex.w, s.tex.h, has_alpha(img));
			std::vector<uint8_t> a;
			if (pic.alpha)
			{
				a.resize(size_t(s.tex.w) * s.tex.h);
				for (size_t p = 0; p < a.size(); ++p) a[p] = img[p * 4 + 3];
			}
			return write_pic(pic, in, bleed_rgb(img, s.tex.w, s.tex.h), a);
		};
		// its upscaled picture(s) -> its DDS, committed into the pack every 32 textures / 30 seconds. 1: done; 0: not
		// (yet) readable; -1: not like its source (another texture's picture or garbage): its pictures are deleted; on the
		// GPU the texture is left for the CPU, on the CPU (`last`) it gets a refused row and loses any old file
		std::vector<NewFile> files;
		unsigned refused = 0, refused_cpu = 0;
		auto last_commit = std::chrono::steady_clock::now();
		const auto source_of = [](const TexInfo& t) { return hdui::sfmt("%ux%u %s m%u", t.w, t.h, hdui::fmt_name(t.fmt), t.mips); };
		// a texture that could not be made: the old file of a stale row goes (its picture is not trusted), the game uses
		// the original texture
		const auto drop = [&](const Source& s)
		{
			const auto it = manifest.find(s.tex.key);
			if (it != manifest.end() && stale_row(it->second)) files.push_back(NewFile{ s.tex.key, {}, {}, Made{} });
		};
		const auto finish = [&](const Source& s, const Pic& pic, const Model& m, bool last)
		{
			std::vector<uint8_t> rgb, a;
			if (!load_pic(pic, up, rgb, a)) return 0;
			const TexInfo& t = s.tex;
			const uint32_t sc4 = pick_scale(t.w, t.h, t.fmt);
			const Rgba hd = combine(rgb, pic.alpha ? &a : nullptr, t.w * 4, t.h * 4, t.w * sc4, t.h * sc4, t.fmt == hdui::GCM_DXT1);
			std::vector<uint8_t> tb;
			if (!hdgame::read_texel(s, tb) || !like_source(hd, t.w * sc4, t.h * sc4, decode(tb.data() + t.data_off, t), t.w, t.h))
			{
				out("  %016llx (%ux%u): the upscaled picture does not match the texture; not used%s", static_cast<unsigned long long>(t.key),
					t.w, t.h, last ? " (the game uses the original texture)" : "");
				remove_pic(pic, in, up);
				refused++;
				if (last)
				{
					refused_cpu++;
					files.push_back(NewFile{ t.key, {}, {}, Made{ {}, refused_model, source_of(t) } });
				}
				return -1;
			}
			const std::string name = pack_name(t.key, t.w, t.h, t.fmt, t.mips, sc4, s.image);
			NewFile f{ t.key, name, make_dds(hd, t.w, t.h, t.fmt, t.mips, sc4), Made{ name, m.name, source_of(t) } };
			bytes += f.data.size();
			files.push_back(std::move(f));
			made++;
			px_done += double(t.w) * t.h;
			remove_pic(pic, in, up);
			return 1;
		};
		const auto commit = [&](bool now)
		{
			if (files.empty() || (!now && files.size() < 32 && std::chrono::steady_clock::now() - last_commit < std::chrono::seconds(30))) return true;
			if (!commit_pack(o.out, stage, files, manifest, why))
			{
				out("@error %s", why.c_str());
				return false;
			}
			files.clear();
			last_commit = std::chrono::steady_clock::now();
			return true;
		};
		// ---- the graphics card: every texture in one upscaler run
		std::vector<const Source*> rest;   // for the CPU
		std::string gpu_where;
		bool gpu_all = false;
		if (be.gpu)
		{
			const fs::path& exe = gpu_exe;
			const fs::path& model_dir = gpu_model_dir;
			if (!gpu_ready)
			{
				out("@warn The graphics card is not used: %s. Everything is made on the CPU.", gpu_why.c_str());
				rest = todo;
			}
			else
			{
				out("@progress upscale 0 %u", total);
				fs::create_directories(in, ec);
				fs::create_directories(up, ec);
				std::vector<Pic> pics;
				std::vector<const Source*> srcs;
				for (const Source* s : todo)
				{
					Pic pic;
					if (!prepare(*s, pic))
					{
						drop(*s);
						failed++;
						continue;
					}
					pics.push_back(pic);
					srcs.push_back(s);
				}
				out("upscaling %zu textures with %s on the graphics card (one upscaler run%s)", pics.size(), model->name,
					o.safe ? ", conservative mode" : "");
				new_phase();
				GpuRun r;
				{
					Busy busy(o.tools);
					r = gpu_pass(job, exe, model->file, o.gpu, o.safe ? safe_settings : normal_settings, in, up, log, model_dir, pics,
						[&](size_t i) { return finish(*srcs[i], pics[i], *model, false) <= 0 ? 0 : commit(false) ? 1 : -1; },
						[&](size_t n) { progress(failed + unsigned(n)); });
				}
				if (r.aborted || !commit(true)) return 2;
				std::vector<uint8_t> d;
				read_file(log, d);
				const std::string ls(d.begin(), d.end());
				gpu_where = gpu_name_in(ls);
				if (gpu_where.empty()) gpu_where = "the graphics card";
				for (size_t i = 0; i < pics.size(); ++i)
					if (!r.done[i]) rest.push_back(srcs[i]);
				const GpuOutcome g = gpu_outcome(r.rc, ls, r.made, pics.size(), r.line);
				gpu_all = g.what.empty() && rest.empty();
				// no usable GPU: later runs keep what the CPU makes now; the GPU working again: they remake it with the GPU model
				if (g.unavailable)
				{
					const std::string s = std::to_string(unix_now()) + "\t" + g.what + "\n";
					write_file(o.tools / unavailable_file, s.data(), s.size());
				}
				else if (r.made) fs::remove(o.tools / unavailable_file, ec);
				if (g.fault)
				{
					write_fault(o.tools, g.what);
					fs::remove(o.tools / crashed_log, ec);
					fs::copy_file(log, o.tools / crashed_log, ec);
					out("@warn The AI upscaler failed on the graphics card (%s) and was stopped at once: nothing more is sent to the "
						"graphics card. The other %zu textures are made on the CPU (slower). Later runs use the CPU too (after a graphics "
						"driver update, delete tools\\upscaler\\%s or run HDUIPack.exe --use-gpu to try the graphics card again). If the "
						"screen flickered or went black, restart the PC before playing.", g.what.c_str(), rest.size(), fault_file);
				}
				else if (!g.what.empty())
					out("@warn The graphics card did not make every texture (%s): the other %zu are made on the CPU.", g.what.c_str(), rest.size());
				if (refused)
					out("@warn %u pictures the graphics card made did not match their textures (damaged output) and were not used: they "
						"are made on the CPU.", refused);
			}
		}
		else rest = todo;

		// ---- the CPU: a few textures per upscaler run (a run can be cancelled and continued)
		const Model& cm = be.gpu ? *cpu_model : *model;
		if (!rest.empty())
		{
			fs::path exe, model_dir;
			if (!ensure_upscaler(o.tools, o.upscaler, cm, false, exe, model_dir, why)) { out("@error %s", why.c_str()); return 2; }
			out("@progress check 0 0");
			std::string unused;
			bool broken = false;
			if (!probe_gpu(job, exe, cm.file, -1, work / "probe", unused, why, broken, cpu_settings.tile, nullptr, model_dir, threads))
			{
				out("@error %s", why.c_str());
				return 2;
			}
			out("upscaling %zu textures with %s on the CPU (%d threads, below-normal priority): about %.0f min on a 6-core CPU", rest.size(), cm.name,
				threads, cpu_minutes(cm, [&] { double px = 0; for (const Source* s : rest) px += double(s->tex.w) * s->tex.h; return px; }()));
			new_phase();
			progress(made + failed);
			for (size_t b0 = 0; b0 < rest.size(); b0 += cpu_settings.batch)
			{
				const size_t b1 = std::min(rest.size(), b0 + cpu_settings.batch);
				fs::remove_all(in, ec);
				fs::remove_all(up, ec);
				fs::create_directories(in, ec);
				fs::create_directories(up, ec);
				std::vector<Pic> pics;
				std::vector<const Source*> srcs;
				for (size_t i = b0; i < b1; ++i)
				{
					Pic pic;
					if (!prepare(*rest[i], pic))
					{
						drop(*rest[i]);
						failed++;
						continue;
					}
					pics.push_back(pic);
					srcs.push_back(rest[i]);
				}
				const size_t images = count_files(in);
				const unsigned base = made + failed;
				int rc = 0;
				if (images)
					rc = run_upscaler(job, exe, cm.file, -1, cpu_settings.tile, in, up, log, up, [&](size_t n)
					{
						progress(base + unsigned(pics.size() * std::min(n, images) / images));
					}, cpu_stall_seconds, 0, nullptr, model_dir, threads);
				if (rc == up_stalled)
				{
					out("@error the upscaler made no progress for %u seconds on the CPU: %s", cpu_stall_seconds, tail(log, 300).c_str());
					return 2;
				}
				if (rc != 0)
				{
					out("@error the upscaler failed (exit %d): %s", rc, tail(log, 300).c_str());
					return 2;
				}
				const unsigned before = made + refused_cpu;
				for (size_t i = 0; i < pics.size(); ++i)
					if (const int f = finish(*srcs[i], pics[i], cm, true); f == 0)
					{
						drop(*srcs[i]);
						failed++;
					}
				if (!pics.empty() && made + refused_cpu == before)
				{
					// none of its pictures came out: running it again on the next textures would only do the same
					out("@error the upscaler made none of its %zu pictures: %s", images, tail(log, 300).c_str());
					commit(true);
					return 2;
				}
				if (!commit(true)) return 2;
				progress(made + failed);
			}
		}
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		// everything made on the graphics card after --use-gpu overrode an earlier failure: later runs use it again
		const bool forgot = be.gpu && gpu_all && !failed && plan.fault && forget_fault(o.tools);
		const std::string where = !be.gpu ? std::string("the CPU") : rest.empty() ? gpu_where :
			gpu_where.empty() ? std::string("the CPU") : gpu_where + " and the CPU";
		out("@summary Made %u HD textures in %.0f min on %s (%.0f MB)%s; the pack has %u textures.%s", made, secs / 60, where.c_str(),
			double(bytes) / (1 << 20), failed ? hdui::sfmt(", %u failed (see logs)", failed).c_str() : "", made + kept,
			forgot ? " The earlier graphics card failure is cleared: the next runs use the graphics card again." : "");
		if (refused_cpu)
			out("@warn %u textures came out unlike the original (see the log): the game uses their original textures; later runs "
				"leave them alone (HDUIPack.exe --force tries them again).", refused_cpu);
		return made || kept || refused_cpu ? 0 : 2;
	}
}

#ifndef HDUI_NO_MAIN
int wmain(int argc, wchar_t** argv)
{
	using namespace hdpack;
	wchar_t self[MAX_PATH];
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	Options o;
	o.root = fs::path(self).parent_path();
	fs::path game;
	for (int i = 1; i < argc; ++i)
	{
		const std::wstring a = argv[i];
		const auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
		if (a == L"--game") game = next();
		else if (a == L"--out") o.out = next();
		else if (a == L"--tools") o.tools = next();
		else if (a == L"--model") { const auto m = next(); o.model.assign(m.begin(), m.end()); }
		else if (a == L"--upscaler") o.upscaler = next();
		else if (a == L"--use-gpu") o.use_gpu = true;
		else if (a == L"--cpu") o.use_cpu = true;
		else if (a == L"--gpu") o.gpu = _wtoi(next().c_str());
		else if (a == L"--limit") o.limit = size_t(_wtoi(next().c_str()));
		else if (a == L"--force") o.force = true;
		else if (a == L"--list") o.list = true;
		else if (a == L"--dry-run") o.dry = true;
		else if (a == L"--remove") o.remove = true;
		else if (a == L"--safe") o.safe = true;
		else if (a == L"--status") o.status = true;
		else
		{
			std::printf("HDUIPack.exe [--game <dir>] [--out <pack dir>] [--model x4plus|fast|remacri|ultrasharp] [--force] [--limit N]\n"
						"             [--use-gpu | --cpu] [--gpu N] [--safe] [--dry-run] [--list] [--remove] [--status]\n"
						"Makes mods\\hd_ui (AI-upscaled menu / HUD textures) from the game files on this PC. Normally run by the\n"
						"launcher's Create HD UI pack button. Upscales on the graphics card (Vulkan) in one upscaler run; if that\n"
						"fails it is stopped at once, the rest is made on the CPU and later runs use the CPU (tools\\upscaler\\gpu_fault.txt).\n"
						"  --model    x4plus (default on the GPU, official Real-ESRGAN; about 30x slower on the CPU), fast (default on\n"
						"             the CPU, comes with the port), remacri or ultrasharp; the larger ones are downloaded the first time\n"
						"  --use-gpu  use the graphics card even after a failure there (gpu_fault.txt)\n"
						"  --cpu      the CPU only (slower)\n"
						"  --gpu N    the upscaler's Vulkan device N (default: its own choice, the discrete card)\n"
						"  --safe     on the GPU: conservative mode, 64 px tiles, one image at a time\n"
						"  --status   prints where the next run upscales (\"@gpu gpu|cpu <note>\"); starts nothing\n");
			return a == L"--help" || a == L"-h" ? 0 : 2;
		}
	}
	if (o.out.empty()) o.out = o.root / "mods" / "hd_ui";
	if (o.tools.empty()) o.tools = o.root / "tools" / "upscaler";
	if (o.upscaler.empty()) o.upscaler = o.root / "tools" / "hdui_upscaler";
	// the upscaler runs with its own folder as working directory, so a relative --out / --tools would point elsewhere
	std::error_code ec;
	o.out = fs::absolute(o.out, ec);
	o.tools = fs::absolute(o.tools, ec);
	o.upscaler = fs::absolute(o.upscaler, ec);
	if (!game.empty()) game = fs::absolute(game, ec);
	o.usrdir = find_usrdir(o.root, game);
	return run(o);
}
#endif
