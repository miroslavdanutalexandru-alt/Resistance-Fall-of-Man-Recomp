// Offline tests for the HD UI pack: HDUIPack.exe (project/hd_ui_pack.cpp + hd_ui_game_r1.h) and the renderer side
// (ps3recomp/libs/video/r1_hd_ui.h, used by rsx_texpack.inc). Nothing here starts the game.
//   dds            DDS round trip for every format / size class: what the builder writes, r1hd_dds_check (the renderer's
//                  check) accepts, r1hd_copy_level stages every level as the file has it, and it decodes back to the
//                  picture; what the check must refuse
//   folder <dir>   commit_pack is atomic; recover_pack after a run killed between the renames; scratch outside the pack
//   upscaler <dir> the upscaler in tools\hdui_upscaler runs only as pinned (a changed / missing file is refused), the
//                  2022 build older versions downloaded is deleted, an upscaler that does not load is "broken", not "no
//                  Vulkan GPU"; the real CPU build (no Vulkan code: checked) upscales the probe image
//   gpu_safety <dir> the upscaler's command line (CPU: -g -1; GPU: its own tile size, --safe: conservative), GPU or
//                  CPU (the CPU after a fault until gpu_fault.txt is deleted or --use-gpu; a run of an earlier boot
//                  whose end Windows' log does not tell counts as a crash; its evidence kept), the watchdog on a
//                  stand-in upscaler (this program: Vulkan error / hang / too long), padding of tiny pictures, and
//                  the single GPU pass: pictures used as they come out, the rest left for the CPU after a lost
//                  device. The Vulkan upscaler is never started
//   real <usrdir>  every packable menu / HUD texture of the installed game: the key the builder computes from the
//                  archive equals the key the upload computes (r1hd_texture_key as rsx_texpack.inc's hd_lookup calls
//                  it) on a copy placed in "guest memory" with other bytes around it; then a sample goes through
//                  make_dds -> r1hd_dds_check -> r1hd_copy_level and back
#define HDUI_NO_MAIN
#include "hd_ui_pack.cpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hdpack;

// mean absolute difference; colour is only compared where the reference is visible (DXT1 drops it elsewhere)
static double mad(const Rgba& a, const Rgba& b, int ch0 = 0, int ch1 = 4)
{
	double s = 0;
	size_t n = 0;
	for (size_t i = 0; i < a.size() && i < b.size(); i += 4)
		if (ch0 == 3 || a[i + 3] >= 128)
			for (int c = ch0; c < ch1; ++c, ++n) s += std::abs(int(a[i + c]) - int(b[i + c]));
	return n ? s / n : 1e9;
}
static Rgba resize(const Rgba& src, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh)
{
	Rgba out(size_t(ow) * oh * 4);
	stbir_resize_uint8_linear(src.data(), int(w), int(h), int(w * 4), out.data(), int(ow), int(oh), int(ow * 4), STBIR_RGBA);
	return out;
}

// Stages every level of a DDS as the renderer does (r1hd_upload_layout + r1hd_copy_level) and returns level 0 as RGBA.
// false when a staged level differs from the file's bytes.
static bool stage_and_decode(const std::vector<uint8_t>& file, const r1hd_dds& d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t nm, Rgba& top)
{
	uint32_t off[16] = {}, pit[16] = {}, rows[16] = {};
	const uint64_t total = r1hd_upload_layout(fmt, w, h, d.scale, nm, off, pit, rows);
	std::vector<uint8_t> st(size_t(total), 0xEE);
	bool same = true;
	for (uint32_t l = 0; l < nm; ++l)
	{
		r1hd_copy_level(st.data() + off[l], pit[l], file.data(), &d, l, w, h, fmt);
		const uint32_t lw = r1hd_level_dim(w, d.scale, l), lh = r1hd_level_dim(h, d.scale, l);
		const uint32_t row = r1hd_level_row(fmt, lw), nr = r1hd_level_rows(fmt, lh);
		for (uint32_t y = 0; y < nr; ++y)
		{
			const uint8_t* a = st.data() + off[l] + size_t(y) * pit[l];
			const uint8_t* b = file.data() + d.off[l] + size_t(y) * row;
			if (hdui::is_dxt(fmt)) same = same && !std::memcmp(a, b, row);
			else
				for (uint32_t x = 0; x < row; x += 4)   // the file is B,G,R,A, the D3D12 texture R,G,B,A
					same = same && a[x] == b[x + 2] && a[x + 1] == b[x + 1] && a[x + 2] == b[x] && a[x + 3] == b[x + 3];
		}
	}
	const uint32_t W = w * d.scale, H = h * d.scale;
	if (hdui::is_dxt(fmt))
	{
		TexInfo t;
		t.w = uint16_t(W); t.h = uint16_t(H); t.fmt = fmt; t.gcm = fmt | hdui::GCM_LN; t.pitch = pit[0]; t.mips_reg = 1;
		top = decode(st.data(), t);
	}
	else
	{
		top.assign(size_t(W) * H * 4, 0);
		for (uint32_t y = 0; y < H; ++y) std::memcpy(&top[size_t(y) * W * 4], st.data() + size_t(y) * pit[0], size_t(W) * 4);
	}
	return same;
}

static bool check(const std::vector<uint8_t>& f, uint32_t w, uint32_t h, uint32_t fmt, uint32_t nm, r1hd_dds& d, std::string* why_out = nullptr)
{
	char why[200] = "";
	const bool ok = r1hd_dds_check(f.data(), f.size(), w, h, fmt, nm, &d, why, sizeof why) != 0;
	if (why_out) *why_out = why;
	return ok;
}

