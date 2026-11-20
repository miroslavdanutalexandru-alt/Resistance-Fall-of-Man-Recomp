// hd_ui_game_r1.h: the Resistance: Fall of Man backend of HDUIPack.exe (hd_ui_pack.cpp, the same file in all three
// ports). Everything game-specific is behind this header: where the game files are, how its archive is read,
// which textures are menu / HUD art, the key the game's texture upload computes for each (r1_hd_ui.h, shared with the
// D3D12 backend's RSX_HD_UI path) and how a texture's top level is decoded.
//
// The game's textures: USRDIR\game.psarc (PSARC, zlib) holds IGHW pairs, a header (.tph / .bnkh, section 0x5300 of
// 0x20-byte records {u32 offset into the texel data, FORMAT, ADDRESS, CONTROL0, REMAP, FILTER, IMAGE_RECT, CONTROL3})
// and the texel data (.tp / .bnk, section 0x5400). The game copies the texel bytes to video memory as they are and
// points the RSX texture registers (the record) at them, so the key computed here from the archive bytes is the key
// the upload computes from guest memory.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "miniz.h"
#include "r1_hd_ui.h"

// The core the generic tool uses (R2/R3: r2_hd_ui.h's hdui namespace)
namespace hdui
{
	constexpr uint32_t GCM_A8R8G8B8 = R1HD_A8R8G8B8, GCM_DXT1 = R1HD_DXT1, GCM_DXT23 = R1HD_DXT23, GCM_DXT45 = R1HD_DXT45;
	constexpr uint32_t GCM_LN = R1HD_LN;

	inline std::string sfmt(const char* f, ...)
	{
		char b[1024];
		va_list ap;
		va_start(ap, f);
		std::vsnprintf(b, sizeof b, f, ap);
		va_end(ap);
		return b;
	}
	inline uint32_t base_format(uint32_t gcm) { return r1hd_base(gcm); }
	inline bool is_dxt(uint32_t fmt) { return r1hd_is_dxt(fmt) != 0; }
	inline uint32_t block_bytes(uint32_t fmt) { return r1hd_block_bytes(fmt); }
	inline const char* fmt_name(uint32_t fmt) { return r1hd_fmt_name(fmt); }
	inline bool handled_format(uint32_t fmt) { return r1hd_handled_format(fmt) != 0; }
	inline uint32_t level_dim(uint32_t base, uint32_t scale, uint32_t l) { return r1hd_level_dim(base, scale, l); }
	inline bool key_of_name(const std::string& n, uint64_t& k) { return n.size() >= 16 && r1hd_key_of_name(n.c_str(), &k); }
	inline std::vector<uint8_t> dds_header(uint32_t w, uint32_t h, uint32_t fmt, uint32_t mips)
	{
		uint32_t hdr[32];
		r1hd_dds_header(hdr, w, h, fmt, mips);
		std::vector<uint8_t> v(128);
		std::memcpy(v.data(), hdr, 128);
		return v;
	}
}

namespace hdgame
{
	namespace fs = std::filesystem;

	constexpr const char* game_name = "Resistance: Fall of Man";
	constexpr const char* archive_name = "game.psarc";

	// The D3D12 backend creates a DXT texture only in whole 4x4 blocks, so the HD size must be one
	constexpr bool dxt_whole_blocks = true;

	inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
	inline uint64_t be40(const uint8_t* p) { return (uint64_t(p[0]) << 32) | be32(p + 1); }

