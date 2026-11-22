#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/pci.h"

#define MMIOBASE 0x40000000
#define ECAMBASE 0x30000000
#define ECAMSIZE 0x01000000

typedef struct Intvec Intvec;

struct Intvec {
	Pcidev *p;
	void (*f)(Ureg*, void*);
	void *a;
};

static uchar *ecam;
static Intvec vec[32];

static void*
cfgaddr(int tbdf, int rno)
{
	return ecam + (BUSBNO(tbdf)<<20 | BUSDNO(tbdf)<<15 | BUSFNO(tbdf)<<12) + rno;
}

int
pcicfgrw32(int tbdf, int rno, int data, int read)
{
	u32int *p;

	if((p = cfgaddr(tbdf, rno & ~3)) != nil){
		if(read)
			data = *p;
		else
			*p = data;
	} else {
		data = -1;
	}
	return data;
}

int
pcicfgrw16(int tbdf, int rno, int data, int read)
{
	u16int *p;

	if((p = cfgaddr(tbdf, rno & ~1)) != nil){
		if(read)
			data = *p;
		else
			*p = data;
	} else {
		data = -1;
	}
	return data;
}

int
pcicfgrw8(int tbdf, int rno, int data, int read)
{
	u8int *p;

	if((p = cfgaddr(tbdf, rno)) != nil){
		if(read)
			data = *p;
		else
			*p = data;
	} else {
		data = -1;
	}
	return data;
}

static void
pciinterrupt(Ureg *ureg, void *)
{
	int i;
	Intvec *v;

	for(i = 0; i < nelem(vec); i++){
		v = &vec[i];
		if(v->f)
			v->f(ureg, v->a);
	}
}

static void
pciintrinit(void)
{
	intrenable(IRQpci1, pciinterrupt, nil, BUSUNKNOWN, "pci");
	intrenable(IRQpci2, pciinterrupt, nil, BUSUNKNOWN, "pci");
	intrenable(IRQpci3, pciinterrupt, nil, BUSUNKNOWN, "pci");
	intrenable(IRQpci4, pciinterrupt, nil, BUSUNKNOWN, "pci");
}

void
pciintrenable(int tbdf, void (*f)(Ureg*, void*), void *a)
{
	Pcidev *p;
	int i;
	Intvec *v;

	if((p = pcimatchtbdf(tbdf)) == nil){
		print("pciintrenable: %T: unknown device\n", tbdf);
		return;
	}

	for(i = 0; i < nelem(vec); i++){
		v = &vec[i];
		if(v->f == nil){
			v->p = p;
			v->f = f;
			v->a = a;
			return;
		}
	}
}

void
pciintrdisable(int tbdf, void (*f)(Ureg*, void*), void *a)
{
	Pcidev *p;
	int i;
	Intvec *v;

	if((p = pcimatchtbdf(tbdf)) == nil){
		print("pciintrdisable: %T: unknown device\n", tbdf);
		return;
	}

	for(i = 0; i < nelem(vec); i++){
		v = &vec[i];
		if(v->p == p && v->f == f && v->a == a)
			v->f = nil;
	}
}

/*
 * QEMU's PCIe host puts the INTx routing in the DTB interrupt-map,
 * not in the device's Interrupt Line register.  The map for bus 0 is
 * the usual rotating one: device 0 pins A-D use IRQ 32-35, device 1
 * starts at 33, and so on.  Drivers read Pcidev.intl, so fill it in.
 */
static void
pciintl(void)
{
	Pcidev *p;
	int dev, pin;
	static uchar map[4][4] = {
		{IRQpci1, IRQpci2, IRQpci3, IRQpci4},
		{IRQpci2, IRQpci3, IRQpci4, IRQpci1},
		{IRQpci3, IRQpci4, IRQpci1, IRQpci2},
		{IRQpci4, IRQpci1, IRQpci2, IRQpci3},
	};

	for(p = nil; (p = pcimatch(p, 0, 0)) != nil; ){
		if(BUSBNO(p->tbdf) != 0)
			continue;
		if(p->intl != 0 && p->intl != 0xff)
			continue;
		pin = pcicfgr8(p, PciINTP);
		if(pin < 1 || pin > 4)
			continue;
		dev = BUSDNO(p->tbdf) & 3;
		p->intl = map[dev][pin-1];
		pcicfgw8(p, PciINTL, p->intl);
	}
}

static void
pcicfginit(void)
{
	char *p;
	Pcidev *pciroot;
	ulong ioa;
	uvlong base;

	fmtinstall('T', tbdffmt);

	pcimaxdno = 32;
	if(p = getconf("*pcimaxdno"))
		pcimaxdno = strtoul(p, 0, 0);

	pciscan(0, &pciroot, nil);
	if(pciroot == nil)
		return;

	if(dtbsocmatch("riscv-virtio"))
		pciintl();

	pciintrinit();

	ioa = 0;
	base = MMIOBASE;
	pcibusmap(pciroot, &base, &ioa, 1);

	if(getconf("*pcihinv"))
		pcihinv(pciroot);
}

void
pciqemulink(void)
{
	ecam = vmap(ECAMBASE, ECAMSIZE);
	pcicfginit();
}