// ------------------------------------------------------------------------------------------------ dds
static int test_dds()
{
	const uint32_t fmts[] = { hdui::GCM_DXT1, hdui::GCM_DXT23, hdui::GCM_DXT45, hdui::GCM_A8R8G8B8 };
	struct Case { uint32_t w, h, s; };
	const Case cases[] = { { 8, 8, 4 }, { 64, 32, 2 }, { 100, 30, 3 }, { 48, 96, 4 }, { 512, 512, 4 }, { 1280, 720, 3 } };
	for (uint32_t fmt : fmts)
		for (const Case& c : cases)
		{
			const uint32_t W = c.w * c.s, H = c.h * c.s;
			// the levels the upload sends for a full mip chain
			const uint32_t mips = r1hd_upload_mips(fmt, c.w, c.h, 99, 0, 1);
			Rgba img(size_t(W) * H * 4);
			for (uint32_t y = 0; y < H; ++y)
				for (uint32_t x = 0; x < W; ++x)
				{
					uint8_t* p = &img[(size_t(y) * W + x) * 4];
					p[0] = uint8_t(x * 255 / W);
					p[1] = uint8_t(y * 255 / H);
					p[2] = 128;
					const bool in = (x - W / 2.0) * (x - W / 2.0) + (y - H / 2.0) * (y - H / 2.0) < (W / 3.0) * (H / 3.0);
					p[3] = fmt == hdui::GCM_DXT1 ? (in ? 255 : 0) : uint8_t(in ? 255 : x * 255 / W);
				}
			const auto file = make_dds(img, c.w, c.h, fmt, mips, c.s);
			if (hdui::is_dxt(fmt) && ((W & 3) || (H & 3)))
			{
				// D3D12 creates DXT textures of whole 4x4 blocks only: refused, and the builder never picks such a scale
				r1hd_dds d = {};
				CHECK(!check(file, c.w, c.h, fmt, mips, d));
				const uint32_t s2 = pick_scale(c.w, c.h, fmt);
				CHECK(s2 != c.s && (!s2 || (!((c.w * s2) & 3) && !((c.h * s2) & 3))));
				continue;
			}
			size_t want = 128;
			for (uint32_t l = 0; l < mips; ++l) want += r1hd_level_size(fmt, hdui::level_dim(c.w, c.s, l), hdui::level_dim(c.h, c.s, l));
			CHECK(file.size() == want);

			r1hd_dds d = {};
			std::string why;
			const bool ok = check(file, c.w, c.h, fmt, mips, d, &why);
			if (!ok) std::printf("  %s %ux%u x%u m%u: %s\n", hdui::fmt_name(fmt), c.w, c.h, c.s, mips, why.c_str());
			CHECK(ok);
			CHECK(d.scale == c.s);
			CHECK(d.levels == mips);
			// the file name the pack uses gives the key back; the size in it is the HD size
			const std::string name = pack_name(0x0123456789abcdefull, c.w, c.h, fmt, mips, c.s);
			uint64_t k = 0;
			CHECK(hdui::key_of_name(name, k) && k == 0x0123456789abcdefull);
			CHECK(name.find(hdui::sfmt("_%ux%u_", W, H)) != std::string::npos);
			if (ok)
			{
				Rgba back;
				CHECK(stage_and_decode(file, d, c.w, c.h, fmt, mips, back));
				const double e_rgb = mad(img, back, 0, 3), e_a = mad(img, back, 3, 4);
				if (e_rgb > 6 || e_a > 6) std::printf("  %s %ux%u x%u: error rgb %.2f alpha %.2f\n", hdui::fmt_name(fmt), c.w, c.h, c.s, e_rgb, e_a);
				CHECK(e_rgb <= 6);
				CHECK(e_a <= 6);
			}
			// what the check must refuse (the game then keeps its own texture)
			CHECK(!check(file, c.w + 1, c.h, fmt, mips, d));
			CHECK(!check(file, c.w, c.h, fmt, mips + 1, d));
			CHECK(!check(file, c.w, c.h, fmt == hdui::GCM_DXT1 ? hdui::GCM_DXT45 : hdui::GCM_DXT1, mips, d));
			const std::vector<uint8_t> cut(file.begin(), file.end() - 8);
			CHECK(!check(cut, c.w, c.h, fmt, mips, d));
			std::vector<uint8_t> bad = file;
			bad[0] = 'X';
			CHECK(!check(bad, c.w, c.h, fmt, mips, d));
		}
	// the size rules: 1..8x, the same on both axes, 8192 at most
	CHECK(r1hd_scale_of(256, 256, 64, 64) == 4);
	CHECK(r1hd_scale_of(256, 128, 64, 64) == 0);
	CHECK(r1hd_scale_of(100, 100, 64, 64) == 0);
	CHECK(r1hd_scale_of(8192 * 2, 8192 * 2, 2048, 2048) == 0);
	CHECK(pick_scale(1280, 720) == 3 && pick_scale(1920, 1080) == 2 && pick_scale(64, 64) == 4 && pick_scale(2048, 64) == 2 && pick_scale(2049, 64) == 0);
	return 0;
}