	// ------------------------------------------------------------------------------------------------ PSARC
	// {"PSAR", u16 major, u16 minor, "zlib", u32 toc length, u32 entry size, u32 entries, u32 block size, u32 flags},
	// entries {md5[16], u32 first block, u40 size, u40 offset}, then the block size table (u16 for 64 KB blocks:
	// 0 = a whole raw block, the chunk's own size = stored raw, else zlib). Entry 0 is the member name list.
	class Psarc
	{
	public:
		~Psarc() { if (f_) std::fclose(f_); }
		bool open(const fs::path& p)
		{
			f_ = _wfopen(p.c_str(), L"rb");
			if (!f_) return false;
			uint8_t h[32];
			if (std::fread(h, 1, 32, f_) != 32 || std::memcmp(h, "PSAR", 4) || std::memcmp(h + 8, "zlib", 4)) return false;
			const uint32_t toc = be32(h + 12), esz = be32(h + 16), n = be32(h + 20);
			bs_ = be32(h + 24);
			if (toc < 32 || toc > (256u << 20) || esz < 30 || !n || uint64_t(n) * esz > toc - 32 || !bs_) return false;
			std::vector<uint8_t> t(toc - 32);
			if (std::fread(t.data(), 1, t.size(), f_) != t.size()) return false;
			for (uint32_t i = 0; i < n; ++i)
			{
				const uint8_t* e = &t[size_t(i) * esz];
				ent_.push_back(Entry{ be32(e + 16), be40(e + 20), be40(e + 25) });
			}
			const size_t bpz = bs_ <= 0x10000 ? 2 : bs_ <= 0x1000000 ? 3 : 4;
			for (size_t o = size_t(n) * esz; o + bpz <= t.size(); o += bpz)
			{
				uint32_t v = 0;
				for (size_t k = 0; k < bpz; ++k) v = (v << 8) | t[o + k];
				z_.push_back(v);
			}
			std::vector<uint8_t> names;
			if (!read_entry(0, names)) return false;
			std::string cur;
			for (size_t i = 0; i <= names.size(); ++i)
			{
				const char c = i < names.size() ? char(names[i]) : '\n';
				if (c == '\n' || c == '\r' || c == 0)
				{
					if (!cur.empty()) idx_[cur] = uint32_t(names_.size() + 1), names_.push_back(cur);
					cur.clear();
				}
				else cur += c;
			}
			return true;
		}
		const std::vector<std::string>& names() const { return names_; }
		bool read(const std::string& name, std::vector<uint8_t>& out)
		{
			const auto it = idx_.find(name);
			return it != idx_.end() && read_entry(it->second, out);
		}

	private:
		struct Entry
		{
			uint32_t block;
			uint64_t size, offset;
		};
		bool read_entry(uint32_t i, std::vector<uint8_t>& out)
		{
			out.clear();
			if (i >= ent_.size() || ent_[i].size > (1ull << 31)) return false;
			const Entry& e = ent_[i];
			out.reserve(size_t(e.size));
			if (_fseeki64(f_, int64_t(e.offset), SEEK_SET)) return false;
			std::vector<uint8_t> in, tmp(bs_);
			for (uint32_t b = e.block; out.size() < e.size; ++b)
			{
				if (b >= z_.size()) return false;
				const size_t chunk = size_t(std::min<uint64_t>(bs_, e.size - out.size()));
				const size_t stored = z_[b] ? z_[b] : bs_;
				in.resize(stored);
				if (std::fread(in.data(), 1, stored, f_) != stored) return false;
				mz_ulong n = mz_ulong(bs_);
				if (stored != chunk && mz_uncompress(tmp.data(), &n, in.data(), mz_ulong(stored)) == MZ_OK && n == chunk)
					out.insert(out.end(), tmp.begin(), tmp.begin() + n);
				else if (stored >= chunk) out.insert(out.end(), in.begin(), in.begin() + chunk);
				else return false;
			}
			return true;
		}
		FILE* f_ = nullptr;
		uint32_t bs_ = 0;
		std::vector<Entry> ent_;
		std::vector<uint32_t> z_;
		std::vector<std::string> names_;
		std::map<std::string, uint32_t> idx_;
	};

	// ------------------------------------------------------------------------------------------------ textures
	// IGHW container: {u32 'IGHW', u32 version, u32 sections, ...}, section table at +32 of 16-byte entries
	// {u32 id, u32 offset, u32 count (0x10000000 flag: an array of `size`-byte records), u32 size}
	inline bool ighw_section(const std::vector<uint8_t>& b, uint32_t id, uint32_t& off, uint32_t& count)
	{
		if (b.size() < 48 || std::memcmp(b.data(), "IGHW", 4)) return false;
		const uint32_t n = be32(&b[8]);
		if (n > 256 || 32 + size_t(n) * 16 > b.size()) return false;
		for (uint32_t i = 0; i < n; ++i)
		{
			const uint8_t* e = &b[32 + i * 16];
			if (be32(e) != id) continue;
			off = be32(e + 4);
			count = be32(e + 8) & 0xFFFFFF;
			return off <= b.size();
		}
		return false;
	}

