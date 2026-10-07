// PS2 GS hardware renderer driver: GS bring-up, GIF DMA ring, frame life cycle (included by ps2_hwd.c).
//
// All GS commands of a frame are written into one stream of 16-byte words held in NBUF buffers of BUF_QW words.
// A full buffer (or the end of the frame) is queued; the DMAC completion interrupt starts the next queued buffer, so the
// EE only sleeps when it is a whole ring ahead of the GS. The stream is strictly ordered: texture uploads, CLUT writes,
// screen copies and draws execute in the order they were issued, so VRAM can be reused inside a frame.

static void poll_dma(void);
static void tex_cap_control(void);
static void tx_report(void);
static void lod_frame_end(void);
static void ov_flush_all(void); // draws queued by the light overlay (ps2_hw_draw.inc); every frame boundary and read-back first
static void rel_poll(void); // OPT9: engine data referenced by DMA is unlocked when the transfer is done (ps2_hw_tex.inc)
static void rel_release_buf(int i);
static void rel_release_all(void);

// DMA tags of a source chain (the GIF channel reads one tag per element: the tag itself is not sent to the GIF)
#define DMAT_REFE 0ull // send qwc qwords from addr, then end
#define DMAT_CNT 1ull // send the qwords that follow the tag, continue after them
#define DMAT_REF 3ull // send qwc qwords from addr, continue with the tag after this one
#define DMAT_END 7ull // send the qwords that follow the tag, then end

static inline u64 dma_tag(u32 qwc, u64 id, u32 addr)
{
	return (u64)qwc | (id << 28) | ((u64)addr << 32);
}

static void dma_start_locked(void) // interrupts disabled
{
	int i;

	if (H.dma_busy || H.q_n == 0)
		return;
	if (H.gate && H.pend && H.q_fid[H.q_head] != H.pend_fid)
		return; // a buffer of the next frame: the GS starts it when the finished frame has been shown (do_flip)
	i = H.q_idx[H.q_head];
	H.q_head = (H.q_head + 1) % NBUF;
	H.q_n--;
	H.dma_cur = i;
	H.dma_busy = 1;
	if (V.on) // PS2-HW-44: the chain goes to the VIF (channel 1) with the VIF codes of its tags (TTE)
	{
		*D1_TADR = phys(H.ring[i]);
		*D1_MADR = 0;
		*D1_QWC = 0;
		__asm__ volatile("sync.l");
		*D1_CHCR = 0x145; // DIR = from memory, source chain mode, TTE, start
	}
	else
	{
		*D2_TADR = phys(H.ring[i]);
		*D2_MADR = 0;
		*D2_QWC = 0;
		__asm__ volatile("sync.l");
		*D2_CHCR = 0x105; // DIR = from memory, source chain mode, start
	}
	__asm__ volatile("sync.p");
	H.st.dma_kicks++;
}

static int dmac_gif_handler(int ch)
{
	(void)ch;
	H.irq_seen++;
	if (H.dma_busy && !(DCH_CHCR & 0x100))
	{
		H.dma_busy = 0;
		H.pending--;
		H.done_seq++;
		iSignalSema(H.sema_dma);
	}
	dma_start_locked();
	ExitHandler();
	return 0;
}

static void poll_dma(void)
{
	u32 irq = DIntr();

	if (H.dma_busy && !(DCH_CHCR & 0x100))
	{
		H.dma_busy = 0;
		H.pending--;
		H.done_seq++;
	}
	dma_start_locked();
	if (irq)
		EIntr();
}

#define WAIT_LIMIT (294912000u * 2u) // 2 s of EE cycles: bounded waits never hang the game
#define FLIP_LIMIT (294912000u / 8u) // 125 ms: the longest wait of a new frame for the flip of the finished one
#define GIF_STAT_REG ((volatile u32 *)0x10003020)
#define WD_MAX_RECOVERIES 6

