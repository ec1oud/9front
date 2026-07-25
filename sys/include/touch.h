#pragma	src	"/sys/src/libtouch"
#pragma	lib	"libtouch.a"

/*
 * Touchpad gesture engine, shared by the USB HID front-end (nusb/kb) and
 * the PS/2 Synaptics front-end (aux/tpad).
 *
 * A Touch is one frame of touchpad state: the currently-touching contacts
 * (coordinates normalised 0..2^31 across the pad, treated as unsigned) and
 * the physical clickpad button.  gesture() turns a stream of frames into
 * mouse(3) events written to fd (typically /dev/mousein): one finger drives
 * the cursor relatively, two fingers scroll (wheel), a quick tap or physical
 * click is button 1, and holding one finger while tapping others arms the
 * middle/right menu buttons and holds them until the finger lifts.  All state
 * lives in Gesture, so a caller keeps one Gesture per device and zeroes it
 * before the first frame.
 */
enum {
	Maxcontacts	= 8,
};

typedef struct Touch Touch;
typedef struct Gesture Gesture;

struct Touch
{
	int	n;			/* number of contacts touching */
	int	id[Maxcontacts];
	int	x[Maxcontacts];
	int	y[Maxcontacts];
	int	button;			/* physical clickpad button down */
};

struct Gesture
{
	int	mode;			/* internal state-machine phase */
	int	primid;			/* contact id of the primary finger */
	int	px, py;			/* previous primary-contact position */
	vlong	sacc;			/* two-finger scroll accumulator */
	int	lastb;			/* last button state emitted */

	/* single-finger tap detection */
	vlong	downtime;		/* when the finger first touched */
	int	downx, downy;		/* position at touch-down */
	int	moved;			/* moved too far (or pressed) to be a tap */

	/* scroll-vs-menu arbitration */
	vlong	pendtime;		/* when the extra finger(s) arrived */
	int	pendx, pendy;		/* primary position when they arrived */
	int	nextra;			/* distinct non-primary fingers seen */
	int	extraid[Maxcontacts];	/* their contact ids */
	int	menub;			/* held menu button mask (2 or 4), or 0 */

	int	hwb;			/* previous physical-button state */
	int	suppress;		/* motion frames left to drop after a press */
};

/*
 * Tunables.  A front-end may expose these for runtime adjustment (nusb/kb
 * does, via its ctl file).  Travel values are fractions of the 2^31 pad
 * span; time values are ns.
 */
extern int Tpgainx, Tpgainy, Scrollthresh, Taptime, Taptravel;
extern int Suppressframes, Scrollstart, Menuwindow;

void	gesture(Gesture *g, Touch *t, int fd);