// ------------------------------------------------------------------------------------------------ folder
static void put(const fs::path& p, const std::string& s)
{
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary) << s;
}
static std::string get(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}
static NewFile nf(uint64_t key, const std::string& name, const std::string& data)
{
	return NewFile{ key, name, std::vector<uint8_t>(data.begin(), data.end()), Made{ name, "remacri", "test" } };
}
static int test_folder(const fs::path& dir)
{
	std::error_code ec;
	fs::remove_all(dir, ec);
	// (what the renderer loads from a pack folder -- never '.' scratch folders -- is ps3recomp's test_hd_ui)
	// 1. commit_pack: the pack is replaced in one rename; scratch lives beside it
	const fs::path out = dir / "mods" / "hd_ui", stage = stage_dir(out);
	std::map<uint64_t, Made> man;
	std::string why;
	std::vector<NewFile> b1 = { nf(0xa, "000000000000000a_32x32_DXT1_m1.dds", "A1"), nf(0xb, "000000000000000b_32x32_DXT5_m1.dds", "B1") };
	CHECK(commit_pack(out, stage, b1, man, why));
	CHECK(get(out / "000000000000000a_32x32_DXT1_m1.dds") == "A1");
	CHECK(fs::exists(out / "pack.tsv") && !fs::exists(stage) && !fs::exists(fs::path(stage).concat(".old")));
	std::vector<NewFile> b2 = { nf(0xa, "000000000000000a_64x64_DXT1_m1.dds", "A2"), nf(0xc, "000000000000000c_16x16_DXT1_m1.dds", "C2") };
	CHECK(commit_pack(out, stage, b2, man, why));
	CHECK(!fs::exists(out / "000000000000000a_32x32_DXT1_m1.dds"));   // a key made again replaces its old file
	CHECK(get(out / "000000000000000a_64x64_DXT1_m1.dds") == "A2");
	CHECK(get(out / "000000000000000b_32x32_DXT5_m1.dds") == "B1");    // kept from the earlier commit
	CHECK(get(out / "000000000000000c_16x16_DXT1_m1.dds") == "C2");
	const auto m2 = read_manifest(out / "pack.tsv");
	CHECK(m2.size() == 3 && m2.at(0xa).file == "000000000000000a_64x64_DXT1_m1.dds" && m2.at(0xb).model == "remacri");
	CHECK(m2.at(0xa).version == pack_version && !stale_row(m2.at(0xb)));
	// rows from before version 2 (4 columns) read as v1; their unchecked non-fast/x4plus pictures are made again
	put(dir / "old.tsv", "000000000000000a\ta.dds\tremacri\t16x16\n000000000000000b\tb.dds\tx4plus\t16x16\n000000000000000c\tc.dds\tfast\t16x16\n000000000000000d\td.dds\tremacri\t16x16\tv2\n");
	const auto mo = read_manifest(dir / "old.tsv");
	CHECK(mo.size() == 4 && mo.at(0xa).version == 1 && mo.at(0xd).version == 2);
	CHECK(stale_row(mo.at(0xa)) && !stale_row(mo.at(0xb)) && !stale_row(mo.at(0xc)) && !stale_row(mo.at(0xd)));
	// like_source: a 4x picture of the texture passes; another texture's picture or garbage is refused
	{
		const uint32_t w = 16, h = 16;
		Rgba src(w * h * 4), other(w * h * 4);
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x)
			{
				uint8_t* p = &src[(y * w + x) * 4];
				const bool frame = x < 3 || y < 3;   // a corner piece: visible frame, transparent elsewhere
				p[0] = uint8_t(40 + x * 8), p[1] = uint8_t(90 + y * 6), p[2] = 200, p[3] = frame ? 255 : 0;
				uint8_t* q = &other[(y * w + x) * 4];
				q[0] = q[1] = q[2] = 60, q[3] = (x > 4 && y > 4) ? 255 : 0;
			}
		Rgba hd = resize(src, w, h, w * 4, h * 4), bad = resize(other, w, h, w * 4, h * 4), noise(hd.size());
		uint32_t r = 12345;
		for (auto& b : noise) r = r * 1103515245u + 12345u, b = uint8_t(r >> 16);
		CHECK(like_source(hd, w * 4, h * 4, src, w, h));
		CHECK(!like_source(bad, w * 4, h * 4, src, w, h));
		CHECK(!like_source(noise, w * 4, h * 4, src, w, h));
		CHECK(!like_source(hd, w * 4, h * 4, src, 0, h));
	}
	// a texture refused on the CPU too: its old (stale) file goes, a refused row stays and is left alone until --force;
	// a failed stale one loses its file and row (the game uses the original texture)
	{
		const fs::path o2 = dir / "refused" / "hd_ui", s2 = stage_dir(o2);
		std::map<uint64_t, Made> mr;
		std::vector<NewFile> r1 = { nf(0x1, "0000000000000001_16x16_DXT5_m1.dds", "R1"), nf(0x2, "0000000000000002_16x16_DXT5_m1.dds", "R2"),
			nf(0x3, "0000000000000003_16x16_DXT5_m1.dds", "R3") };
		CHECK(commit_pack(o2, s2, r1, mr, why));
		std::vector<NewFile> r2 = { NewFile{ 0x1, {}, {}, Made{ {}, refused_model, "16x16 DXT5 m1" } }, NewFile{ 0x2, {}, {}, Made{} } };
		CHECK(commit_pack(o2, s2, r2, mr, why));
		CHECK(!fs::exists(o2 / "0000000000000001_16x16_DXT5_m1.dds") && !fs::exists(o2 / "0000000000000002_16x16_DXT5_m1.dds"));
		CHECK(get(o2 / "0000000000000003_16x16_DXT5_m1.dds") == "R3");
		const auto mr2 = read_manifest(o2 / "pack.tsv");
		CHECK(mr2.size() == 2 && refused_row(mr2.at(0x1)) && mr2.at(0x1).version == pack_version && !mr2.count(0x2));
		CHECK(row_plan(&mr2.at(0x1), o2, false, nullptr, false) == Row::leave && row_plan(&mr2.at(0x1), o2, false, "x4plus", true) == Row::leave);
		CHECK(row_plan(&mr2.at(0x1), o2, true, nullptr, false) == Row::make);   // --force tries it again
		CHECK(row_plan(&mr2.at(0x3), o2, false, nullptr, false) == Row::keep && row_plan(nullptr, o2, false, nullptr, false) == Row::make);
		CHECK(row_plan(&mr2.at(0x3), o2, false, "x4plus", false) == Row::make && row_plan(&mr2.at(0x3), o2, true, nullptr, false) == Row::make);
		Made stale{ "0000000000000003_16x16_DXT5_m1.dds", "remacri", "16x16 DXT5 m1", 1 }, fast{ stale.file, "fast", stale.source };
		CHECK(row_plan(&stale, o2, false, nullptr, false) == Row::make);
		CHECK(row_plan(&fast, o2, false, nullptr, false) == Row::keep && row_plan(&fast, o2, false, nullptr, true) == Row::make);
		fast.file = "missing.dds";
		CHECK(row_plan(&fast, o2, false, nullptr, false) == Row::make);
	}
	// a file the pack folder gains from outside survives a commit
	put(out / "000000000000000d_32x32_DXT1_m1.dds", "D");
	std::vector<NewFile> b3 = { nf(0xe, "000000000000000e_32x32_DXT1_m1.dds", "E") };
	CHECK(commit_pack(out, stage, b3, man, why));
	CHECK(get(out / "000000000000000d_32x32_DXT1_m1.dds") == "D");

	// 2. recover_pack after a run killed between the two renames
	const fs::path old = fs::path(stage).concat(".old");
	fs::create_directories(stage);
	for (auto& e : fs::directory_iterator(out)) fs::copy_file(e.path(), stage / e.path().filename());
	put(stage / "000000000000000f_32x32_DXT1_m1.dds", "F");
	fs::rename(out, old);                       // killed here: no pack folder, stage complete
	CHECK(recover_pack(out, stage, why));
	CHECK(fs::exists(out / "pack.tsv") && get(out / "000000000000000f_32x32_DXT1_m1.dds") == "F");
	CHECK(!fs::exists(stage) && !fs::exists(old));
	fs::rename(out, old);                       // killed while the stage was being written: the old pack comes back
	put(stage / "000000000000000a_64x64_DXT1_m1.dds", "half");
	CHECK(recover_pack(out, stage, why));
	CHECK(fs::exists(out / "pack.tsv") && get(out / "000000000000000a_64x64_DXT1_m1.dds") == "A2");
	CHECK(!fs::exists(stage) && !fs::exists(old));
	put(stage / "x.dds", "stale");              // a stale stage next to a good pack is dropped
	CHECK(recover_pack(out, stage, why));
	CHECK(!fs::exists(stage) && fs::exists(out / "pack.tsv"));
	// a file held open during recovery (antivirus, indexer, Explorer): neither rename works, so both packs are kept
	fs::rename(out, old);
	fs::create_directories(stage);
	for (auto& e : fs::directory_iterator(old)) fs::copy_file(e.path(), stage / e.path().filename());
	{
		auto hold = [](const fs::path& p) { return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); };
		HANDLE hs = hold(stage / "000000000000000f_32x32_DXT1_m1.dds"), ho = hold(old / "pack.tsv");
		CHECK(hs != INVALID_HANDLE_VALUE && ho != INVALID_HANDLE_VALUE);
		std::string rwhy;
		CHECK(!recover_pack(out, stage, rwhy) && rwhy.find("in use") != std::string::npos);
		CHECK(!fs::exists(out) && get(stage / "000000000000000f_32x32_DXT1_m1.dds") == "F" && fs::exists(stage / "pack.tsv"));
		CHECK(fs::exists(old / "pack.tsv") && get(old / "000000000000000f_32x32_DXT1_m1.dds") == "F");
		Options lo;                             // a build stops with @error instead of starting over
		lo.root = dir;
		lo.out = out;
		lo.tools = dir / "tools" / "upscaler";
		CHECK(run(lo) == 2);
		CHECK(fs::exists(stage / "pack.tsv") && fs::exists(old / "pack.tsv"));
		CloseHandle(ho);                        // only the new one in use: the old pack goes back
		CHECK(recover_pack(out, stage, rwhy));
		CHECK(get(out / "000000000000000f_32x32_DXT1_m1.dds") == "F" && !fs::exists(old));
		CloseHandle(hs);
		CHECK(recover_pack(out, stage, rwhy) && !fs::exists(stage) && fs::exists(out / "pack.tsv"));
	}

	// 3. the builder's scratch folder is not inside the pack folder it writes
	Options o;
	o.root = dir;
	o.out = o.root / "mods" / "hd_ui";
	o.tools = o.root / "tools" / "upscaler";
	const fs::path work = o.tools / "work";
	CHECK(std::mismatch(o.out.begin(), o.out.end(), work.begin(), work.end()).first != o.out.end());

	// 4. --remove deletes the pack, also when tools\upscaler was never made (the rename target's parent)
	fs::remove_all(o.tools, ec);
	o.remove = true;
	CHECK(fs::exists(o.out / "pack.tsv"));
	CHECK(run(o) == 0);
	CHECK(!fs::exists(o.out) && !fs::exists(removed_dir(o.out)));
	CHECK(run(o) == 0);                         // nothing to remove is fine
	put(removed_dir(o.out) / "x.dds", "left by a --remove that was ended");
	CHECK(run(o) == 0 && !fs::exists(removed_dir(o.out)));
	fs::remove_all(dir, ec);
	return 0;
}