// OPT5 watchdog: what every stuck wait reports instead of freezing silently (PS2-HW-21)
static void wd_diag(const char *why)
{
	I_OutputMsg("HWD WATCHDOG: %s | VIF1 STAT=%08x D1 CHCR=%08x MADR=%08x QWC=%08x TADR=%08x V.on=%d | frame_no=%u frame_open=%d finish_pending=%d pend=%d pend_done=%d disp=%d target=%d | vbl=%u flips=%u flip_vbl=%u irq_seen=%d | dma: pending=%d busy=%d q_n=%d cur=%d wr=%u CHCR=%08x MADR=%08x QWC=%08x GIF_STAT=%08x CSR=%08x\n",
		why, (unsigned)*VIF1_STAT, (unsigned)*D1_CHCR, (unsigned)*D1_MADR, (unsigned)*D1_QWC, (unsigned)*D1_TADR, V.on, H.frame_no, H.frame_open, H.finish_pending, H.pend, H.pend_done, H.disp, H.target, (unsigned)H.vbl, (unsigned)H.flips,
		(unsigned)H.flip_vbl, H.irq_seen, H.pending, H.dma_busy, H.q_n, H.cur, H.wr, (unsigned)*D2_CHCR, (unsigned)*D2_MADR, (unsigned)*D2_QWC,
		(unsigned)*GIF_STAT_REG, (unsigned)*GS_CSR);
	fflush(stdout);
}

// A GIF DMA / GS wait ran out: stop channel 2, forget the queued buffers (they are never started: the ring is free again) and go on
// with the next frame instead of sleeping forever. The frame in progress is lost; the state the GS knew is invalid. After
// WD_MAX_RECOVERIES the engine is stopped with the diagnostics above.
static void wd_recover(const char *why)
{
	u32 irq;

	H.st.timeouts++;
	wd_diag(why);
	if (++H.st.wd_recoveries > WD_MAX_RECOVERIES)
		hw_failure("PS2 GS watchdog: the GS/DMA stays stuck after repeated resets");
	irq = DIntr();
	*D2_CHCR = 0; // stop channel 2
	if (V.on)
		*D1_CHCR = 0; // PS2-HW-44: and the VIF channel
	__asm__ volatile("sync.l");
	H.dma_busy = 0;
	H.q_n = 0;
	H.q_head = 0;
	H.pending = 0;
	H.wr = 1;
	H.run = 0;
	H.done_seq = H.q_seq; // nothing is read by DMA any more
	H.gsr.valid = 0;
	H.clut_loaded = 0;
	H.finish_pending = 0;
	if (H.pend)
		H.pend_done = 1; // show what there is
	*GS_CSR = 2;
	if (irq)
		EIntr();
	rel_release_all();
}

// Sleep until at most `limit` buffers are queued or in flight.
static void ring_wait(int limit)
{
	u32 t0, t1;

	if (H.pending <= limit)
		return;
	t0 = cyc();
	H.st.dma_waits++;
	while (H.pending > limit)
	{
		poll_dma();
		if (H.pending <= limit)
			break;
		DelayThread(30); // polled sleep: a lost interrupt can never hang the game
		if ((u32)(cyc() - t0) > WAIT_LIMIT)
		{
			wd_recover("GIF DMA ownership timeout (ring_wait)"); // channel 2 is stopped first: no in-flight buffer is handed back
			return;
		}
	}
	t1 = cyc() - t0;
	H.st.cyc_wait += t1;
}

