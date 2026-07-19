# Size Optimization — Technical Reference

Reference material for the 15 size-reduction flags used by `tiny_pomodoro`,
plus the compilation-pipeline and ELF background needed to explain why each one
works.

Each flag entry lists *what it does*, *why it shrinks the binary*, *how it
overlaps with the others*, *what you give up*, and *where to read more*. Flags
are grouped by **what they do**, not by which tool consumes them, so flags whose
effects overlap sit next to each other.

Measurements were taken 2026-07-19 with clang 22 on x86-64 Linux, FLTK 1.4.5.
Absolute byte counts are toolchain-specific; the figures used in the talk live
in `presentation/index.html`.

---

# Part 0 — Compilation and ELF background

## How a C++ file becomes an executable

Every one of the 15 flags acts at exactly one stage of this pipeline.

| # | Stage | Tool | Input → Output |
|---|-------|------|----------------|
| 1 | Preprocess | `clang -E` | `.cpp` + `#include`s → one big translation unit (TU) |
| 2 | Parse / semantic analysis | clang front-end | TU → abstract syntax tree (AST) |
| 3 | Intermediate-representation generation | clang front-end | AST → **LLVM intermediate representation (IR)** |
| 4 | Optimise | LLVM middle-end | IR → better IR (target-independent) |
| 5 | Code generation | LLVM back-end | IR → **machine code**, packed into an `.o` |
| 6 | Link | LLD (the LLVM linker) | many `.o` + libraries → one executable |
| 7 | Load | Linux kernel + `ld.so` | executable → a running process |

Points that are easy to get wrong:

- **The AST is not the IR.** The AST is a faithful tree of the C++ source (it
  still knows about templates, classes, `for` loops). LLVM IR is a low-level,
  typed, static-single-assignment (SSA) form instruction language with no C++ in
  it at all. `-fno-exceptions` and `-fno-rtti` act at the *front-end* — they
  change what IR is even generated. By stage 4 the information is no longer
  present to remove.
- **Stage 4 is target-independent, stage 5 is not.** `-Oz` mostly steers stage
  4; instruction-encoding choices happen in stage 5.
- **An `.o` file is a bag of sections, plus a symbol table, plus relocations.**
  It is not yet a program: addresses are unresolved placeholders. Relocations
  are the "fill this in later" notes the linker reads.
- **`.o` and the final executable are both Executable and Linkable Format
  (ELF)**, but different *types*: `ET_REL` (relocatable) vs `ET_EXEC`/`ET_DYN`
  (executable). `readelf -h` shows this on the `Type:` line.

## What `-Oz` changes (stage 4)

`-Os` and `-Oz` both start from the **`-O2` pass pipeline** — neither is "`-O0`
but smaller". `-Oz` sets the LLVM **`minsize`** function attribute (`-Os` sets
`optsize`), which is consulted by the cost models of individual passes:

- **inlining threshold**: roughly **5** at `-Oz`, **50** at `-Os`, **225** at
  `-O2`/`-O3`. This is the largest single lever.
- **loop unrolling**: off.
- **loop vectorisation / superword-level-parallelism (SLP) vectorisation**: off.
- prefers a `call` to a shared helper over inlining it; prefers shorter
  instruction encodings even when marginally slower.

Runtime cost is workload-dependent: negligible on an idle program such as a
timer, potentially a large multiple on a hot numeric loop.

## Under `-flto=thin`, the compiler does not generate machine code

```
$ file build/tiny/CMakeFiles/tiny_pomodoro_pro_plus.dir/src/main_pro_plus.cpp.o
… LLVM IR bitcode
$ file build/release/CMakeFiles/tiny_pomodoro_pro_plus.dir/src/main_pro_plus.cpp.o
… ELF 64-bit LSB relocatable, x86-64
```

With `-flto=thin` the object file contains no machine code — it is serialised
LLVM IR. Stages 4 and 5 have been moved into the linker. This is why link-time
optimization (LTO) can do things `--gc-sections` cannot (it optimises across
translation units, before codegen), and equally why it is powerless against a
pre-built static archive such as `libfltk.a`, which holds native code rather
than bitcode.

`nm` on a bitcode `.o` fails or reports nothing useful; `llvm-nm` reads it,
because the file is not ELF.

---

## The ELF file format

> **The linker thinks in _sections_. The kernel thinks in _segments_.
> The flags all operate on sections. The file size is decided by segments.**

That gap is why the flags stop paying off at roughly 8 KB.

### The four regions of an ELF file

1. **ELF header** (64 bytes). Magic `\x7fELF`, class (64-bit), type
   (`ET_EXEC`/`ET_DYN`), machine (x86-64), entry point, and the offsets of the
   next two tables. `readelf -h`.
2. **Program header table** (the *segment* view). One entry per segment.
   This is what the kernel reads. `readelf -lW`.
3. **The content** — the sections themselves (`.text`, `.rodata`, `.data`,
   `.bss`, `.symtab`, …).
4. **Section header table** (the *section* view). The kernel never reads this.
   It exists for the linker, the debugger, `objdump`, `readelf`. It can be
   deleted and the program still runs.

### Sections vs segments

The **same bytes** are described twice, by two tables, for two consumers:

| | Sections | Segments |
|---|---|---|
| Described by | section header table | program header table |
| Consumer | linker, debugger, `objdump` | the kernel's ELF loader |
| Typical count | ~30 | **3–4** |
| Granularity | one per kind of content | one per **memory protection** |
| Needed to run? | **no** | **yes** |

