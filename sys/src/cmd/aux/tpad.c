/*
 * PS/2 Synaptics touchpad driver, in userspace.
 *
 * The kernel already knows how to put a Synaptics touchpad into absolute
 * mode (write "synaptic" to /dev/mousectl) and frames its 6-byte packets,
 * but it only tracks a single finger and throws the multi-finger reports
 * away.  Instead we read the framed raw packets from #m/touchpadraw (which
 * diverts them from the kernel's own decode), decode them here, and run the
 * shared gesture engine (libtouch), which emits mouse(3) events on
 * /dev/mousein -- cursor motion, two-finger scroll, taps and the menu-button
 * gestures.
 *
 * Basic PS/2 Synaptics reports one absolute position plus a width code w
 * that also encodes the finger count (w==0 two fingers, w==1 three), but no
 * per-finger ids or second-finger position.  We synthesise contacts at the
 * one reported position with distinct ids so the gesture engine can still
 * tell a two- from a three-finger tap.  A trackpoint muxed through the pad's
 * pass-through port arrives as w==3 packets, which we forward as relative
 * motion.
 */
#include <u.h>
#include <libc.h>
#include <touch.h>

enum {
	Pktlen	= 6,

	/* nominal Synaptics absolute coordinate range (pad edges) */
	Xmin	= 1472,
	Xmax	= 5472,
	Ymin	= 1408,
	Ymax	= 4448,

	Zmin	= 40,		/* pressure below this: no finger */
	Wpalm	= 6,		/* width at/above this: palm, ignore */
};

/* trackpoint L/R/M button bits -> mouse(3) button mask */
static uchar tpbtn[8] = { 0, 1, 4, 5, 2, 3, 6, 7 };

static int mousefd;
static int debug;

static int
norm(int v, int lo, int hi)
{
	vlong n;

	if(hi <= lo)
		return 0;
	n = ((vlong)(v - lo) << 31) / (hi - lo);
	if(n < 0)
		n = 0;
	if(n > 0x7fffffff)
		n = 0x7fffffff;
	return (int)n;
}

static void
trackpoint(uchar *b)
{
	int dx, dy, buttons;
	char buf[64];

	buttons = tpbtn[b[1] & 7];
	dx = b[4];
	if(b[1] & 0x10)
		dx |= ~0xff;		/* sign-extend */
	dy = b[5];
	if(b[1] & 0x20)
		dy |= ~0xff;
	dy = -dy;
	seprint(buf, buf+sizeof buf, "m%11d %11d %11d", dx, dy, buttons);
	write(mousefd, buf, strlen(buf));
}

void
usage(void)
{
	fprint(2, "usage: %s [-d]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	int fd, ctl, i, n, w, z, x, y;
	uchar b[Pktlen];
	Touch t;
	Gesture g;

	ARGBEGIN{
	case 'd':
		debug++;
		break;
	default:
		usage();
	}ARGEND;

	/*
	 * Talk to the kernel mouse device directly via #m, not /dev, which a
	 * window system (rio) may have mounted over: synaptic mode and the raw
	 * packets are kernel-only, and the events must reach the kernel mouse
	 * so every reader (including rio) sees them.
	 */
	ctl = open("#m/mousectl", OWRITE);
	if(ctl < 0)
		sysfatal("open #m/mousectl: %r");
	if(write(ctl, "synaptic", 8) < 0)
		sysfatal("enable synaptic mode: %r");

	fd = open("#m/touchpadraw", OREAD);
	if(fd < 0)
		sysfatal("open #m/touchpadraw: %r");

	mousefd = open("#m/mousein", OWRITE);
	if(mousefd < 0)
		sysfatal("open #m/mousein: %r");

	memset(&g, 0, sizeof g);
	for(;;){
		n = read(fd, b, sizeof b);
		if(n < 0)
			sysfatal("read touchpadraw: %r");
		if(n < Pktlen)
			continue;

		w = (b[3]&0x4)>>2 | (b[0]&0x4)>>1 | (b[0]&0x30)>>2;
		if(w == 3){			/* trackpoint pass-through */
			trackpoint(b);
			continue;
		}

		z = b[2];
		x = b[4] | (b[1]&0xf)<<8;
		if(b[3] & 0x10)
			x |= 1<<12;
		y = b[5] | (b[1]&0xf0)<<4;
		if(b[3] & 0x20)
			y |= 1<<12;

		memset(&t, 0, sizeof t);
		/*
		 * clickpad: the whole pad is one button, reported as the xor of
		 * byte 0 and byte 3 bit 0 (not byte 0's L/R bits, which a
		 * clickpad uses for other data).
		 */
		t.button = ((b[0] ^ b[3]) & 1) ? 1 : 0;
		if(z >= Zmin && w < Wpalm){
			/* finger count from w: 0 => 2 fingers, 1 => 3, else 1 */
			t.n = w==0 ? 2 : (w==1 ? 3 : 1);
			if(t.n > Maxcontacts)
				t.n = Maxcontacts;
			/*
			 * one position, no per-finger ids: place n contacts at
			 * the reported point with distinct ids so the engine
			 * can distinguish 2- from 3-finger gestures.  y is
			 * flipped so finger-up moves the cursor up.
			 */
			for(i = 0; i < t.n; i++){
				t.id[i] = i;
				t.x[i] = norm(x, Xmin, Xmax);
				t.y[i] = 0x7fffffff - norm(y, Ymin, Ymax);
			}
		}
		if(debug)
			fprint(2, "[%.2ux %.2ux %.2ux %.2ux %.2ux %.2ux] "
				"w=%d z=%d x=%d y=%d n=%d b=%d\n",
				b[0], b[1], b[2], b[3], b[4], b[5],
				w, z, x, y, t.n, t.button);
		gesture(&g, &t, mousefd);
	}
}