// Queue the open buffer: its first run is closed with END (or the chain ends with the last reference), the buffer is written back.
static void pk_flush(void)
{
	u32 irq, qwc;
	qw_t *b;
	int slot;

	if (H.wr <= 1)
		return;
	b = H.ring[H.cur];
	qwc = H.wr - H.run - 1;
	if (qwc)
	{
		b[H.run].d[0] = dma_tag(qwc, DMAT_END, 0);
		if (V.on)
			b[H.run].d[1] = vif_gif_codes(qwc); // PS2-HW-44
	}
	else if (V.on)
	{
		// OPT10 (HG): the element in front of the empty run may be a CNT tag with data behind it (VU1 constants, an UNPACK chunk): turning it into a
		// REFE tag made the DMA read its data from address 0 (a stuck chain after a hundred frames). The run's own tag ends the chain instead.
		b[H.run].d[0] = dma_tag(0, DMAT_END, 0);
		b[H.run].d[1] = 0; // VIF NOP, NOP
	}
	else
	{
		// the chain ends with the reference in front of the empty run
		b[H.run - 1].d[0] = (b[H.run - 1].d[0] & ~(7ull << 28)) | (DMAT_REFE << 28);
		H.wr--;
	}
	SyncDCache(b, b + H.wr);
	irq = DIntr();
	slot = (H.q_head + H.q_n) % NBUF;
	H.q_idx[slot] = H.cur;
	H.q_qwc[slot] = H.wr;
	H.q_fid[slot] = H.fid;
	H.buf_seq[H.cur] = ++H.q_seq;
	H.q_n++;
	H.pending++;
	dma_start_locked();
	if (irq)
		EIntr();
	H.st.qwords += H.wr;
	H.cur = (H.cur + 1) % NBUF;
	H.wr = 1; // the tag of the first run
	H.run = 0;
	ring_wait(NBUF - 1); // the buffer that is filled next has to be free
	rel_release_buf(H.cur); // the engine data its previous packets referenced is not read any more
	rel_poll();
}

static qw_t *pk_alloc(u32 n)
{
	qw_t *p;

	if (n > BUF_QW - 2)
	{
		CONS_Alert(CONS_ERROR, "PS2 GS: oversized GIF packet (%u qwords)\n", n);
		abort();
	}
	if (H.wr + n > BUF_QW)
		pk_flush();
	p = H.ring[H.cur] + H.wr;
	H.wr += n;
	return p;
}

// OPT9 (PS2-HW-32): the packet continues with `qwc` qwords read by the DMAC straight from `data` (16-byte aligned, written back by the caller).
// The open run is closed with a CNT tag, the reference takes one tag, the next run starts behind it.
static void pk_ref(const void *data, u32 qwc)
{
	qw_t *b, *t;
	u32 n;

	if (V.on && qwc > VIF_DIRECT_MAX) // PS2-HW-44: one DIRECT carries at most 65535 quadwords
	{
		pk_ref(data, VIF_DIRECT_MAX);
		pk_ref((const u8 *)data + (size_t)VIF_DIRECT_MAX * 16, qwc - VIF_DIRECT_MAX);
		return;
	}
	if (H.wr + 2 > BUF_QW)
		pk_flush();
	b = H.ring[H.cur];
	n = H.wr - H.run - 1;
	if (n)
	{
		b[H.run].d[0] = dma_tag(n, DMAT_CNT, 0);
		if (V.on)
			b[H.run].d[1] = vif_gif_codes(n);
		t = b + H.wr++;
	}
	else
	{
		t = b + H.run; // an empty run: its tag becomes the reference
	}
	t->d[0] = dma_tag(qwc, DMAT_REF, phys(data));
	t->d[1] = V.on ? vif_gif_codes(qwc) : 0;
	H.run = H.wr++;
}

// n A+D register writes behind one GIF tag
static qw_t *ad_alloc(u32 n)
{
	qw_t *p = pk_alloc(n + 1);

	H.serial++;

	p[0].d[0] = HWGIF_TAG(n, 1, 0, 0, 0, 1);
	p[0].d[1] = GIF_REGS_AD;
	return p + 1;
}

// Wait until the GS executed everything that was sent (FINISH seen).
static int gs_wait_finished(void)
{
	u32 t0;
	int ok = 1;

	if (!H.finish_pending)
		return 1;
	ring_wait(0);
	t0 = cyc();
	while (!(*GS_CSR & 2))
	{
		if ((u32)(cyc() - t0) > WAIT_LIMIT)
		{
			wd_recover("GS FINISH timeout (gs_wait_finished)");
			return 0;
		}
		DelayThread(60);
	}
	H.st.cyc_wait += cyc() - t0;
	H.finish_pending = 0;
	if (H.pend)
		H.pend_done = 1; // the EE consumed FINISH (the next frame clears it): the vblank handler must not need to see it
	return ok;
}

