/*
 *	RV64A atomic operations -- modern Along/Aptr API (see atomic(2)).
 *	vlong variants live in atomic64.s.
 *
 *	AMO* and LR/SC only work on cached regions.
 *	AQ|RL on each AMO acts as a fence for that operation alone;
 *	we add explicit FENCE_RW around the AMOs to order them with
 *	surrounding loads and stores.
 */
#include <atom.h>

#define ARG	8

/*
 * get variants -- atomic load with a full memory barrier.
 *
 *	long	agetl(Along *);
 *	void*	agetp(Aptr *);
 */
TEXT agetl(SB), 1, $-4
	FENCE_RW
	MOVW	(R(ARG)), R(ARG)
	FENCE_RW
	RET

TEXT agetp(SB), 1, $-4
	FENCE_RW
	MOV	(R(ARG)), R(ARG)
	FENCE_RW
	RET

/*
 * swap variants -- atomically replace *p with new, return old.
 *
 *	long	aswapl(Along *, long);
 *	void*	aswapp(Aptr *,  void *);
 */
TEXT aswapl(SB), 1, $-4
	MOVW	new+XLEN(FP), R9
	FENCE_RW
	AMOW(Amoswap, AQ|RL, 9, ARG, ARG)
	FENCE_RW
	RET

TEXT aswapp(SB), 1, $-4
	MOV	new+XLEN(FP), R9
	FENCE_RW
	AMOD(Amoswap, AQ|RL, 9, ARG, ARG)
	FENCE_RW
	RET

/*
 * inc -- atomically add delta to *p, return the new value.
 *
 *	long	aincl(Along *, long);
 */
TEXT aincl(SB), 1, $-4
	MOVW	d+XLEN(FP), R9
	FENCE_RW
	AMOW(Amoadd, AQ|RL, 9, ARG, 10)
	FENCE_RW
	ADDW	R9, R10, R(ARG)
	RET

/*
 * cas variants -- compare-and-swap; return 1 on success, 0 on failure.
 *
 *	int	acasl(Along *, long  ov, long  nv);
 *	int	acasp(Aptr *,  void* ov, void* nv);
 */
TEXT acasl(SB), 1, $-4
	MOVWU	ov+XLEN(FP), R12
	MOVWU	nv+(XLEN+4)(FP), R13
	MOV	R0, R11			/* default to failure */
	FENCE_RW
acaslspin:
	LRW(ARG, 14)
	SLL	$32, R14
	SRL	$32, R14		/* zero-extend; LRW sign-extends */
	BNE	R12, R14, acasldone
	/* no FENCE here: any load/store/fence between LR and SC
	 * makes the loop "unconstrained" and forfeits the
	 * forward-progress guarantee (unpriv ISA §13.3). */
	SCW(13, ARG, 14)
	BNE	R14, acaslspin		/* R14 != 0 means store failed */
	MOV	$1, R11
acasldone:
	FENCE_RW
	MOV	R11, R(ARG)
	RET

TEXT acasp(SB), 1, $-4
	MOV	ov+XLEN(FP), R12
	MOV	nv+(2*XLEN)(FP), R13
	MOV	R0, R11			/* default to failure */
	FENCE_RW
acaspspin:
	LRD(ARG, 14)
	BNE	R12, R14, acaspdone
	/* no FENCE here: see comment in acaslspin */
	SCD(13, ARG, 14)
	BNE	R14, acaspspin
	MOV	$1, R11
acaspdone:
	FENCE_RW
	MOV	R11, R(ARG)
	RET

/*
 * full memory barrier (see atomic(2)).
 */
TEXT coherence(SB), 1, $-4
	FENCE_RW
	RET
