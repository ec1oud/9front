/*
 *	RV64A 64-bit atomic operations (see atomic(2)).
 *	These intentionally shadow the lock-based fallbacks in
 *	libc/port/atomic64.c -- ar will warn about the duplicates;
 *	that mirrors amd64 and arm64.
 */
#include <atom.h>

#define ARG	8

/*
 *	vlong	agetv(Avlong *);
 */
TEXT agetv(SB), 1, $-4
	FENCE_RW
	MOV	(R(ARG)), R(ARG)
	FENCE_RW
	RET

/*
 *	vlong	aswapv(Avlong *, vlong);
 */
TEXT aswapv(SB), 1, $-4
	MOV	new+XLEN(FP), R9
	FENCE_RW
	AMOD(Amoswap, AQ|RL, 9, ARG, ARG)
	FENCE_RW
	RET

/*
 *	vlong	aincv(Avlong *, vlong);
 */
TEXT aincv(SB), 1, $-4
	MOV	d+XLEN(FP), R9
	FENCE_RW
	AMOD(Amoadd, AQ|RL, 9, ARG, 10)
	FENCE_RW
	ADD	R9, R10, R(ARG)
	RET

/*
 *	int	acasv(Avlong *, vlong ov, vlong nv);
 */
TEXT acasv(SB), 1, $-4
	MOV	ov+XLEN(FP), R12
	MOV	nv+(2*XLEN)(FP), R13
	MOV	R0, R11			/* default to failure */
	FENCE_RW
acasvspin:
	LRD(ARG, 14)
	BNE	R12, R14, acasvdone
	/* no FENCE here: any load/store/fence between LR and SC
	 * makes the loop "unconstrained" and forfeits the
	 * forward-progress guarantee (unpriv ISA §13.3). */
	SCD(13, ARG, 14)
	BNE	R14, acasvspin
	MOV	$1, R11
acasvdone:
	FENCE_RW
	MOV	R11, R(ARG)
	RET