// show the pending frame (interrupts disabled, or in the handler)
static void do_flip(void)
{
	int b = H.pend_buf;

	GS_SET_DISPFB2(H.fb_addr[b] / 8192, H.fbw / 64, H.fbpsm, 0, 0);
	H.disp = b;
	H.pend = 0;
	H.pend_done = 0;
	H.flips++;
	H.flip_vbl = H.vbl;
	dma_start_locked(); // the buffers of the next frame, held until now
}

// OPT5 (PS2-HW-21): the flip used to need FINISH to be still set at the vblank. The EE itself polls FINISH when the next frame starts
// and clears it at once, so a frame the GS took longer than the EE to draw was never flipped (the picture froze while the frame
// counter ran: heavy 3D scenes). pend_done records "completely drawn" for whoever saw FINISH first.
static int vblank_handler(int cause)
{
	(void)cause;
	H.vbl++;
	if (H.pend && (H.pend_done || (*GS_CSR & 2))) // the frame is completely drawn: show it
		do_flip();
	iSignalSema(H.sema_vbl);
	ExitHandler();
	return 0;
}

static void vblank_wait(int n)
{
	while (n-- > 0)
	{
		while (PollSema(H.sema_vbl) == H.sema_vbl)
			; // forget vblanks that passed already
		WaitSema(H.sema_vbl);
	}
}

static int detect_pal(void)
{
	char rom[16];

	memset(rom, 0, sizeof rom);
	GetRomName(rom); // "0200EC20040614": index 4 is the region letter, 'E' = Europe
	return rom[4] == 'E';
}

// constant state, written at the start of every frame
static void emit_const_env(void)
{
	static const s8 dither[16] = {-4, 2, -3, 3, 0, -2, 1, -1, -3, 3, -4, 2, 1, -1, 0, -2};
	qw_t *p = ad_alloc(10);
	u64 dimx = 0;
	int i;

	for (i = 0; i < 16; i++)
		dimx |= (u64)((u32)dither[i] & 7u) << (4 * i);
	ad_set(p++, (u64)(2048 * 16) | ((u64)(2048 * 16) << 32), GSR_XYOFFSET_1);
	ad_set(p++, 1, GSR_COLCLAMP);
	ad_set(p++, (H.fb32 || !cfg.dither) ? 0 : 1, GSR_DTHE);
	ad_set(p++, dimx, GSR_DIMX);
	ad_set(p++, 0, GSR_PABE);
	ad_set(p++, 0, GSR_FBA_1);
	ad_set(p++, 1, GSR_PRMODECONT);
	ad_set(p++, 0x80ull << 32, GSR_TEXA); // TA0 = 0, AEM = 0, TA1 = 0x80: CT16 alpha bit 1 = opaque
	ad_set(p++, 0, GSR_TEXFLUSH);
	ad_set(p++, 0, GSR_TEXFLUSH);
	H.gsr.valid = 0;
	H.clut_loaded = 0;
}

// OPT9 (PS2-HW-35): wait until the finished frame has been shown. Only when the EE is a whole frame ahead of the display (frame_end of
// the next frame, read-backs): a new frame is prepared without waiting for the flip, its buffers are held back by dma_start_locked.
static void flip_wait(void)
{
	u32 w0, vbl0;

	if (!H.pend)
		return;
	w0 = cyc();
	vbl0 = H.vbl;
	H.st.flip_waits++;
	while (H.pend)
	{
		u32 el = (u32)(cyc() - w0);

		if (el > FLIP_LIMIT && H.vbl == vbl0 && (H.pend_done || (*GS_CSR & 2))) // no vblank interrupt for 125 ms: flip by hand
		{
			u32 fi = DIntr();

			if (H.pend)
			{
				do_flip();
				H.st.flip_forced++;
			}
			if (fi)
				EIntr();
			break;
		}
		if (el > WAIT_LIMIT)
		{
			wd_recover("flip timeout (flip_wait)");
			break;
		}
		DelayThread(100);
	}
	H.st.cyc_flipwait += cyc() - w0;
}

