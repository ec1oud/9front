# Plan 9 from RISC-V space

In this branch is an in-progress port of [9front](https://9front.org/) to the
64-bit [RISC-V](https://en.wikipedia.org/wiki/RISC-V) architecture.  The
porting work is done first on qemu; and on hardware, the first working board is
the [Banana Pi BPI-F3](https://docs.banana-pi.org/en/BPI-F3/BananaPi_BPI-F3).

Progress is tracked in [kanban.md](kanban.md).

## Getting started with this port

Assuming you have a working 9front installation (PC or raspberry pi).  Perhaps
later we'll have a wiki with more details, but roughly:

### Method with Linux hosting the git repo

```
$ git clone https://github.com/rminnich/9front.git 9front-riscv
$ drawterm -a ... -h ...
% . sys/lib/minimalrootbind
```
### Method with 9front hosting the git repo

```
% git/clone https://github.com/rminnich/9front.git 9front-riscv
% cd 9front-riscv
% . sys/lib/minimalrootbind
```
minimalrootbind causes e.g. /amd64 to be read-only in the shell session, but
then binds back as writeable the few host tools and libs that we need to
overwrite (`ar`, `jc`, `jl`, `ja`, `libmach.a`) without polluting the real host
fs.

You will need to create /riscv64, to install stuff into.  That is filesystem
dependent: e.g. read the gefs man page to learn how to use `con -C /srv/gefs.cmd `
or whatever is appropriate on your system.

```
% mkdir /riscv64/include
% dircp riscv64/include /riscv64/include
% cd /
% . /sys/lib/rootstub
```

### Build compiler, linker, assembler, ar

Next: the host's `ar`, `jc`, `jl` need to be rebuilt against a `libmach.a` that
knows about the `.j` (riscv64) object format.  Without this, `ar` silently omits
the `__.SYMDEF` symbol table when building `libc.a`, and `jl` later refuses to
link with "first entry not symbol header".  Let's assume your objtype is
already set to the host architecture:

```
% cd /sys/src/libmach && mk install               # libmach.a with riscv64
% cd /sys/src/cmd
% mk ar.install
% cd cc
% mk install
% cd ../jc
% mk install
% cd ../ja
% mk install
% cd ../jl
% mk install
% cd ../nm
% mk install
```
Sanity-check: an archive built with the new `ar` should have `__.SYMDEF` as its
first member.

```
% jc -o /tmp/u.j /sys/src/libc/port/utflen.c      # any tiny .j file
% rm -f /tmp/test.a
% ar r /tmp/test.a /tmp/u.j
% ar t /tmp/test.a | sed 1q
__.SYMDEF
```
If the first entry is the `.j` filename instead of `__.SYMDEF`, the ar/libmach
rebuild didn't take: try again to rebuild them.

### Build the riscv64 libc and other libraries

```
% objtype=riscv64
% rm -f /riscv64/lib/libc.a                       # force a clean archive
% cd /sys/src/libc && mk install
% cd /sys/src/libc/riscv64 && mk install
% ar t /riscv64/lib/libc.a | sed 1q               # confirm __.SYMDEF again
% cd /sys/src
for (i in libmp libbio libflate libsec libauth libauthsrv libString libthread lib9p libndb libdraw libcontrol libmemlayer libmemdraw libip libregexp ) @{ cd $i; mk install }
```

### Build the riscv64 binaries that go into the bootfs

The kernel's `bootfs.paq` packs `paqfs`, `rc`, and `auth/factotum` (see the `
bootdir` section of `sys/src/9/riscv64/qemu`).  These must be built for riscv64,
not the host:

```
% objtype=riscv64
% cd /sys/src/cmd
% mk -k install
```

### Build the kernel

```
% objtype=riscv64
% cd /sys/src/9/riscv64
% rm -f bootfs.paq             # repack with the fresh bootdir binaries
% mk
```
This produces `9qemu` (Plan 9 a.out, for booting via SBI) and `9qemu.elf` (ELF
wrapper, for qemu's `\-kernel`).

### qemu

If your git tree is hosted on Linux and you did the above steps via drawterm,
you've already got the qemu image on Linux; otherwise copy it over, or use 9pfs
to mount the 9front FS on Linux.

```
$ qemu-system-riscv64 -M virt -m 8G -kernel 9qemu -nographic -s
```
If everything is working you should see paqfs print `qemu.bootfs:` and `
fingerprint:` lines, then bootrc start running.

## Troubleshooting

- `first entry not symbol header` when linking with `libc.a`: ar built the
  archive without a symbol index.  Almost always means the host `ar` is linked
  against a libmach without riscv64 awareness.  Redo step 3 with `mk clean`
  first.
- `jdb.c: 'jc/enam.c' file not found` when building libmach: run `cd
  /sys/src/cmd/jc && rc mkenam` to generate it.  This should be auto-handled by
  the mkfile rule in `sys/src/libmach/mkfile`, but stale build artefacts can
  sometimes skip it.
- `exec header invalid` during boot: one of the binaries in `bootfs.paq` is
  stale (built before the toolchain was rebuilt) or for the wrong arch. Rebuild
  the bootfs binaries in step 5, `rm bootfs.paq`, and rebuild the kernel.
- bootargs prompt loops forever: the `read` from `/dev/cons` returns empty
  because either `aux/kbdfs` is missing from the bootfs or `uartsbi`'s user-mode
  read path isn't delivering input.  As a workaround, set `
  nobootprompt=local\!nothing` at the top of `sys/src/9/boot/bootrc` to bypass
  the prompt.
- Can't cd riscv64: directory entry not found: 'riscv64' (or any
  infinite-looking loop in a multi-arch lib install): the lib's
  top-level mkfile descends into $objtype/ and the riscv64 stub dir
  isn't visible. The affected libs are libc, libmp, libsec (libmach,
  libbio, libflate are not multi-arch in this layout).