	// One texture as the RSX sees it (the texture registers the asset stores)
	struct TexInfo
	{
		uint16_t w = 0, h = 0;
		uint32_t gcm = 0;       // with LN / UN
		uint32_t fmt = 0;       // without
		uint32_t mips = 1;      // the levels the upload sends (r1hd_upload_mips)
		uint32_t mips_reg = 1;  // SET_TEXTURE_FORMAT's level count
		uint32_t pitch = 0;
		size_t data_off = 0;    // into the texel member
		uint64_t key = 0;
		std::string skip;       // why it is not packed ("" = it is)
	};

	// The layout the upload reads level 0 with (rsx_d3d12_backend.c: the pitch only counts for a 1-level texture)
	inline rsx_tex_layout layout0(const TexInfo& t)
	{
		rsx_tex_layout tl;
		rsx_texture_layout_pitched(t.gcm, t.w, t.h, t.mips_reg > 1 ? 0 : t.pitch, &tl);
		return tl;
	}

	// Parses a header / texel pair: one TexInfo per texture record, keyed from the texel bytes
	inline std::vector<TexInfo> parse_textures(const std::vector<uint8_t>& header, const std::vector<uint8_t>& texel)
	{
		std::vector<TexInfo> outv;
		uint32_t roff = 0, rcount = 0, toff = 0, tcount = 0;
		if (!ighw_section(header, 0x5300, roff, rcount) || !ighw_section(texel, 0x5400, toff, tcount)) return outv;
		for (uint32_t i = 0; i < rcount && roff + size_t(i + 1) * 0x20 <= header.size(); ++i)
		{
			const uint8_t* r = &header[roff + size_t(i) * 0x20];
			const uint32_t off = be32(r), format = be32(r + 4), rect = be32(r + 24), ctrl3 = be32(r + 28);
			TexInfo t;
			t.w = uint16_t(rect >> 16);
			t.h = uint16_t(rect & 0xFFFF);
			t.gcm = (format >> 8) & 0xFF;
			t.fmt = r1hd_base(t.gcm);
			t.mips_reg = format >> 16;
			const uint32_t dim = (format >> 4) & 0xF, depth = ctrl3 >> 20;
			const bool cube = (format & 4) != 0, no_border = (format & 8) != 0;
			t.pitch = ctrl3 & 0xFFFFF;
			t.data_off = size_t(toff) + off;
			const char* bad = r1hd_ineligible(t.gcm, t.w, t.h, cube, 0);
			if (!r1hd_handled_format(t.fmt)) t.skip = hdui::sfmt("format 0x%02x", t.gcm);
			else if (dim != 2 || cube || depth > 1) t.skip = "not a 2D texture";
			else if (!r1hd_is_dxt(t.fmt) && !no_border) t.skip = "has border texels";
			else if (bad) t.skip = bad;
			if (t.skip.empty())
			{
				const uint64_t avail = t.data_off < texel.size() ? texel.size() - t.data_off : 0;
				const uint8_t* p = texel.data() + std::min(t.data_off, texel.size());
				uint64_t need = 0;
				if (!r1hd_texture_key(p, avail, t.gcm, t.w, t.h, t.mips_reg, t.pitch, 0, 1, &t.key, &t.mips, &need))
					t.skip = "texel data truncated";
				else if (r1hd_is_fe_atlas(p, layout0(t).row_bytes, t.gcm, t.w, t.h))
					t.skip = "the menu button atlas (the port draws its own buttons)";
			}
			outv.push_back(t);
		}
		return outv;
	}

	// Menu, HUD, font, map and splash art: the Anark menus' texture banks, the GUI sheets, the fonts, the level maps
	// and the loading splashes. Level / effect textures are not packed.
	inline bool is_ui_member(const std::string& ln)
	{
		const auto starts = [&](const char* p) { return ln.compare(0, std::strlen(p), p) == 0; };
		const auto ends = [&](const char* s) { const size_t n = std::strlen(s); return ln.size() >= n && ln.compare(ln.size() - n, n, s) == 0; };
		if (ends(".bnkh")) return starts("/built/anark/");
		if (!ends(".tph")) return false;
		return starts("/art/hud/") || starts("/built/gui/") || starts("/built/fonts/") ||
			(starts("/built/levels/") && ends("_ps3guimaptexture.tph"));
	}