static void frame_begin(void)
{
	u32 irq;
	int b;

	if (H.frame_open)
		return;
	if (H.gate)
	{
		// the buffer the CRTC will not read once the finished frame is shown; nothing to wait for: the GS runs this frame's
		// buffers after that flip
		irq = DIntr();
		H.target = H.last_drawn ^ 1;
		H.fid++;
		H.frame_open = 1;
		if (irq)
			EIntr();
		H.need_copy = !cfg.no_keep;
		emit_const_env();
		return;
	}
	if (H.finish_pending)
	{
		H.st.frame_waits++;
		gs_wait_finished();
	}
	// the finished frame goes to the screen at the next vblank before its buffer is drawn over (the GS may be a frame behind the EE)
	if (H.pend)
	{
		u32 w0 = cyc();

		H.st.flip_waits++;
		while (H.pend && (u32)(cyc() - w0) < FLIP_LIMIT)
			DelayThread(100);
		if (H.pend && H.pend_done) // no vblank interrupt in 125 ms: flip by hand
		{
			u32 fi = DIntr();

			if (H.pend && H.pend_done)
			{
				do_flip();
				H.st.flip_forced++;
			}
			if (fi)
				EIntr();
		}
		H.st.cyc_flipwait += cyc() - w0;
	}
	// target: the frame buffer the CRTC does not read; a frame that was drawn but never shown is replaced
	irq = DIntr();
	b = H.pend ? H.pend_buf : (H.disp ^ 1);
	if (H.pend)
		H.st.dropped++;
	*GS_CSR = 2; // clear FINISH: the flip handler waits for this frame's own
	H.target = b;
	H.pend = 0; // no old flip may consume a mid-frame Sync's FINISH
	H.frame_open = 1;
	H.fid++;
	if (irq)
		EIntr();
	H.need_copy = !cfg.no_keep;
	emit_const_env();
}

// Local to local copy of a whole frame buffer (the GS does it at about one pixel per cycle).
static void gs_copy_fb(int src, int dst)
{
	qw_t *p = ad_alloc(5);
	u64 s = (u64)(H.fb_addr[src] / 256) | ((u64)(H.fbw / 64) << 16) | ((u64)H.fbpsm << 24);
	u64 d = (u64)(H.fb_addr[dst] / 256) | ((u64)(H.fbw / 64) << 16) | ((u64)H.fbpsm << 24);

	ad_set(p++, s | (d << 32), GSR_BITBLTBUF);
	ad_set(p++, 0, GSR_TRXPOS);
	ad_set(p++, (u64)H.vw | ((u64)H.vh << 32), GSR_TRXREG);
	ad_set(p++, 2, GSR_TRXDIR); // local -> local
	ad_set(p++, 0, GSR_TEXFLUSH);
}

// Every operation that touches the frame buffer calls this first. `clearing` = the operation overwrites every colour
// pixel anyway, so the copy of the previous frame is not needed.
static void frame_prepare(int clearing)
{
	if (!H.frame_open)
		frame_begin();
	if (H.need_copy)
	{
		H.need_copy = 0;
		if (!clearing && H.have_drawn && H.last_drawn != H.target)
			gs_copy_fb(H.last_drawn, H.target);
	}
}

static void frame_end(void)
{
	qw_t *p;
	u32 irq;

	ov_flush_all();
	frame_prepare(0);
	if (H.gate && H.pend)
		flip_wait(); // the EE is a whole frame ahead of the display: the previous frame must be shown before this one can be marked finished
	p = ad_alloc(1);
	ad_set(p, 0, GSR_FINISH);
	irq = DIntr();
	*GS_CSR = 2; // mid-frame readback may have set FINISH; only this frame-end counts
	H.pend_done = 0;
	H.pend = 1;
	H.pend_fid = H.fid;
	H.pend_buf = H.target;
	H.finish_pending = 1;
	if (irq)
		EIntr();
	H.last_drawn = H.target;
	H.have_drawn = 1;
	H.frame_open = 0;
	tex_cap_control();
	if (ps2hwd_dbg_flags & HWDBG_TEX)
		lod_frame_end();
	H.frame_no++;
	if ((ps2hwd_dbg_flags & HWDBG_TEX) && ++TX.frames >= 105)
		tx_report();
	pk_flush();
}

