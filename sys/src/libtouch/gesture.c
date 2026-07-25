#include <u.h>
#include <libc.h>
#include <touch.h>

/*
 * Touchpad gesture tunables.  Travel thresholds are fractions of the 2^31
 * pad span; time values are ns.
 *
 * When a second finger lands while one finger is already down we don't yet
 * know whether the user is starting a two-finger scroll or tapping extra
 * fingers to arm a menu button.  Suppress both until we decide: if the
 * primary finger travels past Scrollstart the fingers are dragging, so
 * commit to scrolling; if instead the extra fingers lift again (within
 * Menuwindow) while the primary stayed put it was a tap, so arm a menu.
 * Deciding on motion rather than time lets the window be generous (a
 * one-handed two-finger tap is slow) without making scrolling feel laggy,
 * and tolerates the holding finger drifting a little.  Menuwindow still
 * bounds a motionless rest so two fingers left sitting on the pad
 * eventually count as a scroll.
 */
int Tpgainx	= 1600;		/* full-width finger travel -> ~px */
int Tpgainy	= 900;		/* full-height (pad is wider than tall) */
int Scrollthresh = 178956971;	/* ~2^31/12: pad height -> ~12 notches */
int Taptime	= 180000000;	/* ns: max touch duration for a tap */
int Taptravel	= 100000000;	/* ~5% of pad: max movement for a tap */
int Suppressframes = 3;		/* motion frames to drop after a press */
int Scrollstart	= 120000000;	/* ~5.6% of pad: motion => it's a scroll */
int Menuwindow	= 250000000;	/* ns: max still hold before it's a scroll */

/* gesture engine state machine */
enum {
	Gidle	= 0,			/* no fingers down */
	Gcursor,			/* one finger: relative cursor motion */
	Gpending,			/* 2+ fingers: deciding scroll vs menu */
	Gscroll,			/* committed to two-finger scroll */
	Gmenu,				/* a menu button held, primary dragging */
};

static int
findprim(Touch *t)
{
	int i, prim;

	prim = 0;
	for(i = 1; i < t->n; i++)
		if(t->id[i] < t->id[prim])
			prim = i;
	return prim;
}

static int
findid(Touch *t, int id)
{
	int i;

	for(i = 0; i < t->n; i++)
		if(t->id[i] == id)
			return i;
	return -1;
}

static void
emitmouse(int fd, int x, int y, int b)
{
	char buf[80];

	seprint(buf, buf+sizeof(buf), "m%11d %11d %11d", x, y, b);
	write(fd, buf, strlen(buf));
}

/*
 * Mouse-button gestures need a persistent primary finger (rio menus stay
 * open only while a button is held):
 *
 *	tap a 2nd finger, primary stays	arm button 3 (right menu)
 *	tap two extra fingers, primary stays	arm button 2 (middle)
 *
 * The armed button is held while the primary finger drags, so releasing the
 * primary selects the menu item.  A tap is the extra finger(s) lifting while
 * the primary stays put; if instead the primary drags we commit to a scroll.
 * The number of distinct extra fingers seen (not their peak simultaneous
 * count) picks button 3 vs 2, so the two middle-button fingers may land and
 * lift staggered rather than exactly together.
 */