// ------------------------------------------------------------------------------------------------ xvolume
// The pack folder and tools\upscaler on different drives (review fix): a folder rename cannot cross drives, so the
// stage / old / removed folders are beside the pack folder, never under --tools. `other` is a folder on another drive.
static int test_xvolume(const fs::path& dir, const fs::path& other)
{
	std::error_code ec;
	// where the folders are: beside the pack, dot-named (nothing loads them), whatever --tools is
	const fs::path m = fs::path("D:\\g") / "mods";
	CHECK(stage_dir(m / "hd_ui") == m / ".hd_ui.stage");
	CHECK(stage_dir(fs::path("D:\\g\\mods\\hd_ui\\")) == m / ".hd_ui.stage");
	CHECK(removed_dir(m / "hd_ui") == m / ".hd_ui.removed");
	CHECK(stage_dir(fs::path("E:\\packs\\mine")) == fs::path("E:\\packs\\.mine.stage"));
	if (other.empty() || other == "none" || fs::absolute(other).root_name() == fs::absolute(dir).root_name())
	{
		std::printf("SKIP: no folder on another drive (%s)\n", other.string().c_str());
		return g_fail ? 1 : 77;
	}
	fs::remove_all(dir, ec);
	fs::remove_all(other, ec);
	fs::create_directories(dir, ec);
	put(other / "probe" / "x", "x");
	if (MoveFileExW((other / "probe").c_str(), (dir / "probe").c_str(), 0))
	{
		std::printf("SKIP: %s and %s are one volume (a folder rename works)\n", other.string().c_str(), dir.string().c_str());
		fs::remove_all(dir, ec);
		fs::remove_all(other, ec);
		return g_fail ? 1 : 77;
	}
	Options o;
	o.root = dir;
	o.out = dir / "mods" / "hd_ui";
	o.tools = other / "tools" / "upscaler";
	// commits as run() does them: the stage beside the pack
	std::map<uint64_t, Made> man;
	std::string why;
	std::vector<NewFile> b1 = { nf(0xa, "000000000000000a_32x32_DXT1_m1.dds", "A1") };
	CHECK(commit_pack(o.out, stage_dir(o.out), b1, man, why));
	std::vector<NewFile> b2 = { nf(0xb, "000000000000000b_32x32_DXT5_m1.dds", "B2") };
	CHECK(commit_pack(o.out, stage_dir(o.out), b2, man, why));
	if (!why.empty()) std::printf("  commit: %s\n", why.c_str());
	CHECK(get(o.out / "000000000000000a_32x32_DXT1_m1.dds") == "A1" && get(o.out / "000000000000000b_32x32_DXT5_m1.dds") == "B2");
	CHECK(!fs::exists(stage_dir(o.out)) && !fs::exists(fs::path(stage_dir(o.out)).concat(".old")));
	// a run killed between the renames is recovered by the next run (here --remove, which recovers first)
	fs::create_directories(stage_dir(o.out));
	for (auto& e : fs::directory_iterator(o.out)) fs::copy_file(e.path(), stage_dir(o.out) / e.path().filename());
	fs::rename(o.out, fs::path(stage_dir(o.out)).concat(".old"));
	put(o.tools / "hd_ui.stage" / "x.dds", "old version's scratch");
	o.remove = true;
	CHECK(run(o) == 0);
	CHECK(!fs::exists(o.out) && !fs::exists(removed_dir(o.out)) && !fs::exists(stage_dir(o.out)));
	CHECK(!fs::exists(fs::path(stage_dir(o.out)).concat(".old")) && !fs::exists(o.tools / "hd_ui.stage"));
	fs::remove_all(dir, ec);
	fs::remove_all(other, ec);
	std::printf("xvolume: pack on %s, tools on %s: commit, recover and remove ok\n", dir.root_name().string().c_str(), other.root_name().string().c_str());
	return 0;
}

// ------------------------------------------------------------------------------------------------ real
static int test_real(const fs::path& usrdir)
{
	std::error_code ec;
	if (usrdir == "none" || !fs::is_regular_file(usrdir / hdgame::archive_name, ec))
	{
		std::printf("SKIP: no game files (%s)\n", usrdir.string().c_str());
		return 77;
	}
	const Scan sc = hdgame::scan_archives(usrdir, nullptr);
	CHECK(!sc.textures.empty());
	std::set<std::pair<std::string, std::string>> assets;
	for (const auto& [k, s] : sc.textures) assets.insert({ s.header, s.texel });
	hdgame::Psarc pa;
	CHECK(pa.open(usrdir / hdgame::archive_name));
	std::set<uint64_t> seen;
	std::map<std::string, unsigned> by_fmt;
	unsigned checked = 0, mismatched = 0, sampled = 0;
	std::map<std::string, unsigned> sample_left = { { "DXT1", 3 }, { "DXT3", 3 }, { "DXT5", 3 }, { "A8R8G8B8", 3 } };
	bool tamper_done = false;
	for (const auto& [hdr, tex] : assets)
	{
		std::vector<uint8_t> hb, tb;
		if (!pa.read(hdr, hb) || !pa.read(tex, tb))
		{
			std::printf("  cannot read %s / %s\n", hdr.c_str(), tex.c_str());
			++g_fail;
			continue;
		}
		for (const TexInfo& t : hdgame::parse_textures(hb, tb))
		{
			if (!t.skip.empty()) continue;
			// the texture's bytes as the game has them in RSX memory: somewhere else, with other data around
			const size_t start = t.data_off, have = tb.size() - start;
			std::vector<uint8_t> guest(have + 8192, 0xCD);
			std::memcpy(guest.data() + 4096, tb.data() + start, have);
			const uint8_t* g = guest.data() + 4096;
			// what rsx_texpack.inc's hd_lookup computes at upload (avail unbounded, RSX_TEX_MIPS default)
			uint64_t key = 0, need = 0;
			uint32_t nm = 0;
			const bool keyed = r1hd_texture_key(g, ~0ull, t.gcm, t.w, t.h, t.mips_reg, t.pitch, 0, 1, &key, &nm, &need) != 0;
			CHECK(need <= have);   // the upload reads only the texture's own bytes
			++checked;
			if (!keyed || key != t.key || nm != t.mips)
			{
				if (mismatched++ < 10)
					std::printf("  MISMATCH %s %ux%u %s: builder %016llx m%u, renderer %016llx m%u%s\n", hdr.c_str(), t.w, t.h,
						hdui::fmt_name(t.fmt), static_cast<unsigned long long>(t.key), t.mips, static_cast<unsigned long long>(key), nm,
						keyed ? "" : " (not keyed)");
				continue;
			}
			seen.insert(key);
			by_fmt[hdui::fmt_name(t.fmt)]++;
			if (!tamper_done)
			{
				// one changed texel byte gives another key (the bytes are hashed, not only the size)
				tamper_done = true;
				guest[4096] ^= 0x5A;
				uint64_t k2 = 0;
				CHECK(r1hd_texture_key(g, ~0ull, t.gcm, t.w, t.h, t.mips_reg, t.pitch, 0, 1, &k2, nullptr, nullptr) && k2 != key);
				guest[4096] ^= 0x5A;
			}
			auto& left = sample_left[hdui::fmt_name(t.fmt)];
			const uint32_t s = pick_scale(t.w, t.h, t.fmt);
			if (!left || !s) continue;
			--left;
			++sampled;
			const Rgba img = decode(g, t);
			const Rgba hd = resize(img, t.w, t.h, t.w * s, t.h * s);   // stands in for the AI upscaler's output
			const auto file = make_dds(hd, t.w, t.h, t.fmt, t.mips, s);
			r1hd_dds d = {};
			std::string why;
			const bool ok = check(file, t.w, t.h, t.gcm, nm, d, &why);
			if (!ok) std::printf("  r1hd_dds_check refused %016llx: %s\n", static_cast<unsigned long long>(key), why.c_str());
			CHECK(ok && d.scale == s);
			if (ok)
			{
				Rgba top;
				CHECK(stage_and_decode(file, d, t.w, t.h, t.fmt, nm, top));
				const Rgba down = resize(top, t.w * s, t.h * s, t.w, t.h);
				const double e = mad(img, down, 0, 3);
				std::printf("  sample %016llx %4ux%-4u %-8s m%u x%u -> %ux%u, %zu KB, back-to-original error %.2f\n", static_cast<unsigned long long>(key),
					t.w, t.h, hdui::fmt_name(t.fmt), t.mips, s, t.w * s, t.h * s, file.size() >> 10, e);
				CHECK(e < 16);
			}
		}
	}
	size_t packed = 0;
	for (const auto& [k, s] : sc.textures)
		packed += pick_scale(s.tex.w, s.tex.h, s.tex.fmt) != 0;
	for (const auto& [k, s] : sc.textures)
		if (!seen.count(k)) { std::printf("  key %016llx of the pack list was not matched\n", static_cast<unsigned long long>(k)); ++g_fail; break; }
	std::printf("real: %u textures in %zu assets keyed as the upload keys them, %u mismatches; %zu distinct keys of %zu the builder lists (%zu it upscales);",
		checked, assets.size(), mismatched, seen.size(), sc.textures.size(), packed);
	for (const auto& [f, n] : by_fmt) std::printf(" %s %u", f.c_str(), n);
	std::printf("; %u sampled through make_dds/r1hd_dds_check/upload staging\n", sampled);
	CHECK(mismatched == 0);
	CHECK(sampled >= 3);
	return 0;
}