// Fill the whole frame buffer / Z buffer with a sprite (Z = 0 = far, always passes).
// colour: write RGBA = rgba (GS scale, alpha 0x80 = 1.0) when set; depth: clear Z when set.
static void gs_fill_all(int buf, int colour, int depth, u32 rgba)
{
	qw_t *p = ad_alloc(8);
	qw_t *s;

	ad_set(p++, frame_reg(buf, colour ? 0 : 0xFFFFFFFFu), GSR_FRAME_1);
	ad_set(p++, zbuf_reg(!depth), GSR_ZBUF_1);
	ad_set(p++, scissor_reg(0, 0, H.vw - 1, H.vh - 1), GSR_SCISSOR_1);
	ad_set(p++, (1ull << 16) | (1ull << 17), GSR_TEST_1); // ZTE, ZTST ALWAYS
	ad_set(p++, 0, GSR_ALPHA_1);
	ad_set(p++, 0, GSR_TEX1_1);
	ad_set(p++, 0, GSR_CLAMP_1);
	ad_set(p++, 0, GSR_TEXFLUSH);
	s = pk_alloc(1 + 4);
	s[0].d[0] = HWGIF_TAG(2, 1, 1, PRIM_SPRITE, 0, 2);
	s[0].d[1] = GIF_REGS_RGBA_XYZ;
	s[1].d[0] = (u64)(rgba & 0xFF) | ((u64)((rgba >> 8) & 0xFF) << 32);
	s[1].d[1] = (u64)((rgba >> 16) & 0xFF) | ((u64)(rgba >> 24) << 32);
	s[2].d[0] = (u64)(2048 * 16) | ((u64)(2048 * 16) << 32);
	s[2].d[1] = 0; // Z = 0
	s[3] = s[1];
	s[4].d[0] = (u64)((2048 + H.vw) * 16) | ((u64)((2048 + H.vh) * 16) << 32);
	s[4].d[1] = 0;
	H.gsr.valid = 0;
}

// ---- bring-up ----

// Output formats of docs/VIDEO_MODES.md table B that the GS renderer can use. The frame buffer is the internal picture
// (the engine's vid.width x vid.height rounded up to a size the CRTC can magnify by a whole number); the CRTC magnifies
// it to the screen, so the texture area is not eaten by a full-resolution buffer.
static void hw_choose_size(int mode, int need_w, int need_h, int *w, int *h, int *field_frame)
{
	static const int ws[] = {256, 320, 512, 640};
	int nh = mode == PS2HWD_VIDEO_PAL ? 512 : mode == PS2HWD_VIDEO_480P ? 480 : 448, k, i;

	*w = 640;
	for (i = 0; i < 4; i++)
		if (ws[i] >= need_w)
		{
			*w = ws[i];
			break;
		}
	*h = nh;
	for (k = 4; k >= 1; k--)
		if (nh % k == 0 && nh / k >= need_h)
		{
			*h = nh / k;
			break;
		}
	// interlaced formats show the field lines of a half-height buffer once per field (FRAME read mode)
	*field_frame = mode != PS2HWD_VIDEO_480P && *h <= nh / 2;
}

static u32 fb_pages(int stride, int rows, int psm32)
{
	return (u32)(stride / 64) * (u32)((rows + (psm32 ? 31 : 63)) / (psm32 ? 32 : 64));
}

