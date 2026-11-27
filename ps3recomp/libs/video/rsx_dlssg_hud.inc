/* DLSS Frame Generation: the HUD split (rsx_dlssg.inc has the rest). Included by rsx_d3d12_backend.c after
 * rsx_fsr.inc; hooked into render_frame_impl's draw loop.
 *
 * The renderer knows every draw, so instead of guessing the HUD from pixels (what injectors have to do) it
 * classifies the draws that reach the display buffer after the frame's DLSS inputs were evaluated:
 *   a HUD draw goes to the display buffer (no offscreen target, no MRT), has depth test (or an ALWAYS test)
 *   and depth write off and no stencil, writes RGB, samples no render target (post passes -- bloom, the
 *   upscaled scene, the screen copy -- do; HUD sprites and fonts sample ordinary textures) and does not
 *   read the display buffer, and blends either as alpha (SRC_ALPHA / ONE, INV_SRC_ALPHA) or additively
 *   (SRC_ALPHA / ONE, ONE) with an ADD equation.
 * A run of such draws is the HUD. Before its first draw the display buffer is copied (the HUD-less frame);
 * every HUD draw is drawn a second time into a cleared UI layer with the alpha channel blended so the layer
 * ends up premultiplied (final = ui.rgb + (1 - ui.a) * hudless), which is what DLSS-G recomposes over the
 * generated frames (additive draws get alpha coverage too, and the layer's colour is rebuilt from the real
 * frame at present -- RSX_FG_HUD_DIM). The real frame on screen is never changed.
 * Any other draw to the display buffer closes the run; a later HUD draw starts a new one (the frame so far,
 * earlier HUD included, becomes the HUD-less image). The frame gets no UI layer at all (the whole frame is
 * generated, HUD included, as without the split) when it is unsure: more than four runs, the run changes
 * display buffer, a HUD pipeline variant is not available, the display buffer is replaced at frame end
 * (composite / guest framebuffer paths), or no run was open at the end of the frame. */

static int s_fg_hlog_left;         /* RFOM_FG_HUDLOG frames still to log */
static int s_fg_hlog_cfg = -1;     /* RFOM_FG_HUDLOG (-1: not read yet) */
static u32 s_fg_hlog_at;           /* RFOM_FG_HUDLOG_AT: start at this upscaler frame (0 = when frame generation is on) */
static int s_fg_hlog_wait;         /* RFOM_FG_HUDLOG set, not started yet */
static int s_fg_hmv_pend;          /* print the next scene batch's per-draw motion matching (fg_hud_mv_dump) */
static u32 s_fg_hlog_frame, s_fg_hlog_n;

/* RFOM_FG_HUDLOG=N starts once frame generation is on (DLSS-G or FSR 3 FG; the level-start frames before it were
 * all the log used to catch) or at upscaler frame RFOM_FG_HUDLOG_AT ("[FSR] frame N"). Ctrl+Shift+F2 (game
 * window in front) logs the next N frames (3 when unset) at any time, plus the per-draw motion matching of the
 * next scene ([FG-MV]): press it where generated frames show artifacts. */