`readelf -lW` prints the mapping between them at the bottom ("Section to
Segment mapping"). For `tiny_pomodoro_2`:

```
LOAD  Offset 0x000000  FileSiz 0x0000e8  Flg R    Align 0x1000   ← ELF+prog headers
LOAD  Offset 0x001000  FileSiz 0x00007a  Flg R E  Align 0x1000   ← .text
LOAD  Offset 0x002000  FileSiz 0x000001  Flg R    Align 0x1000   ← .rodata
```

### Why segments must live on separate pages

The kernel loads a program by `mmap()`ing each `PT_LOAD` segment into the
process's address space. Two facts collide:

1. `mmap()` works at **page granularity** — 4 KiB on x86-64.
2. Memory protection (`r`/`w`/`x`) is a property of a **page**, set in the page
   table. There is no finer unit; the hardware memory-management unit (MMU)
   cannot mark part of a page executable and the rest not.

So content with **different protection cannot share a page**. `.text` needs
`R+X`; `.rodata` needs `R` and must not be `X` (an executable read-only segment
is a code-injection surface); `.data`/`.bss` need `R+W` and must not be `X`.
Three protections ⇒ three segments ⇒ three pages minimum, regardless of how few
bytes are in them.

This is why `tiny_pomodoro_2` — **122 bytes of code** — is **8,480 bytes on
disk**. Its three pages are nearly empty:

| Page | Real content | Bytes used | Bytes wasted |
|------|--------------|-----------:|-------------:|
| 1 (`R`) | ELF header + 3 program headers | 232 | 3,864 |
| 2 (`R+X`) | `.text` | 122 | 3,974 |
| 3 (`R`) | `.rodata` — a single `0x07` BEL (bell) byte | 1 | ~31 |
| — | section headers + `.shstrtab` | ~64 | — |

Roughly 419 bytes of content in an 8,480-byte file — about 95% padding. The
file is not 12,288 bytes only because the last page is truncated rather than
padded out.

Confirmed by comparing the two assembly builds: `_ultra` has **3,046 bytes** of
`.text`, `_2` has **122** — 25× more code for a 6% larger file (8,992 vs 8,480
bytes). Once content fits inside a page, more code is free until it spills into
the next one.

### On-disk size and runtime size are different axes

Two sections from this project at opposite corners:

| | On disk | In memory at runtime |
|---|---|---|
| **`.bss`** (`_ultra`'s 1 MiB thread stack) | **0 bytes** | **1 MiB** |
| **`.symtab`** (`tiny_pomodoro`, unstripped) | **~240 KB** | **0 bytes** |

`.bss` is zero-initialised data: the ELF records only how big it is (`MemSiz` >
`FileSiz` in the program header) and the kernel supplies zeroed pages. It costs
nothing in the file. `.symtab` is the reverse — never in a `PT_LOAD` segment, so
the loader never maps it, but every byte occupies disk.

"Binary size" is therefore not one number, and the flags do not all act on the
same one. `-Wl,-s` is a disk optimisation with zero runtime effect.
`-fvisibility=hidden` is primarily a runtime optimisation (fewer dynamic
relocations, faster startup) that also shrinks the file somewhat.

### Going below the page floor

1. **Merge the segments.** Put `.rodata` in the same `R+X` segment as `.text` →
   two pages, then one. Requires a custom linker script; no standard flag
   exposes it.
2. **Overlap the headers.** The ELF header has padding (`e_ident[EI_PAD]`) and
   fields the kernel ignores; the program header table can be hidden inside them.
3. **Hand-roll the ELF.** Brian Raiter's *Teensy ELF* reaches **45 bytes** by
   abandoning sections entirely and using header padding as instructions. The
   result is unstrippable, un-debuggable and kernel-version-fragile.

### Inspection tools

| Command | Shows |
|---------|-------|
| `readelf -h <bin>` | ELF header — type, entry point |
| `readelf -lW <bin>` | segments + section→segment mapping (`Align 0x1000`) |
| `readelf -SW <bin>` | sections — names, sizes, offsets |
| `readelf -d <bin>` | `.dynamic` — the `DT_NEEDED` library list |
| `size <bin>` | `.text` / `.data` / `.bss` summary |
| `nm -C <bin>` | symbols (fails on a stripped binary) |
| `file <x>.o` | distinguishes LLVM bitcode from ELF under `-flto` |
| `ldd <bin>` | what actually gets loaded, vs what was passed to `-l` |
| `bloaty <bin>` | per-section/per-symbol byte attribution |

---

## Unwind tables and exception-handling data

Background for Cluster D. The sections involved are `.eh_frame`,
`.eh_frame_hdr` and `.gcc_except_table`.

### What unwinding is

*Unwinding* is walking back up the call stack, one frame at a time, undoing
each frame as you go: restoring the callee-saved registers the function
overwrote, resetting the stack pointer, and finding the return address of the
caller below. Consumers:

- **C++ exceptions** — after a `throw`, the runtime must find a matching
  `catch`, then destroy every automatic object between the throw point and the
  handler.
- **`backtrace()`**, debuggers (`gdb bt`), and sampling profilers
  (`perf --call-graph=dwarf`).
- **Sanitizers**, which print a stack trace on a diagnosed error.
- **`pthread_cancel`** and other forced-unwind mechanisms.

### Why a side table is needed at all

The naive way to walk a stack is to follow the frame-pointer chain: each
function pushes the caller's `%rbp`, so the saved values form a linked list.
Optimising compilers do not do this — at `-O1` and above `%rbp` is freed up as
an ordinary general-purpose register, and the chain does not exist.

Without it, nothing in the machine code says where the return address lives.
The offset from the stack pointer changes constantly: after the prologue
subtracts from `%rsp`, after every `push`, in the middle of the epilogue. So
the compiler emits a **side table** that answers, *for any given instruction
address*, two questions: where is the caller's stack frame, and where were the
callee-saved registers spilled.

### DWARF Call Frame Information (CFI)

Conceptually the table has one row per instruction address, a column for the
**canonical frame address** (CFA — a fixed reference point in the caller's
frame) and one column per register saying how to recover its original value.
Stored literally this would dwarf the code it describes, so it is encoded as a
**bytecode program** of `DW_CFA_*` opcodes that an interpreter replays from the
start of the function up to the address of interest.

The encoding has two record types. A **CIE** (Common Information Entry) holds
the preamble shared by many functions; an **FDE** (Frame Description Entry)
covers one function's address range and points back at a CIE. From the terminal
build:

```
00000000 0000000000000014 00000000 CIE
  Version:               1
  Augmentation:          "zR"
  Code alignment factor: 1
  Data alignment factor: -8
  Return address column: 16
  DW_CFA_def_cfa: r7 (rsp) ofs 8
  DW_CFA_offset: r16 (rip) at cfa-8

00000018 0000000000000010 0000001c FDE cie=00000000 pc=00000000000026f0..0000000000002716
  DW_CFA_advance_loc: 4 to 00000000000026f4
  DW_CFA_undefined: r16 (rip)
```

The CIE's two initial rules describe the state on entry to any function:
`CFA = rsp + 8`, and the return address (register 16, `rip`) sits at `CFA - 8`
— exactly the situation right after a `call` instruction has pushed it. Each
FDE then adds opcodes that adjust those rules as the function's prologue moves
the stack pointer. `DW_CFA_advance_loc` moves to a later instruction address;
the alignment factors exist so offsets can be stored as small integers.

### `.eh_frame` vs `.debug_frame`

The same information exists in two sections. `.debug_frame` is debug-only: not
allocated, not loaded, discarded by `strip`. `.eh_frame` is the runtime copy —
it is `SHF_ALLOC`, lives in a `PT_LOAD` segment, is mapped into memory, and
**survives stripping**, because a program that throws needs it in order to run.
This is why `.eh_frame` counts against on-disk size, RSS *and* load time, while
`.symtab` costs only disk.

### `.eh_frame_hdr`

Given a program counter, the unwinder must find the FDE covering it. Scanning
every FDE would be linear in the number of functions, so the linker emits a
sorted binary-search index:

```
Version:                 1
Table Encoding Format:   0x3b (sdata4, datarel)
Entries in search table: 0x19
  0x1480 (offset: 0x26f0) -> 0xf0 fde=[    18]
  0x159c (offset: 0x280c) -> 0x2f4 fde=[   21c]
  …
```

Each row is a (function address, FDE pointer) pair, sorted by address. The
section is pointed at by its own program header, **`PT_GNU_EH_FRAME`**, which is
how the runtime locates it — the unwinder never reads section headers, which
may not even be present:

```
GNU_EH_FRAME   0x001270 0x0000000000001270 … 0x0000d4 R 0x4
```

### `.gcc_except_table` — the language-specific data area

`.eh_frame` says *how* to unwind a frame. It says nothing about C++. The
language-specific data area (LSDA) in `.gcc_except_table` holds, per function:

- a **call-site table** — which ranges of instructions are covered by which
  `try` block, and the address of the landing pad to jump to,
- an **action table** — what to do on arrival (run cleanups, test a catch type),
- a **type table** — the `type_info` pointers a `catch` clause matches against.

This is the section `-fno-exceptions` removes outright, because without `throw`
and `catch` there are no handlers to describe. It is also the reason
`-fno-rtti` and exceptions interact: the type table refers to `type_info`
objects.

### The personality routine and two-phase unwinding

The CIE's augmentation data points at a **personality routine** — for C++ under
the Itanium ABI, `__gxx_personality_v0`. It is the language-specific interpreter
of the LSDA, and unwinding runs in two passes:

1. **Search.** Walk up the stack asking each frame's personality routine
   whether it has a handler matching the thrown type. Nothing is modified.
2. **Cleanup.** Walk up again, this time running destructors and cleanup
   landing pads, then transfer control to the handler found in phase 1.

Two passes exist so that when *no* handler is found anywhere, `std::terminate`
can be called with the stack still intact and debuggable, rather than after
everything has already been destroyed.

### Synchronous vs asynchronous tables

This distinction is what Cluster D turns on.

- **Synchronous** unwind info only has to be correct at points where an
  exception can originate — that is, at call sites. Between calls the tables may
  be stale.
- **Asynchronous** unwind info must be correct at **every instruction
  boundary**, including part-way through a prologue where the stack pointer has
  moved but the frame is not yet established.

Anything that can interrupt execution at an arbitrary instruction needs the
asynchronous form: signal delivery, a debugger stopping the process, a sampling
profiler firing on a timer. It costs more because every individual stack
adjustment needs its own CFI row rather than one row per call site. The x86-64
psABI mandates it, which is why it is on by default and why
`-fno-unwind-tables` alone cannot switch it off.

---

## Symbol tables and the dynamic section

Background for Cluster E. The sections involved are `.symtab`/`.strtab`,
`.dynsym`/`.dynstr`, and `.dynamic`.

### What a symbol is

An ELF symbol table is an array of fixed-size records (`Elf64_Sym`, 24 bytes
each) holding: the symbol's **name** (an offset into an associated string
table, not an inline string), its **value** (usually an address), its **size**,
a **type** (`FUNC`, `OBJECT`, `SECTION`, `FILE`, `TLS`, `NOTYPE`), a **binding**
(`LOCAL`, `GLOBAL`, `WEAK`), a **visibility**, and the index of the section it
belongs to. Names live in a separate string table so that identical prefixes can
overlap and entries stay fixed-size.

### Two symbol tables, for two different consumers

| | `.symtab` / `.strtab` | `.dynsym` / `.dynstr` |
|---|---|---|
| Contents | **every** symbol, including file-local `static`s and file names | only symbols used for **dynamic linking** |
| `SHF_ALLOC`? | **no** — address is `0` | **yes** — has a real address |
| Mapped at runtime? | never | always |
| Needed to run? | no | **yes** |
| Removed by `strip` / `-Wl,-s`? | **yes** | no — cannot be |
| Read by | debuggers, profilers, `nm`, `objdump`, the static linker | the dynamic loader |

Concretely, in the terminal build: **`.symtab` holds 88 entries, `.dynsym`
holds 37.** `.dynsym` is the strict subset that has to survive into the running
process — the definitions this object exports, plus the undefined symbols it
imports from shared libraries. `.symtab` is everything else the toolchain
recorded, which is why it is both much larger and completely disposable.

`.gnu.hash` accompanies `.dynsym`: a hash table with a Bloom filter in front,
so the loader can reject a symbol that is definitely absent without probing the
chain at all. Without it every lookup would scan the table linearly.

### Visibility, and why it usually does nothing in an executable

The visibility field takes `STV_DEFAULT` (exported and **interposable**),
`STV_HIDDEN` (never placed in `.dynsym`; demoted to a local symbol at link
time), `STV_PROTECTED` (exported but not interposable) or `STV_INTERNAL`.

*Interposable* is the expensive property. A `STV_DEFAULT` symbol can be replaced
at load time by a definition appearing earlier in the search order — that is
what `LD_PRELOAD` exploits. Because the compiler cannot prove a call to such a
symbol stays inside the module, it must route it through the PLT/GOT and cannot
inline it. A hidden symbol is known to be module-local, so calls become direct
PC-relative branches and are inlinable.

Measured here, `-fvisibility=hidden` leaves both tables **unchanged at 37 and
88 entries**, matching its zero-byte result in Part 3. The reason is structural:
an executable's `.dynsym` consists almost entirely of *imports* — undefined
references to `libc++`, `libc` and friends — and this flag only affects the
visibility of *definitions*. It pays off in shared libraries, which export large
numbers of symbols.

### `.dynamic` and `DT_NEEDED`

`.dynamic` is an array of `Elf64_Dyn` records — a `d_tag` plus a value that is
either an integer or an address — terminated by `DT_NULL`. It is the loader's
instruction list, located at runtime through the **`PT_DYNAMIC`** program
header. Common tags:

| Tag | Meaning |
|---|---|
| `DT_NEEDED` | name of a shared library this object requires; **one entry per dependency** |
| `DT_SONAME` | this object's own canonical name |
| `DT_RUNPATH` / `DT_RPATH` | extra directories to search for dependencies |
| `DT_STRTAB` / `DT_SYMTAB` | addresses of `.dynstr` and `.dynsym` |
| `DT_GNU_HASH` | address of the symbol hash table |
| `DT_RELA`, `DT_RELASZ`, `DT_JMPREL` | relocation tables and their sizes |
| `DT_PLTGOT` | address of the PLT's GOT |
| `DT_INIT_ARRAY` / `DT_FINI_ARRAY` | constructors / destructors to run |

`DT_SYMTAB` is a naming trap: despite the name it points at **`.dynsym`**, not
`.symtab` — the loader never sees `.symtab` at all.

A `DT_NEEDED` value is an offset into `.dynstr`, so the entry is just a library
*name*, not a path. At load time `ld.so` reads `PT_DYNAMIC`, collects every
`DT_NEEDED`, and resolves each name in order: `DT_RUNPATH`, then
`LD_LIBRARY_PATH`, then the `/etc/ld.so.cache` index, then the default
directories. Each library found is loaded and its own `DT_NEEDED` list processed
in turn, breadth-first, until the transitive closure is loaded; only then are
relocations resolved.

Every entry therefore costs an `open`, an `mmap`, a relocation pass, and one
more object in the symbol search scope for every later lookup. `readelf -d`
shows one object's **direct** entries; `ldd` shows the whole transitive closure.

This is what `-Wl,--as-needed` addresses. Without it, every `-l` on the command
line produces a `DT_NEEDED` whether or not a single symbol from it is
referenced. On the FLTK build the link line passes **40** `-l` arguments (31
distinct), and the two build types differ only in this flag:

```
Tiny    (--as-needed):  12 DT_NEEDED entries
Release (no --as-needed): 32 DT_NEEDED entries
```

The 12 that survive are the libraries actually referenced:

> libc++abi, libX11, libXinerama, libXcursor, libXrender, libXfixes, libXft,
> libfontconfig, libm, libasound, libgcc_s, libc

---

# Part 1 — The flags

## Cluster A — The overall size driver

`-Oz` puts LLVM into minimum-size mode. It does *not* imply or subsume any of
the other 14 flags; they remain independently useful on top of it.

### 1. `-Oz`

**What it does.** Selects the optimization level that prioritises code size
above all else. It is strictly more aggressive than `-Os`: clang disables any
`-Os` optimisation whose net effect is to grow the text section, even when that
optimisation would also have made the code faster. Internally both start from
the `-O2` pipeline; `-Oz` then sets the LLVM `MinSize` function attribute, which
feeds into inlining thresholds, loop unrolling decisions, vectorisation cost
models, and the SLP vectoriser.

**Why it shrinks the binary.** Small inlining threshold (≈5 vs ≈225 at `-O3`),
no loop unrolling, no auto-vectorisation, prefers calling helpers over inlining
them, prefers shorter instructions even when slightly slower.

**Trade-offs.** Measurable runtime cost on hot loops. Acceptable for a program
that is overwhelmingly idle.

**Sources.**
- Clang Command Line Reference, `-O<n>` section:
  <https://clang.llvm.org/docs/CommandGuide/clang.html#cmdoption-O0>
- LLVM `MinSize`/`OptimizeForSize` attributes:
  <https://llvm.org/docs/LangRef.html#function-attributes>
- Discussion of the `-Os` vs `-Oz` split in LLVM:
  <https://reviews.llvm.org/D69277>

---

## Cluster B — Dead-code & dead-data elimination

The largest cluster, and the one with the most internal overlap. These flags all
reduce the binary by removing code or data that is never used, in three layers:

1. **LTO (`-flto=thin`)** — at the LLVM IR level, across the whole program,
   *before* native code is generated. The most powerful layer, but it applies
   only to translation units actually compiled with LTO; pre-built system
   libraries are opaque to it.
2. **Sections + `--gc-sections`** — the same job at the ELF section level,
   *after* native code is generated. Necessary for system libraries, and a
   safety net even for LTO'd code.
3. **`--icf=all` and `-fmerge-all-constants`** — *merge* sections (or constants)
   that are bit-identical, rather than only dropping unreferenced ones.

The layers look redundant on paper but each catches things the others miss.

### 2. `-flto=thin`

**What it does.** Emits LLVM bitcode instead of (or alongside) native object
code and defers final code generation until link time, where the linker hands
the bitcode back to LLVM for whole-program analysis. *Thin* LTO splits that
analysis into a fast serial summary pass plus parallel per-module backends, so
it scales without the memory cost of monolithic ("full") LTO.

**Why it shrinks the binary.** Cross-translation-unit inlining lets the
optimiser delete dead code that previously had to survive as an exported symbol;
constant propagation across TUs eliminates unreachable branches; global
dead-code elimination can drop entire functions and globals.

**Overlap.** LTO already does, at the IR level, much of what
`-ffunction-sections` + `-fdata-sections` + `--gc-sections` do at the ELF level
(function-granularity dead-code elimination, DCE) and what
`-fmerge-all-constants` does for `.rodata` blobs (cross-TU constant merging).
The section-based flags still pull weight because **system libraries are not
LTO'd** — FLTK, libc++, the Advanced Linux Sound Architecture (ALSA) and the X
Window System (X11) all arrive as opaque `.o`/`.a` files, and only the linker's
section-level GC can prune them.

**Trade-offs.** Longer link times, requires a compatible linker (LLD, or gold
with the LLVMgold plugin), and debug info is harder to follow.

**Sources.**
- ThinLTO design doc: <https://clang.llvm.org/docs/ThinLTO.html>
- "ThinLTO: Scalable and Incremental LTO" — Tobias Edler von Koch, Teresa
  Johnson et al. (LLVM Developers' Meeting 2015):
  <https://llvm.org/devmtg/2015-04/slides/ThinLTO_EuroLLVM2015.pdf>
- Linker bitcode handling: <https://lld.llvm.org/index.html>

---

### 3. `-ffunction-sections`

**What it does.** Emits each function into its own ELF section
(`.text.<mangled_name>`) instead of one large `.text`.

**Why it shrinks the binary.** Sections are the granularity the linker uses for
dead-code elimination (`--gc-sections`). One function per section lets the
linker drop unreferenced functions individually instead of keeping an entire
object file.

**Overlap.** Pairs with `-fdata-sections` and `--gc-sections` — none of the
three is useful without the other two.

**Trade-offs.** Larger object files on disk (more section headers and relocation
entries) and slightly slower links. Both are dwarfed by the final size win.

**Sources.**
- GNU Compiler Collection (GCC) docs `-ffunction-sections`:
  <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>
- GNU `ld` manual on `--gc-sections`:
  <https://sourceware.org/binutils/docs/ld/Options.html>

---

### 4. `-fdata-sections`

**What it does.** As `-ffunction-sections`, but for data: each global / static
variable goes into its own `.data.*` / `.bss.*` / `.rodata.*` section.

**Why it shrinks the binary.** Lets `--gc-sections` discard unused globals and
string literals individually.

**Overlap.** The data-side counterpart to `-ffunction-sections`. Both exist only
because `--gc-sections` works at section granularity; they have no effect alone.

**Trade-offs.** More relocations, slower link.

**Sources.**
- GCC docs `-fdata-sections`:
  <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>

---

### 5. `-Wl,--gc-sections`

**What it does.** Garbage collection at section granularity. The linker builds a
reachability graph rooted at the entry point (plus any `KEEP`'d sections from
the linker script) and discards every input section not reachable.

**Why it shrinks the binary.** Combined with `-ffunction-sections` and
`-fdata-sections`, this is the mechanism by which unused functions, strings and
globals leave the binary. On a C++ project it also drops unused virtual
functions whose vtables ended up dead.

**Overlap.** The consumer for flags #3 and #4 — they exist for it. Also overlaps
with `-flto=thin`, which drops dead IR-level functions before they reach the
linker; both are kept because LTO cannot see into pre-built system libraries and
`--gc-sections` can.

**Trade-offs.** Things referenced only by name (via `dlsym`, or by the dynamic
linker) need explicit `KEEP` directives or `__attribute__((used))` to survive.
Not an issue for self-contained executables.

**Sources.**
- GNU `ld` manual, `--gc-sections`:
  <https://sourceware.org/binutils/docs/ld/Options.html>
- LLD ELF docs: <https://lld.llvm.org/ELF/start.html>

---

### 6. `-Wl,--icf=all`

**What it does.** Identical code folding (ICF). After input sections are laid
out, the linker hashes the body of each function-bearing section and merges
sections that are byte-identical (modulo relocations) into a single copy,
redirecting all references. `=all` folds anything that matches; `=safe` (the
default elsewhere) folds only sections the compiler tagged as
address-insensitive.

**Why it shrinks the binary.** Template instantiations with different type
parameters but identical generated code collapse into one, as do trivial
getters, thunks and many small helpers.

**Overlap.** Does **not** overlap with LTO. LTO inlines and DCEs but does not
fold two byte-identical *generated* functions into one; that happens only
post-codegen, at the linker. ICF is the natural companion to `--gc-sections`: GC
drops unreferenced sections, ICF merges duplicates among the survivors.

**Trade-offs.** `=all` breaks the C/C++ guarantee that two distinct functions
have distinct addresses, so comparing function pointers can silently equate
unrelated functions. Requires LLD or gold — not supported by classic GNU `bfd`
ld.

**Sources.**
- Sriraman Tallam et al., "Safe ICF: Pointer Safe and Unwinding Aware Identical
  Code Folding in Gold":
  <https://research.google/pubs/safe-icf-pointer-safe-and-unwinding-aware-identical-code-folding-in-the-gold-linker/>
- LLD ICF documentation: <https://lld.llvm.org/ELF/start.html>

---

### 7. `-fmerge-all-constants`

**What it does.** Allows the compiler/linker to merge *any* constants with
identical bit patterns into a single storage location — including arrays and
aggregates, not just the strings and floats covered by the default
`-fmerge-constants`.

**Why it shrinks the binary.** Duplicate string literals, repeated lookup tables
and identical `constexpr` blobs collapse to one copy in `.rodata`.

**Overlap.** The data-side analogue of `--icf=all`: same idea (merge
byte-identical things), different domain (read-only data instead of code). Also
partially overlaps with `-flto=thin`, which already deduplicates constants
across TUs at the IR level. The marginal win on top of LTO is the merging of
*aggregate* constants (arrays, structs) that LTO will not always combine.

**Trade-offs.** **Not standard-conforming C/C++**: the standard guarantees
distinct addresses for distinct objects with the same value, and this flag
breaks that. Nothing in this project compares addresses of constants.

**Sources.**
- GCC docs `-fmerge-all-constants`:
  <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>
- ISO C++ [basic.compound]/3 on object identity.

---

## Cluster C — Disabling C++ runtime features

Both flags remove application binary interface (ABI) machinery the C++ runtime
would otherwise generate. Both are independent of everything in Cluster B, and
neither overlaps with the other — they target different ABI surfaces.

### 8. `-fno-exceptions`

**What it does.** Disables C++ exception support in the front-end: `throw` and
`try`/`catch` become errors, and the compiler stops emitting the landing-pad
machinery (`.gcc_except_table`, personality routines, cleanup records) that the
unwinder needs to run destructors after a throw.

**Why it shrinks the binary.** Removes per-function exception tables, the
language personality routine reference (`__gxx_personality_v0`) and its
transitive pull on `libstdc++`/`libsupc++`. Measured here: removes
`.gcc_except_table` entirely.

**Overlap.** Does **not** subsume `-fno-unwind-tables` (#10). Unwind tables are
emitted for stack walking, signal handling and backtraces even when exceptions
are off — `-fno-exceptions` removes the *landing pads*, not `.eh_frame` itself.

**Trade-offs.** No library that propagates exceptions across its application
programming interface (API) can be used — parts of the Standard Template Library
(STL), for example. Code calling `new` must be ready to receive a null pointer,
or must use `operator new(std::nothrow)`.

**Sources.**
- Itanium C++ ABI, §15 "Exception Handling":
  <https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html>
- GCC docs `-fexceptions`:
  <https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html>
- Ben Craig, "Removing Exceptions Cost From C++" (P0709-adjacent):
  <https://wg21.link/p0709>

---

### 9. `-fno-rtti`

**What it does.** Suppresses generation of `type_info` objects, polymorphic
class type-info tables, and the helpers used by `dynamic_cast<>` and `typeid()`.

**Why it shrinks the binary.** Each polymorphic class normally requires a
`type_info` object plus a name string in `.rodata`. With many small polymorphic
classes or templates this accumulates. Removing run-time type information (RTTI)
also breaks a retention edge in the linker's dead-code elimination graph — once
vtables no longer reference `type_info`, the linker can drop entire chains.

**Overlap.** None with `-fno-exceptions` — they target different ABI artefacts
(`type_info` vs landing pads and the personality routine).

**Trade-offs.** No `dynamic_cast` (except the trivial up-cast), no `typeid`, and
any third-party library relying on RTTI must be compiled the same way or
isolated.

**Sources.**
- Itanium C++ ABI, §2.9 "Type Information":
  <https://itanium-cxx-abi.github.io/cxx-abi/abi.html#rtti>
- Clang docs on RTTI controls:
  <https://clang.llvm.org/docs/UsersManual.html#controlling-code-generation>

---

## Cluster D — Unwind metadata

Background: "Unwind tables and exception-handling data" in Part 0 covers what
`.eh_frame`, `.eh_frame_hdr` and `.gcc_except_table` contain and why they exist.

On x86-64 Linux the processor-specific ABI (psABI) requires asynchronous unwind
tables, so clang's driver defaults to `-fasynchronous-unwind-tables`, and that
default **implies** `-funwind-tables`. Consequently:

- `-fno-unwind-tables` does not switch off the asynchronous default, so the
  async setting turns the tables straight back on. **The flag is a no-op on this
  target.**
- `-fno-asynchronous-unwind-tables` clears the default, and the synchronous
  tables go with it.

Measured (clang 22, x86-64, `src/main_pro_plus.cpp`, all on top of `-Oz`; sizes
from `readelf -SW`):

| flags | `.eh_frame` | `.eh_frame_hdr` | `.gcc_except_table` |
|-------|------------:|----------------:|---------------------|
| *(neither)* | 0x3a0 (928) | 0xd4 | 0x70 |
| `-fno-unwind-tables` | **0x3a0 (unchanged)** | 0xd4 | 0x70 |
| `-fno-asynchronous-unwind-tables` | 0x2c0 (704) | 0x84 | 0x70 |
| both | 0x2c0 (704) | 0x84 | 0x70 |
| `-fno-exceptions` alone | 0x358 (856) | 0xcc | **gone** |
| `-fno-exceptions -fno-async…` | 0x358 (856) | 0xcc | **gone** |

Two things the table shows that are not obvious:

- `-fno-unwind-tables` changes nothing in any combination.
- The two flags do **not** compose additively on `.eh_frame`. Once
  `-fno-exceptions` is in effect, adding `-fno-asynchronous-unwind-tables` has
  no further effect on `.eh_frame` (856 either way), and the combination is
  *larger* than `-fno-asynchronous-unwind-tables` alone (704). `-fno-exceptions`
  changes codegen in ways that alter which frames need unwind information, so
  the sections cannot be reasoned about independently. Whole-binary size is the
  reliable measure; see Part 3.

Keeping `-fno-unwind-tables` in the build is defensible as documentation of
intent and as a guard against target-default changes on other architectures (on
ARM the defaults differ), but on x86-64 Linux it buys zero bytes.

### 10. `-fno-unwind-tables`

**What it does.** Intended to stop emitting the `.eh_frame` (DWARF call-frame
information) tables an unwinder uses to walk the stack when an exception
propagates or a debugger / profiler requests a backtrace.

**Why it would shrink the binary.** `.eh_frame` is always loaded at runtime (it
lives in a `PT_LOAD` segment), so it counts toward on-disk size, resident set
size (RSS) and load time. On a release C++ binary it is frequently the
second-largest read-only section after `.rodata`.

**Overlap.** On x86-64 Linux this flag does nothing on its own. It does *not*
subsume `-fno-asynchronous-unwind-tables` (#11); the reverse holds. Measured:
removing it from the full tiny set changes the binary by 0 bytes on both
targets.

**Trade-offs.** If it took effect: no usable `backtrace()` from the C runtime,
and profilers relying on frame unwinding (`perf --call-graph=dwarf`) fall back
to frame-pointer walking.

**Sources.**
- Linux Standard Base, "Exception Frames":
  <https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html>
- Ian Lance Taylor, "The .eh_frame section": <https://www.airs.com/blog/archives/460>
- GCC docs `-funwind-tables`:
  <https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html>

---

### 11. `-fno-asynchronous-unwind-tables`

**What it does.** Removes the precise per-instruction unwind tables the Itanium
ABI calls "asynchronous" — those needed when control can leave a function at any
instruction boundary (on a signal, a debugger trap, mid-prologue). On x86-64
Linux the ABI mandates them, so they are on by default even with
`-fno-exceptions`.

**Why it shrinks the binary.** Without async tables the compiler emits much
sparser call-frame-information (CFI) directives — only where the unwind state
changes for synchronous reasons.

**Overlap.** Async tables are a strict superset of sync tables, and on x86-64
Linux the default is `-fasynchronous-unwind-tables` (which implies
`-funwind-tables`). This is therefore the only flag of the pair that can clear
the default; `-fno-unwind-tables` (#10) alone is a no-op.

**Trade-offs.** Signal handlers and debuggers cannot reliably reconstruct the
stack from arbitrary instruction pointers. Acceptable for a hosted Linux
application that does not deliver asynchronous signals.

**Sources.**
- System V x86-64 psABI §3.7 (unwind tables required by default):
  <https://gitlab.com/x86-psABIs/x86-64-ABI>
- GCC docs `-fasynchronous-unwind-tables`:
  <https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html>

---

## Cluster E — Symbol surfaces

Background: "Symbol tables and the dynamic section" in Part 0 covers the layout
of `.symtab`, `.dynsym` and `.dynamic`, and what `DT_NEEDED` does at load time.

These three flags all reduce some kind of "symbol" in the output and look
interchangeable. They are not — each touches a different table:

| Flag | Affects | Loaded at runtime? |
|------|---------|--------------------|
| `-fvisibility=hidden` | `.dynsym` / `.dynstr` (dynamic exports) | Yes |
| `-Wl,--as-needed`     | `DT_NEEDED` entries in `.dynamic` | Yes |
| `-Wl,-s`              | `.symtab` / `.strtab` (static symbols) | No (on-disk only) |

The first two also affect runtime behaviour (linking semantics, dynamic loader
work); the third has no effect from the loader's point of view.

### 12. `-fvisibility=hidden`

**What it does.** Changes the default ELF symbol visibility from `default`
(exported, interposable) to `hidden`. Hidden symbols are not placed in the
dynamic symbol table of the resulting shared object or executable, and
cross-dynamic-shared-object (DSO) calls to them are impossible.

**Why it shrinks the binary.** (a) Fewer entries in `.dynsym` / `.dynstr`.
(b) The optimiser can inline hidden functions, devirtualise calls and delete
unused ones, because nothing outside the module can take their address.
(c) Calls become direct program-counter-relative (PC-relative) branches instead
of going through the procedure linkage table / global offset table (PLT/GOT).

**Overlap.** Frequently confused with `-Wl,-s` (#14). They affect different
symbol tables: `.dynsym` (runtime, loaded) vs `.symtab` (debug, on-disk).

**Trade-offs.** Anything intended to be exposed from a shared library must be
annotated explicitly (`__attribute__((visibility("default")))` or the
`[[gnu::visibility]]` attribute). Harmless for a self-contained executable.

**Sources.**
- GCC visibility wiki page (canonical, applies to clang):
  <https://gcc.gnu.org/wiki/Visibility>
- Ulrich Drepper, "How To Write Shared Libraries", §2.2:
  <https://www.akkadia.org/drepper/dsohowto.pdf>
- Clang docs on `-fvisibility=`:
  <https://clang.llvm.org/docs/CommandGuide/clang.html>

---

### 13. `-Wl,--as-needed`

**What it does.** Emits a `DT_NEEDED` entry for a shared library only if at
least one symbol from that library is actually referenced. Without it, every
`-l<foo>` on the command line ends up in the dynamic dependency list whether
used or not.

**Why it shrinks the binary.** A smaller `.dynamic` / `.dynstr` section, and at
runtime fewer libraries mapped, fewer symbol lookups, faster startup, smaller
RSS.

**Overlap.** None with the rest of Cluster E: `.dynamic` / `DT_NEEDED` is a
third table, distinct from both `.dynsym` and `.symtab`.

**Trade-offs.** Link order matters: if `libA.so` pulls in `libB.so`'s symbols
transitively, they must be ordered correctly on the command line.

**Sources.**
- GNU `ld` manual, `--as-needed`:
  <https://sourceware.org/binutils/docs/ld/Options.html>
- Drepper, "How To Write Shared Libraries", §2:
  <https://www.akkadia.org/drepper/dsohowto.pdf>

---

### 14. `-Wl,-s`

**What it does.** Strips the symbol table (`.symtab`) and string table
(`.strtab`) from the output. Equivalent to running `strip` on the finished
binary, but performed by the linker so the data is never written to disk.

**Why it shrinks the binary.** `.symtab` / `.strtab` are not loaded into memory
at runtime but do consume disk — often a double-digit percentage of a small C++
binary, dominated by mangled template names. On the FLTK build here it accounts
for roughly a quarter of a megabyte.

**Overlap.** Often conflated with `-fvisibility=hidden` (#12). `-s` strips the
*static* symbol table used by debuggers and profilers, which is on-disk only;
visibility controls the *dynamic* symbol table used by the runtime loader.

**Trade-offs.** No symbols for debuggers or profilers. Keep an unstripped copy,
or split debug info with `objcopy --only-keep-debug`, for crash investigation.

**Sources.**
- GNU `ld` manual, `-s`: <https://sourceware.org/binutils/docs/ld/Options.html>
- ELF spec on `.symtab` / `.strtab`:
  <https://refspecs.linuxfoundation.org/elf/elf.pdf>

---

## Cluster F — Assembler

The pure-asm `_ultra` build is the only target that uses the Netwide Assembler
(NASM). It operates on a different toolchain path, so nothing here overlaps with
the flags above.

### 15. `nasm -Ox`

**What it does.** Enables NASM's multi-pass optimiser. NASM normally makes a
single forward pass and conservatively picks the longest encoding when a forward
reference could be either short or long (`jmp short` vs `jmp near`). `-Ox` lets
NASM iterate until a fixed point, picking the shortest encoding for every
instruction.

**Why it shrinks the binary.** Short jumps are 2 bytes vs 5; many
immediate-loads have multiple legal encodings; effective addresses with zero
displacement can drop the displacement byte. On a few hundred lines of assembly
this typically saves one to two percent.

**Trade-offs.** Multi-pass assembly is slower (negligible for a small file).
Hand-rolled encodings that assume a specific size must use the explicit form
(`jmp near`).

**Sources.**
- NASM manual, §2.1.22 "Multipass Optimization":
  <https://www.nasm.us/doc/nasmdoc2.html#section-2.1.22>
- Intel SDM Vol. 2 — instruction encoding reference.

---

### Standalone link of the asm target — `ld -nostdlib -static -s`

Not one of the 15, but it is what makes the `_ultra` binary small.

- **`-nostdlib`** — do not link the standard C runtime (`crt1.o`, `crti.o`,
  `crtn.o`, libc). The program provides its own `_start` and talks to the kernel
  directly via `syscall`. The C runtime alone contributes ~10 KB of statically
  linked code.
  Source: <https://sourceware.org/binutils/docs/ld/Options.html>
- **`-static`** — produce a statically linked executable with no `PT_INTERP` /
  dynamic linker, no `.dynamic` section, no PLT/GOT. Same source.
- **`-s`** — strip, as flag #14.

The floor for an ELF executable on Linux is around 45 bytes with hand-written
headers (Brian Raiter, "A Whirlwind Tutorial on Creating Really Teensy ELF
Executables for Linux",
<https://www.muppetlabs.com/~breadbox/software/tiny/teensy.html>).

---

# Part 2 — Flags apply only to code you compile

`tiny_pomodoro` links `/usr/local/lib/libfltk.a`, a pre-built static archive
compiled separately with `-O3`, exceptions on, RTTI on and no size flags. The 15
flags in `CMakeLists.txt` are applied to exactly one file: `src/main.cpp`, ~350
lines. The archive — most of four megabytes, and the bulk of what fills the
binary — was compiled with none of them.

`tiny_pomodoro_src` builds the same `main.cpp`, but FLTK is compiled as part of
this build (`add_subdirectory`), so the flags reach the library's own sources.

## Result

Compiling FLTK with the same flags removes roughly two-thirds of the binary
versus linking the pre-built archive. Exact figures are in the slides.

| build | relative size |
|---|---:|
| pre-built FLTK, all tiny flags | — |
| FLTK from source, tiny flags **minus** `-flto=thin` | about half |
| FLTK from source, **all** tiny flags | about a third |

The gap between the last two rows is LTO's contribution; the rest comes from the
other flags simply reaching the library at all.

Two contributions, lopsided in favour of the first:

1. **The bulk — the flags now apply to FLTK's code.** `-Oz` instead of `-O3`,
   `-fno-exceptions`, `-fno-rtti`, `-ffunction-sections` + `--gc-sections` and
   `--icf=all` all now act on the library, not just on `main.cpp`. This requires
   no LTO.
2. **The remainder — LTO can see inside.** With `-flto=thin`, FLTK's object
   files are LLVM bitcode, so the optimiser inlines and eliminates dead code
   across the app/library boundary. Verify:
   `file build/tiny/fltk_src/src/CMakeFiles/fltk.dir/Fl_Window.cxx.o`
   → `LLVM IR bitcode`, where the pre-built archive's member is
   → `ELF 64-bit LSB relocatable`.

## Comparing the two in `release`

Building both ways with `-O3` and no size flags is intended as a control: if
recompiling the library were itself responsible for the win, it would show up
here too.

The comparison is only valid when both sides use the **same compiler and
standard library**. In the current setup they do not — the pre-built archive is
built by gcc against libstdc++, while the from-source build uses clang and
libc++ — so the release delta measures a compiler difference on top of the flag
difference and cannot be attributed to either alone. Rebuilding the pre-built
archive with clang and libc++, changing only the optimisation flags, would
restore it as a control.

## Standard-library coupling

A pre-built archive carries its own toolchain decisions — standard library, ABI,
optimisation level — and the consuming binary inherits them. Whether that
results in two C++ runtimes being linked into one process depends on how the
archive was built:

- FLTK 1.4 defaults to `FLTK_USE_STD=OFF`, so the archive references **zero
  `std::` symbols**. Nothing in it demands a C++ standard library, and the
  current binaries link only `libc++abi.so.1`.
- An archive built with `FLTK_USE_STD=ON`, or any library that exposes
  `std::` types across its API, does couple the consumer to its standard
  library. Mixing then produces either a duplicated runtime or link errors —
  undefined `std::__throw_length_error` / `_Rb_tree_*` symbols are the typical
  signature, because the two sides disagree about what `std::string` is.

Check which case applies with:

```
$ readelf -d build/tiny/tiny_pomodoro | grep -Ei 'c\+\+|stdc'
```

## Costs of compiling the library yourself

- **Build time.** FLTK takes minutes to compile; the pre-built archive links
  instantly.
- **`-fno-exceptions` / `-fno-rtti` on third-party code** is a genuine risk.
  FLTK 1.4 compiles cleanly with both; not every library will.
- **Ownership.** Upgrades, patches and platform quirks become your problem.

---

# Part 3 — Measured: what each flag is worth

**Method — leave-one-out (ablation).** Build with the full tiny flag set, then
rebuild removing exactly one flag, and record how much the binary grows. This
attributes savings without depending on the order flags are listed in. (Removing
`-Oz` falls back to `-O2`.)

Rankings below are from clang 22, 2026-07-19. Byte counts are in the slides.

### Terminal build (`main_pro_plus.cpp`) — ranked by cost of removing

1. **`-Oz`** — roughly half of all savings
2. **`-Wl,-s`** — roughly a quarter
3. `-fno-exceptions`
4. `-fno-asynchronous-unwind-tables`
5. `-flto=thin`
6. `-Wl,--gc-sections`
7. `-Wl,--as-needed` (marginal)
8. **Seven flags at exactly zero:** `-ffunction-sections`, `-fdata-sections`,
   `-fno-rtti`, `-fvisibility=hidden`, `-fno-unwind-tables`,
   `-fmerge-all-constants`, `-Wl,--icf=all`

### FLTK build (`main.cpp`) — ranked by cost of removing

1. **`-Wl,-s`** — roughly 90% of all savings
2. `-Wl,--icf=all`
3. `-Oz`
4. `-fno-exceptions`
5. `-Wl,--gc-sections`
6. `-fno-asynchronous-unwind-tables`
7. `-flto=thin`
8. `-Wl,--as-needed` (marginal)
9. **The same six flags at exactly zero** (the terminal list minus `--icf=all`,
   which does work here)

### Observations

1. **Which flag dominates depends on the target.** `-Wl,-s` overwhelms the FLTK
   build, where a large fraction of the file is mangled C++ names — data the
   kernel never loads. On the terminal build `-Oz` is larger, because the
   fallback when it is removed (`-O2`) inlines far more aggressively than the
   binary can absorb at this size.
2. **Six of the fourteen flags move zero bytes on both targets.** They are not
   useless in general: `-fno-rtti` needs polymorphism to bite,
   `-fmerge-all-constants` needs duplicate constants, `-ffunction-sections`
   matters only if `--gc-sections` has something to collect. This program does
   not exercise them.
3. **The same flag is worth very different amounts on different programs.**
   `--icf=all` is exactly zero on the terminal build but one of the largest wins
   on FLTK, whose templates and thunks give it duplicates to fold. `-Wl,-s` and
   `-Oz` likewise trade places between the two targets.
4. **`-ffunction-sections` / `-fdata-sections` show zero because LTO gets there
   first.** Removing LTO *and* `--gc-sections` together changes the picture.
   Leave-one-out systematically under-credits redundant flags: when two flags
   catch the same bytes, dropping either alone appears free. This is the main
   weakness of the method.

---

## Background — FLTK

**What it is.** "Fast Light Toolkit" — a C++ graphical user interface (GUI)
library originally written for Silicon Graphics' (SGI) IRIX in the mid-1990s.
The "fast and light" description was accurate against the alternative of the
day (Motif); on modern Linux it sits on top of a large stack of platform glue.

**Why the FLTK build is most of a megabyte.** Not dynamic-linking overhead —
FLTK's own machine code is copied into the executable. `libfltk.a` is a static
archive, so the linker pulls in every archive member that anything references,
and those members reference further members: widget base classes, the event
loop, the drawing layer, font handling, image decoders. That transitive closure
is the bulk of the binary.

**What the platform libraries actually cost.** The link line passes **38**
libraries; the finished binary records only **13** `DT_NEEDED` entries.
`--as-needed` drops all of these:

> gtk-3, gdk-3, pango-1.0, pangocairo-1.0, cairo, cairo-gobject, atk-1.0,
> gdk_pixbuf-2.0, gio-2.0, gobject-2.0, glib-2.0, harfbuzz, wayland-client,
> wayland-cursor, xkbcommon, dbus-1, Xext, z, dl, pthread

The GTK/Wayland/DBus stack is therefore not a dependency of `tiny_pomodoro` at
all — it is on the command line because CMake's `pkg_check_modules` puts it
there, and the linker discards it. Verify with `ldd build/tiny/tiny_pomodoro`.

**Why the optimisations behave differently here.** `-flto=thin` cannot see into
FLTK — not because of dynamic linking, but because the archive holds native
object code rather than LLVM bitcode. `--gc-sections` and `--icf=all` *can*
reach inside a static archive; ICF earns its place here, being one of the
largest regressions when removed from the FLTK build versus exactly 0 bytes on
the terminal build. `--gc-sections` adds comparatively little on top, because
the linker's ordinary archive-member selection — pull only the `.o` members
actually needed — has already done most of that work.

**Trade-offs.** FLTK is a reasonable choice for a native-looking GUI with file
dialogs, accessibility and clipboard support. It is a poor choice when binary
size is the goal. The optimised figure is already heavily reduced; unstripped it
grows by roughly a quarter of a megabyte of mangled symbol names.

**Sources.**
- FLTK home page: <https://www.fltk.org/>
- FLTK introduction (lists the dependency stack):
  <https://www.fltk.org/doc-1.3/intro.html>

---

## Background — X11 / Xlib

**What it is.** The original Linux display server protocol, dating to 1984. The
X server runs as a separate process; `libX11` (Xlib) is the client library that
speaks the X protocol over a Unix-domain socket.

**Why it is much smaller than FLTK.** Xlib exposes primitives, not widgets: open
a connection, create a window, get a graphics context, draw lines / rectangles /
text, read keyboard and mouse events from `XNextEvent`. There is no widget tree,
no theming, no text shaping, no clipboard abstraction, no file dialogs. The
application draws every button by hand — which is what `tiny_pomodoro_pro` does
— but the only libraries linked are `libX11.so` plus a few small extensions
(`Xext`, `Xinerama`, `Xcursor`, `Xrender`, `Xfixes`, `fontconfig`). The
`tiny_pomodoro_pro` tiny build is about **17 KB**, roughly 50× smaller than the
FLTK build for the same feature set.

**Where it sits.** Xlib is effectively the floor for a Linux GUI: anything above
it (FLTK, Qt, GTK) wraps it, and anything below it is not a GUI (terminal,
framebuffer).

**Wayland.** The modern replacement for X11. For size purposes it is not a win —
`libwayland-client` is small, but the application must implement window
decorations, input dispatch and surface buffer management itself, and most
desktops still run XWayland for X clients.

**Sources.**
- X protocol reference (canonical Xlib spec):
  <https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html>
- *Xlib Programming Manual* — Adrian Nye, O'Reilly 1992.
- `man X`.

---

## Further reading

- Brian Raiter, *A Whirlwind Tutorial on Creating Really Teensy ELF Executables
  for Linux*: <https://www.muppetlabs.com/~breadbox/software/tiny/teensy.html>
- Ulrich Drepper, *How To Write Shared Libraries* — visibility, `--as-needed`,
  dynamic-section costs: <https://www.akkadia.org/drepper/dsohowto.pdf>
- LLD documentation — `--icf`, LTO link flow, section GC: <https://lld.llvm.org/>
- Bloaty McBloatface — per-symbol size attribution:
  <https://github.com/google/bloaty>
- Matt Godbolt, *Compiler Explorer* — comparing `-Oz` and `-O3` output:
  <https://godbolt.org/>
- ELF specification, "Program Header":
  <https://refspecs.linuxfoundation.org/elf/elf.pdf>
- System V x86-64 psABI, program-loading rules:
  <https://gitlab.com/x86-psABIs/x86-64-ABI>
- `man elf(5)`.