static int gs_setup(void)
{
	GSGLOBAL *gs;
	u32 pool_start, fb_bytes, z_bytes, clut_bytes;
	int i, field_frame, w, h;
	ee_sema_t sema;

	switch (cfg.video)
	{
		case PS2HWD_VIDEO_PAL:
		case PS2HWD_VIDEO_480P:
		case PS2HWD_VIDEO_NTSC:
			H.mode = cfg.video;
			break;
		default:
			H.mode = detect_pal() ? PS2HWD_VIDEO_PAL : PS2HWD_VIDEO_NTSC;
			break;
	}
	if (cfg.screen_w <= 0 || cfg.screen_h <= 0)
		return -2;
	hw_choose_size(H.mode, cfg.screen_w, cfg.screen_h, &w, &h, &field_frame);
	if (cfg.fbw)
		w = cfg.fbw;
	if (cfg.fbh)
	{
		h = cfg.fbh;
		field_frame = H.mode != PS2HWD_VIDEO_480P && h <= (H.mode == PS2HWD_VIDEO_PAL ? 256 : 224);
	}
	if (w < 64 || w > 1024 || h < 16 || h > 1024)
		return -2;
	H.vw = w;
	H.vh = h;
	H.fbw = (w + 63) & ~63;
	H.fbh = h;
	H.fb32 = cfg.fb32 ? 1 : 0;
	// Never change the requested colour precision to satisfy a budget. Reject impossible layouts before CRTC setup.
	if (2 * fb_pages(H.fbw, H.fbh, H.fb32) + fb_pages(H.fbw, H.fbh, 1)
		+ (CLUT_SLOTS * CLUT_BLOCKS + PAGE_BLOCKS - 1) / PAGE_BLOCKS >= VRAM_BLOCKS / PAGE_BLOCKS)
		return -2;
	H.fbpsm = H.fb32 ? PSM_CT32 : PSM_CT16S;
	H.outid = H.mode == PS2HWD_VIDEO_480P ? 2 : (H.mode == PS2HWD_VIDEO_PAL ? (field_frame ? 4 : 1) : (field_frame ? 3 : 0));

	V.on = (ps2hwd_dbg_flags & HWDBG_VU1) != 0; // PS2-HW-44: -hwdbg 0x4000000: the stream goes through VIF1, the VU1 program draws the polygons
	// OPT10 (HG): dmaKit does not take the VIF1 channel (with it in the mask gsKit_init_screen hangs in PCSX2): the channel is reset by hand below
	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);
	if (V.on)
	{
		*D1_CHCR = 0;
		*D1_QWC = 0;
		__asm__ volatile("sync.l");
	}

	CONS_Printf("HWD init: gsKit_init_global\n");
	gs = gsKit_init_global_custom(8 * 1024, 8 * 1024); // gsKit's own draw queues are never used
	if (!gs)
		return -1;
	H.gs = gs;
	switch (H.mode)
	{
		case PS2HWD_VIDEO_PAL:
			gs->Mode = GS_MODE_PAL;
			gs->Interlace = GS_INTERLACED;
			gs->Field = field_frame ? GS_FRAME : GS_FIELD;
			break;
		case PS2HWD_VIDEO_480P:
			gs->Mode = GS_MODE_DTV_480P;
			gs->Interlace = GS_NONINTERLACED;
			gs->Field = GS_FRAME;
			break;
		default:
			gs->Mode = GS_MODE_NTSC;
			gs->Interlace = GS_INTERLACED;
			gs->Field = field_frame ? GS_FRAME : GS_FIELD;
			break;
	}
	gs->Width = H.vw;
	gs->Height = H.vh;
	gs->PSM = H.fb32 ? GS_PSM_CT32 : GS_PSM_CT16S;
	gs->PSMZ = GS_PSMZ_24;
	gs->DoubleBuffering = GS_SETTING_ON;
	gs->ZBuffering = GS_SETTING_ON;
	gs->Dithering = GS_SETTING_OFF;
	CONS_Printf("HWD init: gsKit_init_screen\n");
	gsKit_init_screen(gs); // video mode, CRTC and magnification; gsKit's own VRAM addresses are not used (it rounds buffer sizes by bytes, not pages)
	{
		// gsKit magnifies to the widest area its table allows (NTSC: 2880 clocks); the picture is shown over the same area as
		// a 640 pixel frame buffer would be (2560 clocks, 1280 for 480p), centred
		int target = H.mode == PS2HWD_VIDEO_480P ? 1280 : 2560, mh = target / H.vw, ndw;

		if (mh < 1)
			mh = 1;
		if (mh > 16)
			mh = 16;
		ndw = H.vw * mh;
		H.crtc_dx = gs->StartX;
		H.crtc_dw = gs->DW;
		gs->StartX += (gs->DW - ndw) / 2;
		gs->MagH = mh - 1;
		gs->DW = ndw;
	}
	CONS_Printf("HWD init: mode_switch\n");
	gsKit_mode_switch(gs, GS_ONESHOT);
	CONS_Printf("HWD init: mode switched\n");
	H.magh = gs->MagH + 1;
	H.magv = gs->MagV + 1;

	// VRAM layout, in whole GS pages: frame buffer 0, 1, Z (32-bit layout), CLUT area, texture pool
	fb_bytes = fb_pages(H.fbw, H.fbh, H.fb32) * 8192u;
	z_bytes = fb_pages(H.fbw, H.fbh, 1) * 8192u;
	clut_bytes = ((CLUT_SLOTS * CLUT_BLOCKS + PAGE_BLOCKS - 1) / PAGE_BLOCKS) * 8192u;
	H.fb_addr[0] = 0;
	H.fb_addr[1] = fb_bytes;
	H.z_addr = 2 * fb_bytes;
	pool_start = H.z_addr + z_bytes;
	if (pool_start + clut_bytes + 8192u > 0x400000u)
		return -2;
	H.clut_base = pool_start / 256;
	H.pool_base = (pool_start + clut_bytes) / 256;
	H.pool_blocks = VRAM_BLOCKS - H.pool_base;
	H.free_n = 1;
	H.free_start[0] = H.pool_base;
	H.free_len[0] = H.pool_blocks;
	H.used_blocks = 0;

	for (i = 0; i < NBUF; i++)
	{
		H.ring[i] = memalign(64, BUF_QW * sizeof(qw_t));
		if (!H.ring[i])
			return -3;
	}
	H.cur = 0;
	H.wr = 1; // the tag of the first run
	H.run = 0;

	sema.init_count = 0;
	sema.max_count = 1;
	sema.option = 0;
	H.sema_vbl = CreateSema(&sema);
	sema.max_count = 32;
	H.sema_dma = CreateSema(&sema);
	if (H.sema_vbl < 0 || H.sema_dma < 0)
		return -4;

	CONS_Printf("HWD init: rings ready\n");
	*GS_IMR = 0xFF00; // no GS interrupts, only vblank is used
	H.dmac = AddDmacHandler(DMAC_GIF, dmac_gif_handler, 0);
	if (H.dmac >= 0)
		EnableDmac(DMAC_GIF);
	if (V.on)
	{
		CONS_Printf("HWD VU1: adding the VIF1 handler\n");
		V.dmac = AddDmacHandler(DMAC_VIF1, dmac_gif_handler, 0);
		if (V.dmac >= 0)
		{
			EnableDmac(DMAC_VIF1);
			V.dmac_set = 1;
		}
		CONS_Printf("HWD VU1: handler %d, booting the microcode\n", V.dmac);
		if (V.dmac < 0 || !vu1_boot())
		{
			CONS_Printf("HWD VU1: VIF1 interrupt or microcode upload failed, the GIF ring is used\n");
			V.on = 0;
		}
	}
	return 0;
}

// is the GIF DMA completion interrupt delivered? (a tiny packet is sent and the handler count is watched)
static void gs_irq_selftest(void)
{
	qw_t *p;
	u32 t0;
	int seen = H.irq_seen;

	H.dma_irq = H.dmac >= 0;
	p = ad_alloc(1);
	ad_set(p, 0, GSR_TEXFLUSH);
	pk_flush();
	t0 = cyc();
	while (H.pending > 0 && (u32)(cyc() - t0) < 294912000u / 10u)
	{
		poll_dma();
		if (H.irq_seen != seen)
			break;
	}
	t0 = cyc();
	while (H.irq_seen == seen && (u32)(cyc() - t0) < 294912000u / 50u)
		;
	H.dma_irq = (H.irq_seen != seen);
	ring_wait(0);
}

static void gs_clear_both(void)
{
	qw_t *p;

	H.target = 0;
	emit_const_env();
	gs_fill_all(1, 1, 1, 0x80000000u);
	gs_fill_all(0, 1, 1, 0x80000000u);
	p = ad_alloc(1);
	ad_set(p, 0, GSR_FINISH);
	*GS_CSR = 2;
	H.finish_pending = 1;
	pk_flush();
	gs_wait_finished();
}