static void fg_hud_log_poll(void)
{
    if (s_fg_hlog_cfg < 0) {
        const char* e = getenv("RFOM_FG_HUDLOG");
        s_fg_hlog_cfg = e ? atoi(e) : 0;
        if (s_fg_hlog_cfg < 0) s_fg_hlog_cfg = 0;
        e = getenv("RFOM_FG_HUDLOG_AT");
        s_fg_hlog_at = e ? (u32)strtoul(e, NULL, 10) : 0;
        s_fg_hlog_wait = s_fg_hlog_cfg > 0;
        if (s_fg_hlog_wait && s_fg_hlog_at)
            fprintf(stderr, "[FG-HUD] RFOM_FG_HUDLOG=%d: logging starts at upscaler frame %u; Ctrl+Shift+F2 logs again%c",
                    s_fg_hlog_cfg, s_fg_hlog_at, 10);
        else if (s_fg_hlog_wait)
            fprintf(stderr, "[FG-HUD] RFOM_FG_HUDLOG=%d: logging starts when frame generation is on; Ctrl+Shift+F2 logs again%c",
                    s_fg_hlog_cfg, 10);
    }
    if (s_fg_hlog_wait && (s_fg_hlog_at ? s_fsr_frame_r >= s_fg_hlog_at : (s_fg_on || s_f3_on))) {
        s_fg_hlog_wait = 0; s_fg_hlog_left = s_fg_hlog_cfg; s_fg_hmv_pend = 1;
        fprintf(stderr, "[FG-HUD] logging %d frames from upscaler frame %u%c", s_fg_hlog_cfg, (unsigned)s_fsr_frame_r, 10);
    }
    { static int said = 0;
      if (!said && (s_fg_on || s_f3_on)) { said = 1;
          fprintf(stderr, "[FG-HUD] Ctrl+Shift+F2: log the HUD split and per-draw motion of the next frames ([FG-HUD] / [FG-MV] lines)%c", 10); } }
    static int was = 0;
    const int down = s_app_active && s_d3d.hwnd && GetForegroundWindow() == s_d3d.hwnd && (GetAsyncKeyState(VK_F2) & 0x8000) &&
                     (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (down && !was) {
        s_fg_hlog_wait = 0; s_fg_hlog_left = s_fg_hlog_cfg > 0 ? s_fg_hlog_cfg : 3; s_fg_hmv_pend = 1;
        fprintf(stderr, "[FG-HUD] Ctrl+Shift+F2: logging %d frames         from upscaler frame %u (frame generation %s)%c", s_fg_hlog_left,
                        (unsigned)s_fsr_frame_r, s_fg_on || s_f3_on ? "on" : "off", 10);
        if (s_fgd_on) fgd_user_mark("Ctrl+Shift+F2, FG-HUD / FG-MV log", (unsigned)s_fsr_frame_r);   /* RFOM_FG_DIAG */
    }
    was = down;
}

/* render_frame_impl, start of a guest frame: forget the previous frame's runs (the gate stays). */
static void fg_hud_frame_start(void)
{
    const int g = s_fgh.gate;
    if (s_fg_hlog_n) { s_fg_hlog_n = 0; s_fg_hlog_frame++; if (s_fg_hlog_left > 0) s_fg_hlog_left--; }
    fg_hud_log_poll();
    memset(&s_fgh, 0, sizeof s_fgh);
    s_fgh.gate = g;
}

/* render_frame_impl, end of a batch with its draw records still in place: after Ctrl+Shift+F2 / the HUD log start,
 * the first batch with scene draws prints how each was matched to its previous-frame draw (fsr_draws.txt of the
 * F12 capture, as stderr lines). */
static void fg_hud_mv_dump(const D3D12DrawRecord* draws, u32 nd)
{
    if (s_fgd_on && (s_fg_ready || s_f3_ready))   /* RFOM_FG_DIAG: this present's matching counts */
        for (u32 d = 0; d < nd && d < MAX_DRAWS; d++)
            if (draws[d].fsr == 1) s_fgdf.m[draws[d].fsr_mt < 6 ? draws[d].fsr_mt : 0]++;
    if (!s_fg_hmv_pend) return;
    u32 c[6] = {0}, ns = 0;
    for (u32 d = 0; d < nd && d < MAX_DRAWS; d++)
        if (draws[d].fsr == 1) { ns++; c[draws[d].fsr_mt < 6 ? draws[d].fsr_mt : 0]++; }
    if (!ns) return;
    s_fg_hmv_pend = 0;
    fprintf(stderr, "[FG-MV] upscaler frame %u: %u scene draws: exact %u instance %u nearest %u camera-split %u ambiguous %u"
                    " unmatched %u (frame generation %s)%c", (unsigned)s_fsr_frame_r, ns, c[1], c[2], c[3], c[4], c[5], c[0],
            s_fg_on || s_f3_on ? "on" : "off", 10);
    fprintf(stderr, "[FG-MV] d mt(0 none,1 exact,2 exact-instance,3 nearest,4 camera-split,5 ambiguous nearest) chain bd slot pslot fp vs tex0 tex1 count%c", 10);
    for (u32 d = 0; d < nd && d < MAX_DRAWS; d++) {
        const D3D12DrawRecord* dr = &draws[d];
        if (dr->fsr != 1) continue;
        fprintf(stderr, "[FG-MV] %u %u %u %g %u %d %08X %d %08X %08X %u%c", d, dr->fsr_mt, dr->fsr_chain, dr->fsr_bd, dr->fsr_slot,
                (int)dr->fsr_pslot, dr->fp_addr, dr->vs_idx, (u32)dr->tex[0].raw, (u32)dr->tex[1].raw,
                dr->ib_count ? dr->ib_count : dr->vertex_count, 10);
    }
}

/* display-buffer colour clear in the draw loop, and the guest frame's first clear */
static void fg_hud_clear(void) { s_fgh.open = 0; }

static int fg_hud_blend(u32 k, u32* alpha_bits)
{
    if (!(k & 1u)) return 0;
    const u32 sc = (k >> 1) & 31u, dc = (k >> 6) & 31u, oc = (k >> 21) & 7u;
    if (oc != (u32)D3D12_BLEND_OP_ADD) return 0;
    if ((sc == (u32)D3D12_BLEND_SRC_ALPHA || sc == (u32)D3D12_BLEND_ONE) && dc == (u32)D3D12_BLEND_INV_SRC_ALPHA) {
        *alpha_bits = ((u32)D3D12_BLEND_ONE << 11) | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 16) | ((u32)D3D12_BLEND_OP_ADD << 24);
        return 1;
    }
    if ((sc == (u32)D3D12_BLEND_SRC_ALPHA || sc == (u32)D3D12_BLEND_ONE) && dc == (u32)D3D12_BLEND_ONE) {
        /* Additive HUD parts (likely the dim unlit segments of the ammo / health digits, glows) used to leave
         * the UI alpha untouched: colour in the layer at alpha 0, which DLSS-G reads as "no UI here", so they
         * were missing from the generated frames and flickered. RSX_FG_HUD_DIM (default on) gives SRC_ALPHA/ONE
         * draws their src.a as coverage; fg_present rebuilds the colour so the composite still equals the real
         * frame. Only with the rebuild pass in place (else present copies the layer and generated frames would
         * darken), and not for ONE/ONE, whose src.a is no coverage (old alpha kept). */
        const int cov = sc == (u32)D3D12_BLEND_SRC_ALPHA && s_fg_dim_pso && fg_hud_dim();
        const u32 sa = cov ? (u32)D3D12_BLEND_ONE : (u32)D3D12_BLEND_ZERO;
        const u32 da = cov ? (u32)D3D12_BLEND_INV_SRC_ALPHA : (u32)D3D12_BLEND_ONE;
        *alpha_bits = (sa << 11) | (da << 16) | ((u32)D3D12_BLEND_OP_ADD << 24);
        return 1;
    }
    return 0;
}

