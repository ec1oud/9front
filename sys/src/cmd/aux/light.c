#include <u.h>
#include <libc.h>

/*
 * Adjust the LCD backlight on Intel integrated graphics by poking
 * the panel backlight PWM duty-cycle register.  The kernel igfx
 * driver (pc/vgaigfx.c) vmaps the MMIO BAR and publishes it as the
 * "igfxmmio" physical segment, so we can reach the registers from
 * userspace without any new kernel code.
 *
 * The PWM register holds the period in bits 31:16 and the duty cycle
 * in bits 15:0; brightness = duty/period.  Which register drives the
 * panel varies: PCH side on Ironlake..Broadwell, CPU side on some
 * Haswell eDP panels (e.g. the ThinkPad T440p), and a legacy location
 * pre-Ironlake.  We probe the candidates and pick the one that is
 * actually running (nonzero period and, since the screen is lit,
 * nonzero duty); -r overrides the choice.
 */

enum {
	CPUCTL2	= 0x48254,	/* CPU-side backlight duty/period */
	PCHCTL2	= 0xC8254,	/* PCH-side backlight duty/period */
	OLDCTL2	= 0x61254,	/* pre-Ironlake backlight duty/period */
	Minpct	= 1,		/* never let the screen go fully dark */
};

static ulong candidates[] = { CPUCTL2, PCHCTL2, OLDCTL2 };

static u32int *mmio;
static uvlong mmiosize;
static ulong reg;
static int rset;

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
	char path[64], buf[512], *base, *p, *s;
	int fd, i, ccrb, vid;
	uvlong size;
	Dir *d;
	long n, r;

	if((fd = open(base = "/dev/pci", OREAD)) < 0)
		if((fd = open(base = "#$/pci", OREAD)) < 0)
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
		snprint(path, sizeof path, "%s/%s", base, d[i].name);
		if((fd = open(path, OREAD)) < 0)
			continue;
		r = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(r <= 0)
			continue;
		buf[r] = 0;

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

/*
 * Pick the PWM register that is actually driving the panel: since the
 * screen is lit its duty cycle is nonzero.  Fall back to any register
 * with a nonzero period so a fully-dimmed panel can still be raised.
 */
static void
pickreg(void)
{
	int i;
	u32int v;
	ulong c;

	for(i = 0; i < nelem(candidates); i++){
		c = candidates[i];
		if(c + 4 > mmiosize)
			continue;
		v = mmio[c/4];
		if((v >> 16) != 0 && (v & 0xFFFF) != 0){
			reg = c;
			return;
		}
	}
	for(i = 0; i < nelem(candidates); i++){
		c = candidates[i];
		if(c + 4 > mmiosize)
			continue;
		if((mmio[c/4] >> 16) != 0){
			reg = c;
			return;
		}
	}
	sysfatal("no active backlight PWM register found; try -r");
}

void
main(int argc, char **argv)
{
	u32int v;
	ulong period, duty, pct, want;

	ARGBEGIN{
	case 'r':
		reg = strtoul(EARGF(usage()), nil, 0);
		rset = 1;
		break;
	default:
		usage();
	}ARGEND;

	if(argc > 1)
		usage();

	mmiosize = findigfx();
	if(rset && reg + 4 > mmiosize)
		sysfatal("register %#lux outside mmio (%#llux)", reg, mmiosize);
	mmio = segattach(0, "igfxmmio", 0, mmiosize);
	if(mmio == (u32int*)-1)
		sysfatal("segattach igfxmmio: %r");

	if(!rset)
		pickreg();

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