// ------------------------------------------------------------------------------------------------ upscaler files
static bool has_text(const std::vector<uint8_t>& d, const char* t)
{
	return std::search(d.begin(), d.end(), t, t + std::strlen(t)) != d.end();
}

static int test_upscaler(const fs::path& root)
{
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root, ec);
	const fs::path repo_up = fs::path(HDUI_REPO) / "tools" / "hdui_upscaler";

	// a file of tools\hdui_upscaler is run only as the port ships it: missing or changed -> refused
	std::string why;
	put(root / "f.bin", "abc");
	CHECK(check_pinned(root / "f.bin", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", why));
	CHECK(!check_pinned(root / "f.bin", upscaler_cpu.sha, why) && why.find("not the file") != std::string::npos);
	CHECK(!check_pinned(root / "none.exe", upscaler_cpu.sha, why) && why.find("missing") != std::string::npos);
	const fs::path fake_up = root / "fake_up", tools = root / "tools";
	put(fake_up / "bin" / upscaler_cpu.name, "MZ not the pinned build");
	put(fake_up / "bin" / upscaler_omp.name, "MZ");
	fs::path exe, md;
	CHECK(!ensure_upscaler(tools, fake_up, *find_model("fast"), false, exe, md, why) && exe.empty());
	std::printf("  changed upscaler -> %s\n", why.c_str());

	// the 2022 build older versions downloaded is deleted; a downloaded model is moved out of it only if it checks out
	put(tools / legacy_dir / "realesrgan-ncnn-vulkan.exe", "MZ");
	put(tools / legacy_dir / "models" / "remacri-4x.param", "damaged");
	put(tools / legacy_zip, "zip");
	if (fs::is_regular_file(repo_up / "bin" / upscaler_cpu.name, ec))
	{
		CHECK(ensure_upscaler(tools, repo_up, *find_model("fast"), false, exe, md, why));
		CHECK(exe == repo_up / "bin" / upscaler_cpu.name && md == repo_up / "models");
		CHECK(!fs::exists(tools / legacy_dir, ec) && !fs::exists(tools / legacy_zip, ec) && !fs::exists(tools / "models" / "remacri-4x.param", ec));
		CHECK(ensure_upscaler(tools, repo_up, *find_model("fast"), true, exe, md, why) && exe == repo_up / "bin" / upscaler_gpu.name);
		// the CPU build has no Vulkan code at all (so it cannot reach the GPU); the GPU build loads Vulkan itself
		std::vector<uint8_t> cpu, gpu_exe;
		CHECK(read_file(repo_up / "bin" / upscaler_cpu.name, cpu) && read_file(repo_up / "bin" / upscaler_gpu.name, gpu_exe));
		CHECK(!has_text(cpu, "vulkan-1") && !has_text(cpu, "vkCreateInstance") && !has_text(cpu, "vkQueueSubmit"));
		CHECK(has_text(gpu_exe, "vulkan-1") && has_text(gpu_exe, "GPU_STOP:"));
	}
	else std::printf("  (tools\\hdui_upscaler\\bin is missing: its part is skipped)\n");

	// an upscaler that does not load is a broken install, not "no Vulkan GPU"
	CHECK(is_loader_error(-1) && is_loader_error(int(0xC0000135u)) && is_loader_error(int(0xC000007Bu)) && is_loader_error(int(0xC0000142u)));
	CHECK(!is_loader_error(0) && !is_loader_error(1) && !is_loader_error(-2));
	put(root / "broken" / "realesrgan-ncnn-vulkan.exe", "MZ");
	Job job;
	std::string gpu;
	bool broken = false, fault = true;
	CHECK(!probe_gpu(job, root / "broken" / "realesrgan-ncnn-vulkan.exe", "remacri", -1, root / "probe", gpu, why, broken));
	CHECK(broken);
	CHECK(why.find("Vulkan") == std::string::npos);
	std::printf("  not loadable -> %s\n", why.c_str());
	broken = false;
	CHECK(!probe_gpu(job, root / "broken" / "realesrgan-ncnn-vulkan.exe", "remacri", -1, root / "probe", gpu, why, broken, 200, &fault, root, 2));
	CHECK(broken && !fault);

	// the real CPU upscaler (never the Vulkan one) on the probe's test image, with the bundled model: only when it is the
	// pinned build and has no Vulkan code
	std::vector<uint8_t> cpu;
	if (read_file(repo_up / "bin" / upscaler_cpu.name, cpu) && sha256_file(repo_up / "bin" / upscaler_cpu.name) == upscaler_cpu.sha &&
		!has_text(cpu, "vulkan-1") && !has_text(cpu, "vkCreateInstance"))
	{
		const Model& m = *find_model("fast");
		fault = true;
		why.clear();
		CHECK(probe_gpu(job, repo_up / "bin" / upscaler_cpu.name, m.file, -1, root / "cpu_probe", gpu, why, broken, cpu_settings.tile, &fault,
			repo_up / "models", 2));
		CHECK(!fault && gpu.empty());
		if (!why.empty()) std::printf("  CPU probe: %s\n", why.c_str());
	}
	fs::remove_all(root, ec);
	return 0;
}

// ------------------------------------------------------------------------------------------------ GPU safety
// What keeps the upscaler from freezing the PC, without running it: its command line, the watchdog's reading of its
// output, and what a crashed / failed earlier run makes the next one do.
static int test_gpu_safety(const fs::path& root)
{
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "work" / "probe", ec);

	// the GPU: the upscaler's own tile size and jobs (as Lantern runs it); --safe: conservative mode, 64 px tiles, one image
	// at a time; -g only when asked (ncnn picks the discrete card itself)
	const std::wstring cl = upscaler_cmdline("C:\\u\\realesrgan-ncnn-vulkan.exe", "realesrgan-x4plus", -1, normal_settings.tile, "C:\\in", "C:\\out");
	CHECK(cl.find(L" -t ") == std::wstring::npos && cl.find(L" -j ") == std::wstring::npos && cl.find(L" -g ") == std::wstring::npos);
	CHECK(cl.find(L" -c") == std::wstring::npos && cl.find(L" -n realesrgan-x4plus -s 4 ") != std::wstring::npos);
	const std::wstring sl = upscaler_cmdline("x.exe", "m", 1, safe_settings.tile, "i", "o", {}, 0, safe_settings.conservative);
	CHECK(sl.find(L" -t 64 -g 1 -j 1:1:1 -c") != std::wstring::npos);
	CHECK(normal_settings.tile == 0 && !normal_settings.conservative && safe_settings.tile == 64 && safe_settings.conservative);
	CHECK(find_model(gpu_default_model) && find_model("realesrgan-x4plus") == find_model("x4plus") && find_model("animevideov3") == find_model("fast"));
	CHECK(std::string(find_model(gpu_default_model)->file) == "realesrgan-x4plus" && find_model(cpu_default_model)->from == From::bundled);
	// the CPU: -g -1, the threads as proc jobs, no GPU options; the model folder as given
	const std::wstring ccl = upscaler_cmdline("C:\\u\\realesrgan-ncnn-cpu.exe", "realesr-animevideov3-x4", 0, cpu_settings.tile, "i", "o", "C:\\m\\models", 6);
	CHECK(ccl.find(L" -m \"C:\\m\\models\"") != std::wstring::npos && ccl.find(L" -t 200 -j 1:6:1 -g -1") != std::wstring::npos);
	CHECK(ccl.find(L" -c") == std::wstring::npos && ccl.find(L"-g 0") == std::wstring::npos);
	CHECK(cpu_threads(12) == 6 && cpu_threads(1) == 1 && cpu_threads(0) == 1 && cpu_threads(64) == 16);

	// the watchdog's reading of the upscaler's output (what the RTX 5070 run printed before the PC froze)
	const std::string froze = "[0 NVIDIA GeForce RTX 5070]  queueC=2[8]  queueG=0[16]  queueT=1[2]\n0.00%\n0.00%\nvkQueueSubmit failed -4\n0.00%\n";
	std::string line;
	CHECK(upscaler_gpu_error(froze, &line) && line == "vkQueueSubmit failed -4");
	CHECK(upscaler_gpu_error("  vkWaitForFences failed -4\r\n") && upscaler_gpu_error("VK_ERROR_DEVICE_LOST\n"));
	CHECK(!upscaler_gpu_error("[0 NVIDIA GeForce RTX 5070]  bugsbn1=0  bugbilz=0\n0.00%\n50.00%\n"));
	CHECK(!upscaler_gpu_error("decode image C:\\in\\a.png failed\n"));   // a picture it cannot read is no GPU failure
	CHECK(!is_loader_error(up_gpu_error) && !is_loader_error(up_stalled) && !is_loader_error(up_timeout));
	// no Vulkan GPU / driver at all, or out of memory: no driver fault (it would hold every later run to the safe settings
	// and then refuse); only a lost device is one
	const std::string no_gpu = "vkCreateInstance failed -9\ninvalid gpu device\n";
	CHECK(!upscaler_gpu_error(no_gpu) && upscaler_no_gpu(no_gpu, &line) && line == "vkCreateInstance failed -9");
	CHECK(!upscaler_gpu_error("vkEnumeratePhysicalDevices failed -3\n") && upscaler_no_gpu("vkEnumeratePhysicalDevices failed -3\n"));
	CHECK(!upscaler_gpu_error("vkAllocateMemory failed -2\n") && !upscaler_gpu_error("vkCreateDevice failed -3\n"));
	CHECK(!upscaler_gpu_error("vkQueueSubmit failed -42\n") && upscaler_gpu_error("vkCreateDevice failed -4\n"));
	CHECK(!upscaler_no_gpu(froze));

	// the upscaler's own stop at a failed GPU submission is a driver fault too
	CHECK(upscaler_gpu_error("GPU_STOP: vkQueueSubmit failed -4 (device lost?) - stopping, nothing more is sent to the GPU\n", &line) &&
		line.compare(0, 9, "GPU_STOP:") == 0);

	// the GPU unless --cpu or an earlier run failed there (gpu_fault.txt, or a crash found now); --use-gpu tries it again
	const uint64_t boot = 1'800'000'000;
	static uint64_t asked = 0;   // the time the shutdown check was asked about
	const ShutdownCheck crashed = [](uint64_t t) { asked = t; return 1; };
	const ShutdownCheck clean = [](uint64_t t) { asked = t; return 0; };
	const ShutdownCheck unknown = [](uint64_t t) { asked = t; return -1; };
	Plan p = plan_gpu(root, boot, crashed);
	CHECK(!p.fault && p.note.empty() && p.crash.empty() && !p.stale_marker);
	Backend be = choose_backend(false, false, p, root);
	CHECK(be.gpu && be.note.empty());
	CHECK(!choose_backend(false, true, p, root).gpu && !choose_backend(true, true, p, root).gpu);   // --cpu

	// a marker of this boot is a Cancel / killed run, not a crash
	asked = 0;
	put(root / running_marker, std::to_string(boot + 100) + "\tgpu\n");
	p = plan_gpu(root, boot, crashed);
	CHECK(!p.fault && !p.stale_marker && asked == 0);
	// a marker older than the boot after a normal restart / shutdown / sign-out: no crash, only cleared
	put(root / running_marker, std::to_string(boot - 3600) + "\tgpu\n");
	p = plan_gpu(root, boot, clean);
	CHECK(asked == boot - 3600 && !p.fault && p.crash.empty() && p.stale_marker);
	// ... the System log does not tell (cleared, wrapped, not readable): counted as a crash
	p = plan_gpu(root, boot, unknown);
	CHECK(p.fault && !p.crash.empty() && p.stale_marker && p.crash.find("does not tell") != std::string::npos);
	put(root / running_marker, "garbage");   // unreadable marker: not known either
	CHECK(plan_gpu(root, boot, clean).fault);
	// ... Windows logged an unexpected shutdown after it: the PC did not survive the upscaler
	put(root / running_marker, std::to_string(boot - 3600) + "\tgpu\n");
	p = plan_gpu(root, boot, crashed);
	CHECK(p.fault && p.crash.find("unexpectedly") != std::string::npos && p.note.find(p.crash) != std::string::npos);
	be = choose_backend(false, false, p, root);
	CHECK(!be.gpu && be.note.find(fault_file) != std::string::npos && be.note.find("--use-gpu") != std::string::npos);
	be = choose_backend(true, false, p, root);
	CHECK(be.gpu && be.note.find("tried again") != std::string::npos);
	// what run() does with it: gpu_fault.txt, and the marker and the upscaler's log kept as upscaler_crashed.*
	const std::string froze_log = "[0 NVIDIA GeForce RTX 5070]\n0.00%\nvkQueueSubmit failed -4\n";
	put(root / "work" / "upscaler.log", froze_log);
	p = plan_gpu(root, boot, crashed);
	CHECK(p.crash.find("vkQueueSubmit failed -4") != std::string::npos && p.stale_marker);
	write_fault(root, p.crash);
	keep_crash_evidence(root);
	CHECK(!fs::exists(root / running_marker, ec) && !fs::exists(root / "work" / "upscaler.log", ec));
	CHECK(get(root / crashed_marker).find("\tgpu") != std::string::npos && get(root / crashed_log) == froze_log);
	p = plan_gpu(root, boot, clean);   // the next runs: from gpu_fault.txt
	CHECK(p.fault && p.crash.empty() && p.note.find("vkQueueSubmit failed -4") != std::string::npos && !choose_backend(false, false, p, root).gpu);
	// ... until it is deleted, or a --use-gpu run makes everything on the GPU
	CHECK(forget_fault(root) && !fs::exists(root / fault_file, ec) && !forget_fault(root));
	CHECK(!plan_gpu(root, boot, clean).fault);
	put(root / fault_file, "garbage");   // whatever is in it
	CHECK(plan_gpu(root, boot, clean).fault);
	fs::remove(root / fault_file, ec);
	// a log of a PC without a Vulkan GPU is no crash
	put(root / "work" / "upscaler.log", no_gpu);
	CHECK(!plan_gpu(root, boot, crashed).fault);
	fs::remove_all(root / "work", ec);
	// the real System log query answers (0 or 1; -1 only if Windows' event log service is off). Query from just before
	// this boot so the start event of the current boot is always inside the range, however long the PC has been on.
	const int real = unexpected_shutdown_since(boot_unix() - 60);
	std::printf("  unexpected shutdown at this boot (System log): %d\n", real);
	CHECK(real == 0 || real == 1);
	// only the first start of Windows after the marker counts: a clean restart during a run (the marker stays), then
	// days later an unrelated freeze, is no crash of the upscaler
	const uint64_t m = 1'800'000'000;
	const std::vector<BootEvent> froze_then = { { m - 50, true }, { m + 600, true }, { m + 640, false } };   // 41 at the next start
	CHECK(unexpected_shutdown_in(froze_then, m) == 1);
	const std::vector<BootEvent> clean_then = { { m + 600, true }, { m + 3 * 86400, true }, { m + 3 * 86400 + 30, false } };
	CHECK(unexpected_shutdown_in(clean_then, m) == 0);
	CHECK(unexpected_shutdown_in({ { m + 600, true }, { m + 600 + boot_log_window + 1, false } }, m) == 0);
	CHECK(unexpected_shutdown_in({ { m + 600, false }, { m + 610, true } }, m) == 1);   // 6008 logged just before 6005
	CHECK(unexpected_shutdown_in({ { m - 10, false }, { m + 600, true } }, m) == 0);   // before the run
	CHECK(unexpected_shutdown_in({ { m + 640, false } }, m) == -1);   // its start is not in the log: not known
	CHECK(unexpected_shutdown_in({}, m) == -1);

	// the watchdog ends an upscaler that reports a Vulkan error but keeps going (as ncnn does), one that hangs, and one
	// that runs too long; this test program stands in for it (see main: "-i <in> ..."), nothing touches the GPU
	wchar_t self[MAX_PATH];
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	Job job;
	const auto fake = [&](const char* mode, unsigned stall, unsigned max, std::string* gl)
	{
		const auto t = std::chrono::steady_clock::now();
		const int rc = run_upscaler(job, self, "remacri-4x", -1, 32, root / mode, root / "out", root / (std::string(mode) + ".log"), root, [](size_t) {}, stall, max, gl);
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
		std::printf("  fake upscaler %-9s -> %d after %.1f s\n", mode, rc, s);
		CHECK(s < 20);
		return rc;
	};
	std::string gl;
	CHECK(fake("gpu_error", 0, 0, &gl) == up_gpu_error && gl == "vkQueueSubmit failed -4");
	CHECK(fake("hang", 2, 0, nullptr) == up_stalled);
	CHECK(fake("busy", 30, 2, nullptr) == up_timeout);   // keeps printing progress, never finishes
	CHECK(fake("ok", 30, 0, nullptr) == 0);
	CHECK(fake("lost_exit", 30, 0, &gl) == up_gpu_error);   // reported the error, then ended with exit code 0
	CHECK(fake("no_gpu", 30, 0, &gl) == -1);   // as realesrgan without a Vulkan GPU: no watchdog verdict
	// the probe: no Vulkan GPU is said as such (not a damaged install, though it ends with -1, and no driver fault); a
	// lost device on the test image is a driver fault
	std::string gpu, why;
	bool broken = true, fault = true;
	CHECK(!probe_gpu(job, self, "remacri-4x", -1, root / "no_gpu", gpu, why, broken, 32, &fault));
	CHECK(!broken && !fault && why.find("no Vulkan GPU") != std::string::npos);
	std::printf("  probe without a GPU -> %s\n", why.c_str());
	CHECK(!probe_gpu(job, self, "remacri-4x", -1, root / "lost_exit", gpu, why, broken, 32, &fault));
	CHECK(!broken && fault && why.find("vkQueueSubmit failed -4") != std::string::npos);

	// pictures below 64 px are padded with their edge pixels (centred) and cropped back after the upscaler
	const Pic tiny = pic_layout("k", 10, 6, true), wide = pic_layout("w", 100, 8, false);
	CHECK(tiny.pw == 64 && tiny.ph == 64 && tiny.px == 27 && tiny.py == 29 && wide.pw == 100 && wide.ph == 64 && wide.px == 0 && wide.py == 28);
	CHECK(pic_layout("b", 300, 64, false).pw == 300 && pic_layout("b", 300, 64, false).py == 0);
	std::vector<uint8_t> rgb(10 * 6 * 3), al(10 * 6);
	for (size_t i = 0; i < al.size(); ++i)
	{
		rgb[i * 3] = uint8_t(i * 4);
		rgb[i * 3 + 1] = uint8_t(255 - i);
		rgb[i * 3 + 2] = uint8_t(i * 7);
		al[i] = uint8_t(i * 3);
	}
	const std::vector<uint8_t> padded = pad_edge(rgb, tiny, 3);
	CHECK(padded.size() == 64 * 64 * 3 && std::memcmp(&padded[0], &rgb[0], 3) == 0 && std::memcmp(&padded[(64 * 64 - 1) * 3], &rgb[(10 * 6 - 1) * 3], 3) == 0);
	CHECK(std::memcmp(&padded[(size_t(29 + 2) * 64 + 27 + 3) * 3], &rgb[(2 * 10 + 3) * 3], 3) == 0);

	// the GPU pass: every picture in ONE upscaler run (the stand-in, see main), each used as soon as its output is there
	const auto gpu_test = [&](const char* mode, const std::vector<Pic>& pics, int answer, GpuRun& r, std::string& log_text)
	{
		const fs::path in = root / mode, up = root / (std::string(mode) + "_up"), log = root / (std::string(mode) + ".log");
		fs::create_directories(in, ec);
		fs::create_directories(up, ec);
		for (const Pic& pc : pics)
		{
			std::vector<uint8_t> c(size_t(pc.w) * pc.h * 3), a(size_t(pc.w) * pc.h);
			for (size_t i = 0; i < a.size(); ++i) c[i * 3] = c[i * 3 + 1] = c[i * 3 + 2] = a[i] = uint8_t(i);
			if (pc.key == "k") c = rgb, a = al;
			CHECK(write_pic(pc, in, c, a));
		}
		size_t ticks = 0;
		const auto t = std::chrono::steady_clock::now();
		r = gpu_pass(job, self, "realesrgan-x4plus", -1, normal_settings, in, up, log, root, pics, [&](size_t i)
		{
			std::vector<uint8_t> c, a;
			if (!load_pic(pics[i], up, c, a)) return 0;
			if (pics[i].key == "k")
				for (uint32_t y = 0; y < 24; ++y)
					for (uint32_t x = 0; x < 40; ++x)
					{
						const size_t s = size_t(y / 4) * 10 + x / 4, d = size_t(y) * 40 + x;
						if (std::memcmp(&c[d * 3], &rgb[s * 3], 3) != 0 || a[d] != al[s]) return 0;   // a shifted crop
					}
			return answer;
		}, [&](size_t) { ++ticks; }, 3);
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
		std::vector<uint8_t> d;
		read_file(log, d);
		log_text.assign(d.begin(), d.end());
		std::printf("  GPU pass %-8s -> rc %d, %zu of %zu made%s after %.1f s\n", mode, r.rc, r.made, pics.size(), r.aborted ? ", aborted" : "", s);
		CHECK(s < 20 && ticks > 0);
	};
	const std::vector<Pic> pics = { tiny, wide, pic_layout("x", 70, 70, false) };
	GpuRun r;
	std::string lt;
	gpu_test("allok", pics, 1, r, lt);
	CHECK(r.rc == 0 && r.made == 3 && !r.aborted && r.done == std::vector<char>({ 1, 1, 1 }));
	CHECK(gpu_name_in(lt) == "Fake GPU");
	GpuOutcome g = gpu_outcome(r.rc, lt, r.made, pics.size(), r.line);
	CHECK(!g.fault && g.what.empty());
	// the GPU lost after two pictures: those two are kept, the upscaler is ended at once, the third goes to the CPU
	gpu_test("partial", pics, 1, r, lt);
	CHECK(r.rc == up_gpu_error && r.made == 2 && r.done[2] == 0 && r.line == "vkQueueSubmit failed -4");
	g = gpu_outcome(r.rc, lt, r.made, pics.size(), r.line);
	CHECK(g.fault && g.what == "vkQueueSubmit failed -4");
	// a texture that cannot be committed stops the run (and the upscaler with it)
	gpu_test("partial2", pics, -1, r, lt);
	CHECK(r.aborted && r.made == 0);
	// what the other endings mean
	CHECK(gpu_outcome(up_stalled, "", 1, 3, "").fault);
	CHECK(gpu_outcome(0, "", 0, 3, "").fault && gpu_outcome(1, "", 3, 3, "").fault);   // no output at all / a crash of it
	CHECK(gpu_outcome(0, "x\nGPU_STOP: vkQueueSubmit failed -4\n", 3, 3, "").fault);   // its own stop, even with exit code 0
	g = gpu_outcome(0, "", 2, 3, "");   // a picture did not come out: the CPU makes it, no fault
	CHECK(!g.fault && g.what.find("1 of 3") != std::string::npos);
	CHECK(!gpu_outcome(-1, no_gpu, 0, 3, "").fault && !gpu_outcome(int(0xC0000135u), "", 0, 3, "").fault);
	CHECK(gpu_outcome(-1, no_gpu, 0, 3, "").unavailable && gpu_outcome(int(0xC0000135u), "", 0, 3, "").unavailable);
	CHECK(!gpu_outcome(up_stalled, "", 1, 3, "").unavailable && !gpu_outcome(0, "", 2, 3, "").unavailable);
	// the CPU's fast textures are made again on the GPU only when it can be used: not when its upscaler / model is
	// missing (download failed), nor after a run found no usable GPU (unless --use-gpu); else every run remade the pack
	fs::remove(root / unavailable_file, ec);
	CHECK(remake_cpu_textures(true, false, root) && !remake_cpu_textures(false, false, root) && !remake_cpu_textures(false, true, root));
	CHECK(write_file(root / unavailable_file, "1", 1));
	CHECK(!remake_cpu_textures(true, false, root) && remake_cpu_textures(true, true, root));
	// with gpu_unavailable.txt, a small test image tells whether the GPU works now: it does -> the file goes (the fast
	// textures are made again); still no GPU -> it stays; a lost device -> gpu_fault.txt and its log are kept
	CHECK(recheck_gpu(job, self, "x4plus", -1, root, root / "no_gpu", root, why) == Recheck::unavailable);
	CHECK(fs::exists(root / unavailable_file) && !fs::exists(root / fault_file) && !fs::exists(root / running_marker));
	CHECK(recheck_gpu(job, self, "x4plus", -1, root, root / "allok", root, why) == Recheck::gpu_ok);
	CHECK(!fs::exists(root / unavailable_file) && remake_cpu_textures(true, false, root) && !fs::exists(root / running_marker));
	CHECK(write_file(root / unavailable_file, "1", 1));
	CHECK(recheck_gpu(job, self, "x4plus", -1, root, root / "lost_exit", root, why) == Recheck::fault);
	CHECK(fs::exists(root / fault_file) && fs::exists(root / crashed_log) && !fs::exists(root / running_marker));
	// no probe log is left for the next run to take for a new crash (the log is moved to upscaler_crashed.log)
	CHECK(!fs::exists(root / "lost_exit") && !fs::exists(root / "allok") && !fs::exists(root / "no_gpu"));
	fs::remove_all(root, ec);
	return 0;
}

