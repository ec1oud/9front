/*
 * screenqemu.c -- QEMU virt ramfb framebuffer.
 *
 * QEMU's ramfb device scans out a buffer in guest RAM.  The guest
 * points it at the buffer by writing a RAMFBCfg to the fw_cfg file
 * "etc/ramfb".  Modern QEMU no longer accepts writes on the fw_cfg
 * data port, so the descriptor goes through the DMA interface; the
 * directory read still uses the byte-wide data port.
 *
 * The buffer is ordinary RAM.  ramfb reads it directly, so the CPU
 * copy in flushmemscreen() needs no cache maintenance.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

enum {
	FwcfgPa		= 0x10100000,
	FwcfgSize	= 0x1000,

	FwcfgData	= 0x00,
	FwcfgSelect	= 0x08,
	FwcfgDma	= 0x10,

	FwcfgFileDir	= 0x19,
	FwcfgEntrySize	= 64,
	FwcfgNameSize	= 56,

	FwcfgDmaSelect	= 1<<3,
	FwcfgDmaWrite	= 1<<4,

	RamfbSize	= 28,

	FbWidth		= 1920,
	FbHeight	= 1080,
	FbDepth		= 32,
	FbSize		= FbWidth*FbHeight*FbDepth/8,

	/* DRM_FORMAT_XRGB8888, the memory layout of Plan 9 XRGB32 */
	FbFourcc	= 'X' | 'R'<<8 | '2'<<16 | '4'<<24,
};

static u64int dmabuf[2];
static u64int cfgbuf[4];

static u16int
be16(u16int v)
{
	return (v>>8) | (v<<8);
}

static u32int
be32(u32int v)
{
	return (v>>24) | ((v>>8) & 0xff00) | ((v<<8) & 0xff0000) | (v<<24);
}

static u64int
be64(u64int v)
{
	return (u64int)be32(v)<<32 | be32(v>>32);
}

static void
putbe32(uchar *p, u32int v)
{
	p[0] = v>>24;
	p[1] = v>>16;
	p[2] = v>>8;
	p[3] = v;
}

static void
putbe64(uchar *p, u64int v)
{
	putbe32(p, v>>32);
	putbe32(p+4, v);
}

static u16int
getbe16(uchar *p)
{
	return (u16int)p[0]<<8 | p[1];
}

static u32int
getbe32(uchar *p)
{
	return (u32int)p[0]<<24 | (u32int)p[1]<<16 | (u32int)p[2]<<8 | p[3];
}

static void
fwselect(uchar *fw, int sel)
{
	*(volatile u16int*)(fw+FwcfgSelect) = be16(sel);
	coherence();
}

static uchar
fwread8(uchar *fw)
{
	return *(volatile u8int*)(fw+FwcfgData);
}

static int
fwcfgfile(uchar *fw, char *name)
{
	uchar ent[FwcfgEntrySize];
	u32int count;
	int i, j, sel;

	fwselect(fw, FwcfgFileDir);
	for(j = 0; j < 4; j++)
		ent[j] = fwread8(fw);
	count = getbe32(ent);
	if(count > 256)
		return -1;

	for(i = 0; i < count; i++){
		for(j = 0; j < FwcfgEntrySize; j++)
			ent[j] = fwread8(fw);
		if(strncmp((char*)ent+8, name, FwcfgNameSize) == 0){
			sel = getbe16(ent+4);
			return sel;
		}
	}
	return -1;
}

void*
qemufbinit(int *width, int *height, int *stride, uvlong *pa)
{
	uchar *fw, *cfg, *dma;
	void *fb;
	int sel;

	if(!dtbsocmatch("riscv-virtio"))
		return nil;

	fw = vmap(FwcfgPa, FwcfgSize);
	if(fw == nil)
		return nil;

	sel = fwcfgfile(fw, "etc/ramfb");
	if(sel < 0)
		return nil;

	fb = xspanalloc(FbSize, BY2PG, 0);
	memset(fb, 0, FbSize);

	dma = (uchar*)dmabuf;
	cfg = (uchar*)cfgbuf;
	memset(cfg, 0, sizeof cfgbuf);
	putbe64(cfg+0, PADDR(fb));
	putbe32(cfg+8, FbFourcc);
	putbe32(cfg+12, 0);
	putbe32(cfg+16, FbWidth);
	putbe32(cfg+20, FbHeight);
	putbe32(cfg+24, FbWidth*FbDepth/8);

	memset(dma, 0, sizeof dmabuf);
	putbe32(dma+0, (u32int)sel<<16 | FwcfgDmaSelect | FwcfgDmaWrite);
	putbe32(dma+4, RamfbSize);
	putbe64(dma+8, PADDR(cfg));

	cachedwbinvse(dma, sizeof dmabuf);
	cachedwbinvse(cfg, sizeof cfgbuf);
	coherence();
	*(volatile u64int*)(fw+FwcfgDma) = be64(PADDR(dma));
	coherence();
	cachedinvse(dma, sizeof dmabuf);
	if(getbe32(dma) != 0){
		print("qemufb: ramfb rejected the framebuffer descriptor\n");
		return nil;
	}

	*width = FbWidth;
	*height = FbHeight;
	*stride = FbWidth*FbDepth/8;
	*pa = PADDR(fb);
	return fb;
}