void
gesture(Gesture *g, Touch *t, int fd)
{
	int i, j, prim, b, dx, dy, press;
	vlong d, now;

	now = nsec();
	b = t->button ? 1 : 0;
	press = b && !g->hwb;		/* physical button just went down */
	g->hwb = b;

	if(t->n == 0){
		/* all fingers up: finish whatever gesture was in progress */
		if(g->menub){
			emitmouse(fd, 0, 0, 0);		/* release held menu button */
		} else if(g->mode == Gcursor && !g->moved && !g->lastb
		      && now - g->downtime < Taptime){
			/* clickfinger: a quick, still, one-finger touch = click */
			emitmouse(fd, 0, 0, 1);
			emitmouse(fd, 0, 0, 0);
		} else if(b != g->lastb){
			emitmouse(fd, 0, 0, b);		/* release physical button */
		}
		g->mode = Gidle;
		g->menub = 0;
		g->lastb = b;
		g->sacc = 0;
		return;
	}

	if(g->mode == Gidle){
		/* first finger(s) of a new touch: latch a primary contact */
		prim = findprim(t);
		g->primid = t->id[prim];
		g->px = t->x[prim];
		g->py = t->y[prim];
		g->downtime = now;
		g->downx = t->x[prim];
		g->downy = t->y[prim];
		g->moved = 0;
		g->mode = Gcursor;
	}

	/* track the primary finger across frames by its contact id */
	prim = findid(t, g->primid);
	if(prim < 0){
		/* primary lifted but other fingers remain: re-latch a primary */
		if(g->menub){
			emitmouse(fd, 0, 0, 0);
			g->menub = 0;
		}
		prim = findprim(t);
		g->primid = t->id[prim];
		g->px = t->x[prim];
		g->py = t->y[prim];
		g->moved = 1;			/* not a clean single-finger tap */
		if(t->n >= 2){
			g->mode = Gpending;
			g->pendtime = now;
			g->pendx = t->x[prim];
			g->pendy = t->y[prim];
			g->nextra = 0;
		} else
			g->mode = Gcursor;
		g->lastb = b;
		return;
	}

	if(press){
		g->suppress = Suppressframes;
		g->moved = 1;			/* a real click is never a tap */
	}

	switch(g->mode){
	case Gpending:
		/*
		 * Remember every distinct non-primary finger, so the two extra
		 * fingers of a middle-button tap need not be down at the same
		 * instant -- staggered taps still count as two.
		 */
		for(i = 0; i < t->n; i++){
			if(t->id[i] == g->primid)
				continue;
			for(j = 0; j < g->nextra; j++)
				if(g->extraid[j] == t->id[i])
					break;
			if(j == g->nextra && g->nextra < Maxcontacts)
				g->extraid[g->nextra++] = t->id[i];
		}

		/* has the primary travelled far enough to be a scroll drag? */
		dx = dy = 0;
		d = (vlong)(uint)t->x[prim] - (vlong)(uint)g->pendx;
		if(d < 0)
			d = -d;
		if(d > Scrollstart)
			dx = 1;
		d = (vlong)(uint)t->y[prim] - (vlong)(uint)g->pendy;
		if(d < 0)
			d = -d;
		if(d > Scrollstart)
			dy = 1;

		if(dx || dy || now - g->pendtime >= Menuwindow){
			/* commit to scroll; carry the motion so far into sacc */
			g->mode = Gscroll;
			g->sacc = (vlong)(uint)t->y[prim] - (vlong)(uint)g->pendy;
			g->px = t->x[prim];
			g->py = t->y[prim];
			g->lastb = b;
			return;
		}
		if(t->n == 1){
			/*
			 * extra finger(s) tapped and lifted while the primary
			 * held still: arm a menu button.  Two distinct extra
			 * fingers = middle (2), one = right (4).
			 */
			g->menub = g->nextra >= 2 ? 2 : 4;
			g->mode = Gmenu;
			g->px = t->x[prim];
			g->py = t->y[prim];
			emitmouse(fd, 0, 0, g->menub);
			g->lastb = b;
			return;
		}
		g->px = t->x[prim];
		g->py = t->y[prim];
		g->lastb = b;
		return;			/* suppress motion while deciding */

	case Gscroll:
		if(t->n >= 2){
			/* two-finger scroll: track primary y, no cursor motion */
			g->sacc += (vlong)(uint)t->y[prim] - (vlong)(uint)g->py;
			while(g->sacc >= Scrollthresh){
				emitmouse(fd, 0, 0, b|16);
				emitmouse(fd, 0, 0, b);
				g->sacc -= Scrollthresh;
			}
			while(g->sacc <= -Scrollthresh){
				emitmouse(fd, 0, 0, b|8);
				emitmouse(fd, 0, 0, b);
				g->sacc += Scrollthresh;
			}
			g->px = t->x[prim];
			g->py = t->y[prim];
			g->lastb = b;
			return;
		}
		/* dropped to one finger: hand the primary to cursor mode */
		g->mode = Gcursor;
		g->moved = 1;
		g->px = t->x[prim];
		g->py = t->y[prim];
		g->lastb = b;
		return;

	case Gmenu:
		/* primary drags with the menu button held (ignore extra fingers) */
		d = (vlong)(uint)t->x[prim] - (vlong)(uint)g->px;
		dx = (d * Tpgainx) >> 31;
		d = (vlong)(uint)t->y[prim] - (vlong)(uint)g->py;
		dy = (d * Tpgainy) >> 31;
		g->px = t->x[prim];
		g->py = t->y[prim];
		if(dx != 0 || dy != 0)
			emitmouse(fd, dx, dy, g->menub);
		g->lastb = b;
		return;
	}

	/* Gcursor: single finger drives the cursor */
	if(t->n >= 2){
		/* a second finger arrived: begin the scroll-vs-menu decision */
		g->mode = Gpending;
		g->pendtime = now;
		g->pendx = t->x[prim];
		g->pendy = t->y[prim];
		g->nextra = 0;
		g->px = t->x[prim];
		g->py = t->y[prim];
		g->lastb = b;
		return;
	}

	d = (vlong)(uint)t->x[prim] - (vlong)(uint)g->px;
	dx = (d * Tpgainx) >> 31;
	d = (vlong)(uint)t->y[prim] - (vlong)(uint)g->py;
	dy = (d * Tpgainy) >> 31;

	/* moved too far from the touch-down point to still be a tap? */
	d = (vlong)(uint)t->x[prim] - (vlong)(uint)g->downx;
	if(d < 0)
		d = -d;
	if(d > Taptravel)
		g->moved = 1;
	d = (vlong)(uint)t->y[prim] - (vlong)(uint)g->downy;
	if(d < 0)
		d = -d;
	if(d > Taptravel)
		g->moved = 1;

	/* drop the small cursor jump the finger makes while pressing */
	if(g->suppress > 0){
		g->suppress--;
		dx = dy = 0;
	}

	g->px = t->x[prim];
	g->py = t->y[prim];

	if(dx != 0 || dy != 0 || b != g->lastb)
		emitmouse(fd, dx, dy, b);
	g->lastb = b;
}