// The stand-in upscaler for test_gpu_safety: "-i <...\mode> -o <out> ..." or "-i <...\mode\probe.png> ..." (probe_gpu).
// allok / partial*: each PNG of the input folder 4x (nearest), partial* stops after three files (the first two pictures, one with alpha) with a lost device (and goes on
// printing it, as ncnn does)
static int fake_upscaler(const std::string& in, const std::string& outp)
{
	const fs::path ip(in);
	const std::string mode = (ip.has_extension() ? ip.parent_path().filename() : ip.filename()).string();
	if (mode == "no_gpu")
	{
		std::fprintf(stderr, "vkCreateInstance failed -9\ninvalid gpu device\n");
		return -1;
	}
	std::fprintf(stderr, "[0 Fake GPU]  queueC=2[8]\n");
	if (mode == "allok" || mode.compare(0, 7, "partial") == 0)
	{
		std::vector<fs::path> files;
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(ip, ec)) files.push_back(e.path());
		if (ip.has_extension()) files = { ip };   // the probe's test image -> outp
		std::sort(files.begin(), files.end());
		size_t n = 0;
		for (const fs::path& f : files)
		{
			if (mode != "allok" && n == 3)
				for (;;) { std::fprintf(stderr, "vkQueueSubmit failed -4\n"); Sleep(100); }
			int w = 0, h = 0, c = 0;
			stbi_uc* d = stbi_load(f.string().c_str(), &w, &h, &c, 3);
			if (!d) return 1;
			std::vector<uint8_t> o(size_t(w) * 4 * h * 4 * 3);
			for (int y = 0; y < h * 4; ++y)
				for (int x = 0; x < w * 4; ++x) std::memcpy(&o[(size_t(y) * w * 4 + x) * 3], d + (size_t(y / 4) * w + x / 4) * 3, 3);
			stbi_image_free(d);
			stbi_write_png((ip.has_extension() ? fs::path(outp) : fs::path(outp) / f.filename()).string().c_str(), w * 4, h * 4, 3, o.data(), w * 4 * 3);
			std::fprintf(stderr, "100.00%%\n");
			n++;
		}
		return 0;
	}
	if (mode == "gpu_error" || mode == "lost_exit")
	{
		std::fprintf(stderr, "0.00%%\nvkQueueSubmit failed -4\n");
		if (mode == "lost_exit") return 0;
		for (;;) { Sleep(100); std::fprintf(stderr, "0.00%%\nvkQueueSubmit failed -4\n"); }
	}
	if (mode == "hang") for (;;) Sleep(1000);
	if (mode == "busy") for (;;) { Sleep(200); std::fprintf(stderr, "1.00%%\n"); }
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 2) return 2;
	const std::string m = argv[1];
	if (m == "-i" && argc > 4) return fake_upscaler(argv[2], argv[4]);
	int rc = 2;
	try
	{
		if (m == "dds") rc = test_dds();
		else if (m == "folder" && argc > 2) rc = test_folder(argv[2]);
		else if (m == "xvolume" && argc > 3) rc = test_xvolume(argv[2], argv[3]);
		else if (m == "upscaler" && argc > 2) rc = test_upscaler(argv[2]);
		else if (m == "gpu_safety" && argc > 2) rc = test_gpu_safety(argv[2]);
		else if (m == "real" && argc > 2) rc = test_real(argv[2]);
	}
	catch (const std::exception& e)
	{
		std::printf("FAIL exception: %s\n", e.what());
		return 1;
	}
	if (rc == 77) return 77;
	std::printf("%s: %s\n", m.c_str(), g_fail ? "FAILED" : "ok");
	return g_fail ? 1 : rc;
}