	struct Source
	{
		TexInfo tex;
		std::string archive, header, texel;   // archive path, member names
		bool image = false;                   // a loose image (none in R1; see hd_ui_pack.cpp's pack_name)
	};

	// Every packable texture of the archive, by key (the first asset wins a duplicate)
	struct Scan
	{
		std::map<uint64_t, Source> textures;
		std::map<std::string, unsigned> skipped;
		unsigned assets = 0, archives = 0;
	};

	inline std::string lower_ascii(std::string s)
	{
		for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	// progress(done, total): over the archive's UI assets
	inline Scan scan_archives(const fs::path& usrdir, const std::function<void(unsigned, unsigned)>& progress)
	{
		Scan sc;
		const fs::path arc = usrdir / archive_name;
		Psarc pa;
		if (!pa.open(arc)) return sc;
		sc.archives = 1;
		std::set<std::string> have(pa.names().begin(), pa.names().end());
		std::vector<std::pair<std::string, std::string>> pairs;
		for (const auto& n : pa.names())
		{
			if (!is_ui_member(lower_ascii(n))) continue;
			const std::string tx = n.substr(0, n.size() - 1);   // .tph -> .tp, .bnkh -> .bnk
			if (have.count(tx)) pairs.emplace_back(n, tx);
		}
		for (size_t i = 0; i < pairs.size(); ++i)
		{
			if (progress) progress(unsigned(i), unsigned(pairs.size()));
			std::vector<uint8_t> hb, tb;
			if (!pa.read(pairs[i].first, hb) || !pa.read(pairs[i].second, tb)) continue;
			const auto texs = parse_textures(hb, tb);
			if (texs.empty()) continue;
			sc.assets++;
			for (const auto& t : texs)
			{
				if (!t.skip.empty())
				{
					sc.skipped[t.skip]++;
					continue;
				}
				if (sc.textures.count(t.key)) continue;
				sc.textures[t.key] = Source{ t, arc.string(), pairs[i].first, pairs[i].second };
			}
		}
		if (progress) progress(unsigned(pairs.size()), unsigned(pairs.size()));
		return sc;
	}

	// The texel member of a source (the bytes its key was computed from)
	inline bool read_texel(const Source& s, std::vector<uint8_t>& tb)
	{
		Psarc pa;
		return pa.open(fs::path(s.archive)) && pa.read(s.texel, tb) && s.tex.data_off < tb.size();
	}

	// Block rows of a DXT texture are this many bytes apart
	inline uint32_t dxt_row_bytes(const TexInfo& t) { return layout0(t).row_bytes; }

	// Level 0 of an A8R8G8B8 texture -> RGBA (w * 4 bytes a row), decoded as the upload decodes it
	inline void decode_argb(const uint8_t* d, const TexInfo& t, uint8_t* rgba)
	{
		const rsx_tex_layout tl = layout0(t);
		rsx_texture_decode_reference(rgba, t.w * 4u, d, t.w, t.h, &tl, 0);
	}

	inline fs::path find_usrdir(const fs::path& root, const fs::path& given)
	{
		std::error_code ec;
		std::vector<fs::path> cand;
		if (!given.empty()) cand = { given / "USRDIR", given / "PS3_GAME" / "USRDIR", given };
		else cand = { root / "gamefiles" / "PS3_GAME" / "USRDIR", root / "game" / "PS3_GAME" / "USRDIR", root / "PS3_GAME" / "USRDIR", root / "game" / "USRDIR" };
		for (const auto& c : cand)
			if (fs::is_regular_file(c / archive_name, ec)) return c;
		return {};
	}

	inline std::string text_no_game() { return "the game files were not found (gamefiles\\PS3_GAME\\USRDIR\\game.psarc next to the launcher): run Setup first"; }
	inline std::string text_no_textures(const fs::path& usrdir, unsigned archives)
	{
		return hdui::sfmt("no menu / HUD textures were found in %s (%u archives read)", (usrdir / archive_name).string().c_str(), archives);
	}
}