/* 0 = a HUD draw; otherwise why not (RFOM_FG_HUDLOG prints it) */
static int fg_hud_why(const D3D12DrawRecord* dr, const int* cur_m, u32* key)
{
    if (!dr->fp_addr || dr->rt_mrt[0] || cur_m[0] >= 0 || dr_num_rts(dr) != 1) return 1;
    const u32 c = dr->cull;
    if (!(c & 0x100u) || (c & 0x400u)) return 2;                         /* depth state unknown, or written */
    if ((c & 0x200u) && ((c >> 12) & 7u) != 7u) return 3;                /* a real depth test */
    if (c >> 24) return 4;                                               /* stencil */
    if ((dr->cmask & 7u) != 7u) return 5;
    if (dr->samp_disp) return 6;
    for (int u = 0; u < RSX_UNITS; u++) if (dr->tex_rt[u] >= 0) return 7;
    u32 ab = 0;
    if (!fg_hud_blend(dr->blend_key, &ab)) return 8;
    *key = (dr->blend_key & ~((31u << 11) | (31u << 16) | (7u << 24))) | ab;
    return 0;
}
static int fg_hud_is(const D3D12DrawRecord* dr, const int* cur_m, u32* key) { return !fg_hud_why(dr, cur_m, key); }

/* RFOM_FG_HUDLOG=N (default 0, diagnostic): for N frames with DLSS inputs, one stderr line per draw to the
 * display buffer after the scene was tagged -- whether it went to the HUD layer and why not, the run state
 * and the draw's state -- to find HUD parts the split misses. Started by fg_hud_log_poll. */
