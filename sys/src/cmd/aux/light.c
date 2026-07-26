#include <u.h>
#include <libc.h>

/*
 * Adjust the LCD backlight on Intel integrated graphics by poking
 * the panel backlight PWM duty-cycle register.  The kernel igfx
 * driver (pc/vgaigfx.c) vmaps the MMIO BAR and publishes it as the
 * "igfxmmio" physical segment, so we can reach the registers from
 * userspace without any new kernel code.
 *
 * Ironlake through Broadwell (the T440p is Haswell) drive the panel
 * PWM from the PCH:
 *	BLC_PWM_PCH_CTL1 0xC8250  bit31 = PWM enable
 *	BLC_PWM_PCH_CTL2 0xC8254  bits 31:16 = period, 15:0 = duty
 * brightness = duty/period.  Other generations put the PWM elsewhere
 * (CPU side 0x48254, pre-gen5 0x61254); use -r to point at those.
 */

enum {
	PCHCTL2	= 0xC8254,	/* default: PCH backlight duty/period */
	Minpct	= 1,		/* never let the screen go fully dark */
};

static u32int *mmio;
static uvlong mmiosize;
static ulong reg = PCHCTL2;

static void
usage(void)
{
	fprint(2, "usage: %s [-r reg] [[+|-]percent]\n", argv0);
	exits("usage");
}

/*
 * Find the first Intel (vid 0x8086) display controller (class 0x03)
 * and return the size of its first memory BAR, which is what the
 * kernel used to register the igfxmmio segment.
 */
static uvlong
findigfx(void)
{
	char path[64], buf[512], *p, *s;
	int fd, i, ccrb, vid;
	uvlong size;
	Dir *d;
	long n;

	if((fd = open("/dev/pci", OREAD)) < 0)
		if((fd = open("#$/pci", OREAD)) < 0)
			sysfatal("open /dev/pci: %r");
	n = dirreadall(fd, &d);
	close(fd);
	if(n < 0)
		sysfatal("dirreadall /dev/pci: %r");

	size = 0;
	for(i = 0; i < n && size == 0; i++){
		p = d[i].name + strlen(d[i].name) - 3;
		if(p < d[i].name || strcmp(p, "ctl") != 0)
			continue;
		snprint(path, sizeof path, "/dev/pci/%s", d[i].name);
		if((fd = open(path, OREAD)) < 0)
			continue;
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n <= 0)
			continue;
		buf[n] = 0;

		/* "ccrb.ccru.ccrp vid/did intl [i:bar size]..." */
		ccrb = strtol(buf, nil, 16);
		if(ccrb != 0x03)
			continue;
		if((p = strchr(buf, ' ')) == nil)
			continue;
		vid = strtol(p + 1, nil, 16);
		if(vid != 0x8086)
			continue;
		if((s = strstr(buf, " 0:")) == nil)
			sysfatal("igfx has no mem[0] bar");
		strtoull(s + 3, &s, 16);	/* bar address, unused */
		size = strtoll(s + 1, nil, 10);
	}
	free(d);
	if(size == 0)
		sysfatal("no Intel display controller found");
	return size;
}

static u32int
rd(void)
{
	return mmio[reg/4];
}

static void
wr(u32int v)
{
	mmio[reg/4] = v;
}

void
main(int argc, char **argv)
{
	u32int v;
	ulong period, duty, pct, want;

	ARGBEGIN{
	case 'r':
		reg = strtoul(EARGF(usage()), nil, 0);
		break;
	default:
		usage();
	}ARGEND;

	if(argc > 1)
		usage();

	mmiosize = findigfx();
	if(reg + 4 > mmiosize)
		sysfatal("register %#lux outside mmio (%#llux)", reg, mmiosize);
	mmio = segattach(0, "igfxmmio", 0, mmiosize);
	if(mmio == (u32int*)-1)
		sysfatal("segattach igfxmmio: %r");

	v = rd();
	period = v >> 16;
	duty = v & 0xFFFF;
	if(period == 0)
		sysfatal("register %#lux has zero period; wrong register?", reg);
	pct = (duty * 100 + period/2) / period;

	if(argc == 0){
		print("%lud\n", pct);
		exits(nil);
	}

	/* set: absolute "N", or relative "+N" / "-N" */
	if(argv[0][0] == '+' || argv[0][0] == '-'){
		long delta = atol(argv[0]);
		want = (long)pct + delta < 0 ? 0 : pct + delta;
	} else
		want = strtoul(argv[0], nil, 10);

	if(want < Minpct)
		want = Minpct;
	if(want > 100)
		want = 100;

	duty = (want * period + 50) / 100;
	if(duty < 1)
		duty = 1;
	if(duty > period)
		duty = period;
	wr((period << 16) | duty);

	exits(nil);
}