static void fg_hud_log(const D3D12DrawRecord* dr, int why)
{
    if (s_fg_hlog_left <= 0) return;
    static const char* const wn[] = { "HUD", "fp/MRT", "depth-write", "depth-test", "stencil", "cmask",
                                      "reads-display", "samples-RT", "blend" };
    int trt = -1;
    for (int u = 0; u < RSX_UNITS; u++) if (dr->tex_rt[u] >= 0) { trt = dr->tex_rt[u]; break; }
    fprintf(stderr, "[FG-HUD] f%u #%u %s run=%u open=%d unsure=%d fp=%X blend=%08X cull=%08X cmask=%X tag=%X"
                    " samp_disp=%X tex_rt=%d tex0=%X %ux%u verts=%u vp=%ux%u%c",
            s_fg_hlog_frame, s_fg_hlog_n++, wn[why < 0 || why > 8 ? 0 : why], s_fgh.runs, s_fgh.open, s_fgh.unsure,
            (unsigned)dr->fp_addr, (unsigned)dr->blend_key, (unsigned)dr->cull, (unsigned)dr->cmask,
            (unsigned)dr->disp_tag, (unsigned)dr->samp_disp, trt, (unsigned)dr->tex[0].off, (unsigned)dr->tex[0].w,
            (unsigned)dr->tex[0].h, (unsigned)(dr->ib_count ? dr->ib_count : dr->vertex_count),
            (unsigned)dr->vp_w, (unsigned)dr->vp_h, 10);
}

/* Every draw the loop issues, just before it is issued, with its state bound. */
static void fg_hud_draw(const D3D12DrawRecord* dr, int cur_rt, const int* cur_m, ID3D12PipelineState* bound_pso,
                        D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv, u32 fi)
{
    if (!s_fgh.gate || !s_fgh.tagged || cur_rt >= 0) return;
    u32 key = 0;
    const int why = fg_hud_why(dr, cur_m, &key);
    if (s_fg_hlog_left > 0) fg_hud_log(dr, why);
    if (s_fgh.unsure) return;
    if (why) { s_fgh.open = 0; return; }
    if (s_fgh.open && dr->disp_tag && dr->disp_tag != s_fgh.tag) { s_fgh.unsure = 1; return; }
    fp_set_unnorm(dr);
    ID3D12PipelineState* up = vp_get_fp_pso(dr_vs_key(dr), dr->fp_addr, key, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                            dr->fp_exp32, dr->cmask | 8u, dr->cull, dr_cube_mask(dr));
    if (!up) { s_fgh.unsure = 1; return; }
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    if (!s_fgh.open) {
        if (++s_fgh.runs > 4) { s_fgh.unsure = 1; return; }
        s_fgh.open = 1; s_fgh.tag = dr->disp_tag;
        D3D12_RESOURCE_STATES ds = D3D12_RESOURCE_STATE_RENDER_TARGET;
        fg_tr(cl, s_d3d.render_targets[fi], &ds, D3D12_RESOURCE_STATE_COPY_SOURCE);
        fg_tr(cl, s_fg_hl, &s_fg_hl_st, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->lpVtbl->CopyResource(cl, s_fg_hl, s_d3d.render_targets[fi]);
        fg_tr(cl, s_d3d.render_targets[fi], &ds, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fg_tr(cl, s_fg_ui, &s_fg_ui_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
        static const float zero[4] = {0, 0, 0, 0};
        cl->lpVtbl->ClearRenderTargetView(cl, fg_rtv(0), zero, 0, NULL);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE ur = fg_rtv(0);
    cl->lpVtbl->SetPipelineState(cl, up);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &ur, FALSE, dsv.ptr ? &dsv : NULL);
    if (dr->ib_count && s_vp_ib)
        cl->lpVtbl->DrawIndexedInstanced(cl, dr->ib_count, 1, dr->ib_first, dr->gf ? 0 : (INT)(dr->vb_byte_offset / 256), 0);
    else
        cl->lpVtbl->DrawInstanced(cl, dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
    cl->lpVtbl->SetPipelineState(cl, bound_pso);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, dsv.ptr ? &dsv : NULL);
    s_fgh.ndraw++;
}

/* Frame end, before the guest-framebuffer / composite paths may replace the display buffer. */
static void fg_hud_frame_end(void)
{
    static int gfb = -1;
    if (gfb < 0) gfb = getenv("GCM_GUEST_FB") != NULL;
    if (s_fgh.open && (s_composite_src || gfb)) s_fgh.unsure = 1;
}

/* fg_present: DLSS gave up for FSR (no inputs for DLSS-G any more) -- say so once. */
static void fg_late_check(void)
{
    static int said = 0;
    if (!said && s_fg_ready && s_dlss_bad) {
        said = 1;
        fg_log("DLSS Super Resolution failed and the port switched to FSR -- frame generation stays off");
    }
}
