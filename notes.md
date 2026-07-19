# Size Optimization Flags — Talk Notes

Reference notes for each of the 15 size-reduction flags used by `tiny_pomodoro`.
Each entry lists *what it does*, *why it shrinks the binary*, *what you give
up*, and *where to read more*.

Flags are grouped by **what they do**, not by which tool consumes them, so
flags whose effects overlap sit next to each other and the overlap can be
discussed in one place. Each cluster starts with a short note on how the
flags inside it interact.

**Two sections were factually wrong and are now corrected** — Cluster D (the
unwind pair: I had the two flags backwards) and the Fast Light Toolkit (FLTK)
background (it is
linked *statically*, not as a `.so`). Both are marked with a CORRECTED note.
Read those before rehearsing.

---

# Part 0 — Background you need before the flags make sense

## How a C++ file becomes an executable

The single most useful framing for the talk: **every one of the 15 flags acts
at exactly one stage of this pipeline.** Most confusion about them
(*"isn't `-fvisibility=hidden` the same as stripping?"*) dissolves the moment
you know which stage a flag fires at.

| # | Stage | Tool | Input → Output |
|---|-------|------|----------------|
| 1 | Preprocess | `clang -E` | `.cpp` + `#include`s → one big translation unit (TU) |
| 2 | Parse / semantic analysis | clang front-end | TU → abstract syntax tree (AST) |
| 3 | Intermediate-representation generation | clang front-end | AST → **LLVM intermediate representation (IR)** |
| 4 | Optimise | LLVM middle-end | IR → better IR (target-independent) |
| 5 | Code generation | LLVM back-end | IR → **machine code**, packed into an `.o` |
| 6 | Link | LLD (the LLVM linker) | many `.o` + libraries → one executable |
| 7 | Load | Linux kernel + `ld.so` | executable → a running process |

Things worth knowing that are easy to get wrong on stage:

- **The AST is not the IR.** The AST is a faithful tree of the C++ you wrote
  (it still knows about templates, classes, `for` loops). LLVM IR is a
  low-level, typed, static-single-assignment (SSA) form instruction language
  with no C++ in it at all.
  `-fno-exceptions` and `-fno-rtti` act at the *front-end* — they change what
  IR is even generated. By stage 4 the information is simply not there to
  remove.
- **Stage 4 is target-independent, stage 5 is not.** `-Oz` mostly steers
  stage 4 (see below); instruction-encoding choices happen in stage 5.
- **An `.o` file is a bag of sections, plus a symbol table, plus
  relocations.** It is not yet a program: addresses are unresolved
  placeholders. Relocations are the "fill this in later" notes the linker
  reads.
- **`.o` and the final executable are both Executable and Linkable Format
  (ELF)**, but different *types*:
  `ET_REL` (relocatable) vs `ET_EXEC`/`ET_DYN` (executable). `readelf -h`
  shows this on the `Type:` line.

### What `-Oz` actually changes (stage 4)

`-Os` and `-Oz` both start from the **`-O2` pass pipeline** — this surprises
people who assume `-Os` is "`-O0` but smaller". What `-Oz` does is set the
LLVM **`minsize`** function attribute (`-Os` sets `optsize`), and that
attribute is consulted by the cost models of individual passes:

- **inlining threshold**: roughly **5** at `-Oz`, **50** at `-Os`, **225** at
  `-O2`/`-O3`. This is the single biggest lever.
- **loop unrolling**: off.
- **loop vectorisation / superword-level-parallelism (SLP) vectorisation**: off.
- prefers a `call` to a shared helper over inlining it; prefers shorter
  instruction encodings even when marginally slower.

If someone asks "what does `-Oz` cost me at runtime": on a pomodoro timer,
nothing measurable — the program sleeps ~99.99% of its life. On a hot numeric
loop it can be a large multiple. Be honest that this is a *domain-appropriate*
choice, not a universal one.

### The plot twist: with `-flto=thin`, your compiler does not compile

This is the best single demo in the whole talk, and it takes one command:

```
$ file build/tiny/CMakeFiles/tiny_pomodoro_pro_plus.dir/src/main_pro_plus.cpp.o
… LLVM IR bitcode
$ file build/release/CMakeFiles/tiny_pomodoro_pro_plus.dir/src/main_pro_plus.cpp.o
… ELF 64-bit LSB relocatable, x86-64
```

Under `-flto=thin` the "object file" contains **no machine code at all** — it
is serialised LLVM IR. Stages 4 and 5 have been *moved into the linker*. That
is why link-time optimization (LTO) can do things `--gc-sections` cannot (it optimises across
translation units, before codegen), and equally why it is powerless against a
pre-built static archive like `libfltk.a` (native code, not bitcode).

If you want a second beat: `nm` on the bitcode `.o` fails or reports nothing
useful, while `llvm-nm` reads it fine — because it is not an ELF file.

---

## The ELF file format

The executable is not a blob. It has a rigid structure, and **the single most
important idea in the whole talk lives here**:

> **The linker thinks in _sections_. The kernel thinks in _segments_.
> The flags all operate on sections. The file size is decided by segments.**

That gap is exactly why the flags stop paying off at ~8 KB.

### The four regions of an ELF file

1. **ELF header** (64 bytes). Magic `\x7fELF`, class (64-bit), type
   (`ET_EXEC`/`ET_DYN`), machine (x86-64), entry point, and the offsets of the
   next two tables. `readelf -h`.
2. **Program header table** (the *segment* view). One entry per segment.
   **This is what the kernel reads.** `readelf -lW`.
3. **The content** — the sections themselves (`.text`, `.rodata`, `.data`,
   `.bss`, `.symtab`, …).
4. **Section header table** (the *section* view). **The kernel never reads
   this.** It exists for the linker, the debugger, `objdump`, `readelf`.
   You can literally delete it and the program still runs.

### Sections vs segments — say this slowly

The **same bytes** are described twice, by two tables, for two different
audiences:

| | Sections | Segments |
|---|---|---|
| Described by | section header table | program header table |
| Consumer | linker, debugger, `objdump` | **the kernel's ELF loader** |
| Typical count | ~30 | **3–4** |
| Granularity | one per kind of content | one per **memory protection** |
| Needed to run? | **no** | **yes** |

`readelf -lW` prints the mapping between them at the bottom
("Section to Segment mapping"). For `tiny_pomodoro_2`:

```
LOAD  Offset 0x000000  FileSiz 0x0000e8  Flg R    Align 0x1000   ← ELF+prog headers
LOAD  Offset 0x001000  FileSiz 0x00007a  Flg R E  Align 0x1000   ← .text
LOAD  Offset 0x002000  FileSiz 0x000001  Flg R    Align 0x1000   ← .rodata
```

### Why segments must live on separate pages — the heart of the floor slide

The kernel loads a program by **`mmap()`ing each `PT_LOAD` segment** into the
process's address space. Two facts collide:

1. `mmap()` works at **page granularity** — 4 KiB on x86-64.
2. Memory protection (`r`/`w`/`x`) is a property of a **page**, set in the
   page table. There is no finer unit. The hardware memory-management unit
   (MMU) has no way to say
   "the first 122 bytes of this page are executable and the rest is not".

So content with **different protection cannot share a page**. `.text` needs
`R+X`; `.rodata` needs `R` (and must *not* be `X`, or you hand an attacker a
code-injection surface); `.data`/`.bss` need `R+W` (and must not be `X`).
Three protections ⇒ three segments ⇒ **three pages minimum**, no matter how
few bytes are actually in them.

That is why `tiny_pomodoro_2` — **122 bytes of code** — is **8,480 bytes on
disk**. Its three pages are essentially empty:

| Page | Real content | Bytes used | Bytes wasted |
|------|--------------|-----------:|-------------:|
| 1 (`R`) | ELF header + 3 program headers | 232 | 3,864 |
| 2 (`R+X`) | `.text` | 122 | 3,974 |
| 3 (`R`) | `.rodata` — a single `0x07` BEL (bell) byte | 1 | ~31 |
| — | section headers + `.shstrtab` | ~64 | — |

**~419 bytes of content in an 8,480-byte file. About 95% of it is padding.**
And the file is *not* 12,288 bytes only because the last page is truncated
rather than padded out.

The empirical confirmation, which is the line to land the talk on:
`_ultra` has **3,046 bytes** of `.text`; `_2` has **122**. That is **25×
more code — and only a 6% bigger file** (8,992 vs 8,480 B). Once your content
fits inside a page, more code is *free* until you spill into the next one.

### The two orthogonal axes — kills the naive "binary size" mental model

Two sections from this very project, sitting at opposite corners:

| | On disk | In memory at runtime |
|---|---|---|
| **`.bss`** (`_ultra`'s 1 MiB thread stack) | **0 bytes** | **1 MiB** |
| **`.symtab`** (`tiny_pomodoro`, unstripped) | **~244 KB** | **0 bytes** |

`.bss` is "zero-initialised data": the ELF records only *how big* it is
(`MemSiz` > `FileSiz` in the program header) and the kernel hands you zeroed
pages. It costs nothing in the file. `.symtab` is the reverse: it is never in
a `PT_LOAD` segment, so the loader never maps it, but every byte of it sits on
your disk.

**"Binary size" is not one number**, and the flags do not all act on the same
one. `-Wl,-s` is a *disk* optimisation with zero runtime effect.
`-fvisibility=hidden` is a *runtime* optimisation (fewer dynamic relocations,
faster startup) that happens to shrink the file a little too.

### How to go below the page floor (the "different talk" slide)

1. **Merge the segments.** Put `.rodata` in the same `R+X` segment as `.text`
   → two pages, then one. Needs a custom linker script; no standard flag
   exposes it.
2. **Overlap the headers.** The ELF header has padding (`e_ident[EI_PAD]`) and
   fields the kernel ignores; the program header table can be hidden inside
   them.
3. **Hand-roll the ELF.** Brian Raiter's *Teensy ELF* reaches **45 bytes** by
   abandoning sections entirely and using header padding as instructions.
   Unstrippable, un-debuggable, kernel-version-fragile — but it runs.

**The takeaway line.** *"The flags get you to the page floor. Below it you are
not optimising any more — you are hand-crafting ELF, and that is a different
talk."*

### Tools to have in your shell on stage

| Command | Shows |
|---------|-------|
| `readelf -h <bin>` | ELF header — type, entry point |
| `readelf -lW <bin>` | **segments** + section→segment mapping (`Align 0x1000` is the smoking gun) |
| `readelf -SW <bin>` | **sections** — names, sizes, offsets |
| `readelf -d <bin>` | `.dynamic` — the `DT_NEEDED` library list |
| `size <bin>` | `.text` / `.data` / `.bss` one-liner — best live demo |
| `nm -C <bin>` | symbols (fails on a stripped binary — that *is* the demo) |
| `file <x>.o` | reveals LLVM bitcode vs ELF under `-flto` |
| `ldd <bin>` | what actually gets loaded (vs what you passed to `-l`) |
| `bloaty <bin>` | per-section/per-symbol byte attribution |

---

# Part 1 — The flags

## Cluster A — The overall size driver

`-Oz` is the umbrella flag that puts LLVM into "minimum-size" mode. It does
*not* imply or subsume any of the other 14 flags — they are all knobs that
stay independently useful on top of it.

### 1. `-Oz`

**What it does.** Selects the optimization level that prioritises code size
*above all else*. It is strictly more aggressive than `-Os`: clang turns off
any `-Os` optimisation whose net effect is to grow the text section, even
when that optimisation would also have made the code faster. Internally,
both `-Os` and `-Oz` start from the `-O2` pipeline; `-Oz` then sets the LLVM
`MinSize` function attribute, which feeds into inlining thresholds, loop
unrolling decisions, vectorisation cost models, and the SLP vectoriser.

**Why it shrinks the binary.** Tiny inlining threshold (≈5 vs. ≈225 at
`-O3`), no loop unrolling, no auto-vectorisation, prefers calling helpers
over inlining them, prefers shorter instructions even when slightly slower.

**Trade-offs.** Measurable runtime cost on hot loops. Fine here because a
pomodoro timer is overwhelmingly idle.

**Sources.**
- Clang Command Line Reference, `-O<n>` section:
  <https://clang.llvm.org/docs/CommandGuide/clang.html#cmdoption-O0>
- LLVM `MinSize`/`OptimizeForSize` attributes:
  <https://llvm.org/docs/LangRef.html#function-attributes>
- Discussion of the `-Os` vs `-Oz` split in LLVM:
  <https://reviews.llvm.org/D69277>

---

## Cluster B — Dead-code & dead-data elimination

This is the largest cluster, and the one with the most internal overlap.
The flags below all reduce the binary by getting rid of code or data that
is never used. They form three layers stacked on each other:

1. **LTO (`-flto=thin`)** — does this at the LLVM IR level across the
   whole program *before* native code is generated. This is the most
   powerful layer but only applies to translation units you actually
   compile with LTO; pre-built system libraries are opaque to it.
2. **Sections + `--gc-sections`** — does the same thing at the ELF
   section level *after* native code is generated. Necessary for system
   libraries, and a safety net even for LTO'd code.
3. **`--icf=all` and `-fmerge-all-constants`** — go one step further by
   *merging* sections (or constants) that are bit-identical, rather than
   only dropping unreferenced ones.

The result is a lot of redundancy on paper but very little wasted effort
in practice: each layer catches things the others miss.

### 2. `-flto=thin`

**What it does.** Emits LLVM bitcode (instead of, or alongside, native
object code) and defers final code generation until link time, where the
linker hands the bitcode back to LLVM for whole-program analysis. *Thin*
LTO splits that analysis into a fast serial "summary" pass plus parallel
per-module backends, so it scales to large codebases without the memory
blow-up of classic monolithic ("full") LTO.

**Why it shrinks the binary.** Cross-translation-unit inlining lets the
optimiser delete dead code that previously had to survive as an exported
symbol; constant propagation across TUs eliminates unreachable branches;
global dead-code elimination can drop entire functions and globals.

**Overlap with the rest of Cluster B.** LTO already does, at the IR level,
much of what `-ffunction-sections` + `-fdata-sections` + `--gc-sections`
do at the ELF level (function-granularity dead-code elimination, DCE) and what
`-fmerge-all-constants` does for `.rodata` blobs (cross-TU constant
merging). The section-based flags are still pulling weight because
**system libraries are not LTO'd** — FLTK, libc++, the Advanced Linux Sound
Architecture (ALSA), and the X Window System (X11) all arrive
as opaque `.o`/`.a` files, and the only tool that can prune them is the
linker's section-level GC.

**Trade-offs.** Longer link times, requires a compatible linker (LLD or
gold with the LLVMgold plugin), and debug info is harder to follow.

**Sources.**
- ThinLTO design doc: <https://clang.llvm.org/docs/ThinLTO.html>
- "ThinLTO: Scalable and Incremental LTO" — Tobias Edler von Koch, Teresa
  Johnson et al. (LLVM Developers' Meeting 2015):
  <https://llvm.org/devmtg/2015-04/slides/ThinLTO_EuroLLVM2015.pdf>
- Linker bitcode handling: <https://lld.llvm.org/index.html>

---

### 3. `-ffunction-sections`

**What it does.** Emits each function into its own ELF section
(`.text.<mangled_name>`) instead of one big `.text`.

**Why it shrinks the binary.** Sections are the granularity the linker
uses for dead-code elimination (`--gc-sections`). One function per
section means the linker can drop unreferenced functions individually
instead of having to keep an entire object file.

**Overlap.** Pairs with `-fdata-sections` (next) and `--gc-sections`
(after that) — none of the three is useful without the other two.

**Trade-offs.** Larger object files on disk (more section headers and
relocation entries) and slightly slower link times. Both are dwarfed by
the final binary size win.

**Sources.**
- GNU Compiler Collection (GCC) docs `-ffunction-sections`:
  <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>
- GNU `ld` manual on `--gc-sections`:
  <https://sourceware.org/binutils/docs/ld/Options.html>

---

### 4. `-fdata-sections`

**What it does.** Same idea as `-ffunction-sections`, but for data:
each global / static variable goes into its own `.data.*` / `.bss.*` /
`.rodata.*` section.

**Why it shrinks the binary.** Lets `--gc-sections` discard unused
globals and string literals individually.

**Overlap.** The data-side counterpart to `-ffunction-sections`. Both
exist *only* because `--gc-sections` works at section granularity; they
have no effect on their own.

**Trade-offs.** Same as above (more relocations, slower link).

**Sources.**
- GCC docs `-fdata-sections`:
  <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>

---

### 5. `-Wl,--gc-sections`

**What it does.** Performs garbage collection at section granularity.
The linker builds a reachability graph rooted at the entry point (and
any KEEP'd sections from the linker script) and discards every input
section not reachable.

**Why it shrinks the binary.** Combined with `-ffunction-sections` and
`-fdata-sections`, this is *the* mechanism by which unused functions,
strings, and globals leave the binary. On a C++ project it also drops
unused virtual functions whose vtables ended up dead.

**Overlap.** This is the consumer for flags #3 and #4 — they exist for
this. It also overlaps with `-flto=thin`: LTO already drops dead
IR-level functions before they ever reach the linker. The reason to
keep both is that LTO cannot see into pre-built system libraries, but
`--gc-sections` can.

**Trade-offs.** Things referenced only by name (e.g. via `dlsym`, or by
the dynamic linker) need explicit `KEEP` directives or
`__attribute__((used))` to survive. Not an issue for self-contained
executables.

**Sources.**
- GNU `ld` manual, `--gc-sections`:
  <https://sourceware.org/binutils/docs/ld/Options.html>
- LLD ELF docs: <https://lld.llvm.org/ELF/start.html>

---

### 6. `-Wl,--icf=all`

**What it does.** Identical code folding (ICF). After all input sections are
laid out, the linker hashes the body of each function-bearing section
and merges sections that are byte-identical (modulo relocations) into a
single copy, redirecting all references. `=all` folds *anything* that
matches; `=safe` (the default elsewhere) only folds sections the
compiler has tagged as address-insensitive.

**Why it shrinks the binary.** Template instantiations with different
type parameters but identical generated code collapse into one. So do
trivial getters, thunks, and many small helpers.

**Overlap.** Does **not** overlap with LTO. LTO inlines and DCEs but
does not fold two byte-identical *generated* functions into one; that
only happens post-codegen, at the linker. ICF is the natural
companion to `--gc-sections`: GC drops unreferenced sections, ICF
merges duplicates among the survivors.

**Trade-offs.** `=all` breaks the C/C++ guarantee that two distinct
functions have distinct addresses, so taking and comparing function
pointers can silently equate unrelated functions. Requires LLD or gold —
not supported by classic GNU `bfd` ld.

**Sources.**
- Sriraman Tallam et al., "Safe ICF: Pointer Safe and Unwinding Aware
  Identical Code Folding in Gold":
  <https://research.google/pubs/safe-icf-pointer-safe-and-unwinding-aware-identical-code-folding-in-the-gold-linker/>
- LLD ICF documentation:
  <https://lld.llvm.org/ELF/start.html>

---

### 7. `-fmerge-all-constants`

**What it does.** Allows the compiler/linker to merge *any* constants
with identical bit patterns into a single storage location — including
arrays and aggregates, not just the strings and floats covered by the
default `-fmerge-constants`.

**Why it shrinks the binary.** Duplicate string literals, repeated
lookup tables, and identical `constexpr` blobs collapse to one copy in
`.rodata`.

**Overlap.** This is the data-side analogue of `--icf=all`: same idea
(merge byte-identical things), different domain (read-only data instead
of code). Also partially overlaps with `-flto=thin`, which already
deduplicates constants across TUs at the IR level. The marginal win on
top of LTO is the merging of *aggregate* constants (arrays, structs)
that LTO will not always combine.

**Trade-offs.** *Not standard-conforming C/C++*: the standard
guarantees distinct addresses for distinct objects with the same value,
and this flag breaks that. In practice nothing in this project compares
addresses of constants, but be aware before recommending it broadly.

**Sources.**
- GCC docs `-fmerge-all-constants`:
  <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>
- ISO C++ [basic.compound]/3 on object identity (background on why it is
  non-conforming).

---

## Cluster C — Disabling C++ runtime features

These two travel together. Both remove application binary interface (ABI) machinery that the C++
runtime would otherwise generate; both are independent of everything in
Cluster B; neither overlaps with the other (different ABI surfaces).
They are listed here as a pair because in the talk it is natural to
explain "what does C++ cost you" in one breath.

### 8. `-fno-exceptions`

**What it does.** Disables C++ exception support in the front-end:
`throw` and `try`/`catch` become errors, and the compiler stops emitting
the landing-pad machinery (`.gcc_except_table`, personality routines,
cleanup records) that the unwinder needs to run destructors after a
throw.

**Why it shrinks the binary.** Removes per-function exception tables,
the language personality routine reference (`__gxx_personality_v0`) and
its transitive pull on `libstdc++`/`libsupc++`. For small programs this
can strip tens of KB.

**Overlap.** Does **not** subsume `-fno-unwind-tables` (#10). Unwind
tables are emitted for stack walking, signal handling, and backtraces
even when exceptions are off — `-fno-exceptions` only removes the
*landing pads*, not the `.eh_frame` itself.

**Trade-offs.** You cannot use any library that propagates exceptions
across its application programming interface (API) — parts of the Standard
Template Library (STL), for example. Code calling `new` must
be ready to get a null pointer if combined with `-fno-exceptions` and
`nothrow` new, or must use `operator new(std::nothrow)`.

**Sources.**
- Itanium C++ ABI, §15 "Exception Handling":
  <https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html>
- GCC docs `-fexceptions`:
  <https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html>
- Embedded-systems perspective on EH cost: Ben Craig,
  "Removing Exceptions Cost From C++" (P0709-adjacent talks),
  <https://wg21.link/p0709>

---

### 9. `-fno-rtti`

**What it does.** Suppresses generation of `type_info` objects,
polymorphic class type-info tables, and the helpers used by
`dynamic_cast<>` and `typeid()`.

**Why it shrinks the binary.** Each polymorphic class normally drags in
a `type_info` object plus a name string into `.rodata`. With many small
polymorphic classes (or templates), this adds up quickly. Removing run-time
type information (RTTI) also breaks a hidden retention edge in the linker dead-code elimination
graph — once vtables no longer reference `type_info`, the linker can
drop entire chains.

**Overlap.** None with `-fno-exceptions` — they target different ABI
artefacts (`type_info` vs landing pads + personality routine), even
though they sound related.

**Trade-offs.** No `dynamic_cast` (except the trivial up-cast), no
`typeid`, and any third-party library that relies on RTTI must be
compiled the same way or carefully isolated.

**Sources.**
- Itanium C++ ABI, §2.9 "Type Information":
  <https://itanium-cxx-abi.github.io/cxx-abi/abi.html#rtti>
- Clang docs on RTTI controls:
  <https://clang.llvm.org/docs/UsersManual.html#controlling-code-generation>

---

## Cluster D — Unwind metadata (one flag does the work, one does nothing)

> **CORRECTED 2026-07-12 — I had this backwards.** An earlier version of
> these notes claimed that `-fno-unwind-tables` is the stronger flag and
> "strictly subsumes" `-fno-asynchronous-unwind-tables", and that either
> flag alone removes both kinds of table. **That is wrong**, and the
> measurement below shows it. It is the *asynchronous* flag that does all
> the work; `-fno-unwind-tables` on its own is a **no-op** on x86-64 Linux.
> Do not repeat the old claim on stage.

**What I measured** (clang-20, x86-64, `src/main_pro_plus.cpp`, all on top
of `-Oz`; sizes are the `.eh_frame` section):

| flags | `.eh_frame` | `.eh_frame_hdr` | `.gcc_except_table` |
|-------|-------------|-----------------|---------------------|
| *(neither)* | 0x398 | 0xd4 | 0x68 |
| `-fno-unwind-tables` | **0x398 (unchanged!)** | 0xd4 | 0x68 |
| `-fno-asynchronous-unwind-tables` | 0x2b8 | 0x84 | 0x68 |
| both | 0x2b8 | 0x84 | 0x68 |
| `-fno-exceptions` alone | 0x348 | 0xcc | **gone** |
| `-fno-exceptions -fno-async…` | **0x48** | 0x1c | **gone** |

Hex is what `readelf -SW` prints (handy for a live demo). In decimal, the
`.eh_frame` column is **920 → 920 → 696 → 696 → 840 → 72 bytes** — the slide
uses the decimals (920 / 696 / 72) for the headline three rows.

**Why.** The x86-64 processor-specific ABI (psABI) *requires* asynchronous unwind tables, so clang's
driver defaults to `-fasynchronous-unwind-tables` on this target. That
default **implies** `-funwind-tables`. Passing `-fno-unwind-tables` does not
switch off the asynchronous default, so the async setting simply turns the
tables straight back on — the flag is silently overridden. Only
`-fno-asynchronous-unwind-tables` clears the default, and *then* the sync
tables go too.

**The real lesson for the talk** (better than the old one): `.eh_frame` does
not collapse until you turn off exceptions **and** async unwind tables
*together* — 0x398 → 0x48, and the 0x48 that survives is not even your code,
it is the C-runtime startup objects. Two flags, from two different clusters,
that only pay off as a pair. That is a more interesting story than "these two
are really one".

**Keeping `-fno-unwind-tables` in the build is still defensible** — it
documents intent and guards against target-default changes on other
architectures (on ARM, for instance, the defaults differ). But be honest:
on *this* target it buys exactly zero bytes. The leave-one-out table further
down confirms it: removing it changes the binary by **0 B**.

### 10. `-fno-unwind-tables`

**What it does.** Stops emitting the `.eh_frame` (DWARF call-frame
information) tables that an unwinder uses to walk the stack when an
exception propagates or when a debugger / profiler asks for a
backtrace.

**Why it shrinks the binary.** `.eh_frame` is *always* loaded at
runtime (it lives in a `PT_LOAD` segment), so it counts toward on-disk
size, resident set size (RSS), and load time. On a release C++ binary it is frequently the
second-largest read-only section after `.rodata`.

**Overlap.** ⚠️ **On x86-64 Linux this flag does nothing on its own.** It
does *not* subsume `-fno-asynchronous-unwind-tables` (#11) — the reverse is
true. The async default implies `-funwind-tables` and overrides this flag.
Measured: removing it from the full tiny set changes the binary by 0 bytes.
See the corrected cluster intro.

**Trade-offs.** *If* it took effect: no usable `backtrace()` from the C
runtime, and profilers that rely on frame unwinding (`perf --call-graph=dwarf`)
fall back to frame-pointer walking. Also useless while exceptions are still
enabled — the compiler re-emits the tables regardless.

**Sources.**
- Linux Standard Base, "Exception Frames":
  <https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html>
- Ian Lance Taylor, "The .eh_frame section":
  <https://www.airs.com/blog/archives/460>
- GCC docs `-funwind-tables`:
  <https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html>

---

### 11. `-fno-asynchronous-unwind-tables`

**What it does.** Like the above, but specifically removes the
*precise* per-instruction unwind tables that the Itanium ABI calls
"asynchronous" unwind info — the tables needed when control can leave a
function at any instruction boundary (e.g. on a signal, on a debugger
trap, mid-prologue). On x86-64 Linux the ABI mandates them, so they are
on by default even with `-fno-exceptions`.

**Why it shrinks the binary.** Without async tables, the compiler can
emit much sparser call-frame-information (CFI) directives — only at points where the unwind
state actually changes for synchronous reasons.

**Overlap.** ✅ **This is the flag that does the work.** Async tables are a
strict superset of sync tables — you cannot have async without sync — and on
x86-64 Linux the *default* is `-fasynchronous-unwind-tables` (which implies
`-funwind-tables`). So this is the **only** flag of the pair that can clear
the default. `-fno-unwind-tables` (#10) alone is a no-op. Measured cost of
removing this flag from the tiny set: **+528 B** (terminal build), **+864 B**
(FLTK build).

Pairs with `-fno-exceptions` (#8): neither alone empties `.eh_frame`, but
together they take it from 0x398 down to 0x48.

**Trade-offs.** Signal handlers and debuggers cannot reliably
reconstruct the stack from arbitrary instruction pointers. For a hosted
Linux application that does not deliver async signals, this is fine.

**Sources.**
- System V x86-64 psABI §3.7 (unwind tables required by default):
  <https://gitlab.com/x86-psABIs/x86-64-ABI>
- GCC docs `-fasynchronous-unwind-tables`:
  <https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html>

---

## Cluster E — Symbol surfaces (related-looking, not overlapping)

The next three flags all reduce some kind of "symbol" in the output and
look interchangeable at a glance. They are not. Each touches a
different table:

| Flag | Affects | Loaded at runtime? |
|------|---------|--------------------|
| `-fvisibility=hidden` | `.dynsym` / `.dynstr` (dynamic exports) | Yes |
| `-Wl,--as-needed`     | `DT_NEEDED` entries in `.dynamic` | Yes |
| `-Wl,-s`              | `.symtab` / `.strtab` (static symbols) | No (on-disk only) |

The first two also affect runtime behaviour (linking semantics, dynamic
loader work); the third is purely cosmetic from the loader's point of
view. Worth a slide on its own — this distinction trips people up.

### 12. `-fvisibility=hidden`

**What it does.** Changes the default ELF symbol visibility from
`default` (exported, interposable) to `hidden`. Symbols marked hidden
are *not* placed in the dynamic symbol table of the resulting shared
object or executable, and cross-dynamic-shared-object (DSO) calls to them
are impossible.

**Why it shrinks the binary.** (a) Fewer entries in `.dynsym` /
`.dynstr`. (b) The optimiser is free to inline hidden functions,
devirtualise calls, and delete unused ones — because nothing outside
the module can take their address. (c) Calls become direct
program-counter-relative (PC-relative) branches instead of going through the
procedure linkage table / global offset table (PLT/GOT).

**Overlap.** Often confused with `-Wl,-s` (#14). They affect *different*
symbol tables: `.dynsym` (runtime, loaded) vs `.symtab` (debug,
on-disk). You typically want both, for different reasons.

**Trade-offs.** Anything you intend to expose from a shared library
must be explicitly annotated (`__attribute__((visibility("default")))`
or the `[[gnu::visibility]]` attribute). Harmless for a self-contained
executable.

**Sources.**
- GCC visibility wiki page (still the canonical reference, applies to
  clang): <https://gcc.gnu.org/wiki/Visibility>
- Ulrich Drepper, "How To Write Shared Libraries", §2.2:
  <https://www.akkadia.org/drepper/dsohowto.pdf>
- Clang docs on `-fvisibility=`:
  <https://clang.llvm.org/docs/CommandGuide/clang.html>

---

### 13. `-Wl,--as-needed`

**What it does.** Tells the linker to only emit a `DT_NEEDED` entry for
a shared library if at least one symbol from that library is actually
referenced by the program. Without this, every `-l<foo>` on the command
line ends up in the dynamic dependency list, whether it is used or not.

**Why it shrinks the binary.** A smaller `.dynamic` / `.dynstr`
section, and — more importantly at runtime — fewer libraries mapped,
fewer symbol lookups, faster startup, smaller RSS.

**Overlap.** None with the rest of Cluster E: `.dynamic` /
`DT_NEEDED` is a third table, distinct from both `.dynsym` and
`.symtab`.

**Trade-offs.** Link order matters: if `libA.so` pulls in `libB.so`'s
symbols transitively, you need to order them correctly on the command
line.

**Sources.**
- GNU `ld` manual, `--as-needed`:
  <https://sourceware.org/binutils/docs/ld/Options.html>
- Drepper, "How To Write Shared Libraries", §2:
  <https://www.akkadia.org/drepper/dsohowto.pdf>

---

### 14. `-Wl,-s`

**What it does.** Strips the symbol table (`.symtab`) and string table
(`.strtab`) from the output. Equivalent to running `strip` on the
finished binary, but performed by the linker so the data is never
written to disk.

**Why it shrinks the binary.** `.symtab` / `.strtab` are not loaded
into memory at runtime, but they do consume on-disk space — often a
double-digit percentage of a small C++ binary's total size, dominated
by mangled template names.

**Overlap.** Often conflated with `-fvisibility=hidden` (#12). They
are not the same: `-s` strips the *static* symbol table used by
debuggers and profilers, which is on-disk only; visibility controls
the *dynamic* symbol table used by the runtime loader. Both are
worth doing, for different reasons.

**Trade-offs.** No symbols for debuggers or profilers. Keep an
unstripped copy (or split debug info with `objcopy --only-keep-debug`)
for crash investigation in production.

**Sources.**
- GNU `ld` manual, `-s`:
  <https://sourceware.org/binutils/docs/ld/Options.html>
- ELF spec on `.symtab` / `.strtab`:
  <https://refspecs.linuxfoundation.org/elf/elf.pdf>

---

## Cluster F — Assembler

The pure-asm `_ultra` build is the only one that touches the Netwide
Assembler (NASM). Nothing
here overlaps with anything above (it operates on a different toolchain
path entirely), but for completeness:

### 15. `nasm -Ox`

**What it does.** Enables NASM's *multi-pass* optimiser. NASM normally
makes a single forward pass and conservatively picks the longest
encoding when a forward reference could be either short or long (e.g.
`jmp short` vs `jmp near`). `-Ox` lets NASM iterate until a fixed
point, picking the shortest encoding for every instruction.

**Why it shrinks the binary.** Short jumps are 2 bytes vs 5; many
immediate-loads have multiple legal encodings; effective addresses with
zero displacement can drop the displacement byte. On a few hundred
lines of asm this typically saves a percent or two — small in absolute
terms but visible when the goal is a kilobyte-class binary.

**Trade-offs.** Multi-pass assembly is slower (negligibly so for a
small file). Some hand-rolled encodings that assume a specific size
(e.g. patching) must use the explicit form (`jmp near`).

**Sources.**
- NASM manual, §2.1.22 "Multipass Optimization":
  <https://www.nasm.us/doc/nasmdoc2.html#section-2.1.22>
- Intel SDM Vol. 2 (instruction encoding reference, background on why
  multiple encodings exist).

---

### Bonus: standalone link of the asm target — `ld -nostdlib -static -s`

Not in the headline list of 15 but worth mentioning in the talk because
it is what makes the `_ultra` binary tiny.

- **`-nostdlib`** — do not link the standard C runtime (`crt1.o`,
  `crti.o`, `crtn.o`, libc). The program provides its own `_start` and
  talks to the kernel directly via `syscall`. The C runtime alone
  contributes ~10 KB of statically-linked code before you write a line
  of your own.
  Source: GNU `ld` manual, `-nostdlib`:
  <https://sourceware.org/binutils/docs/ld/Options.html>

- **`-static`** — produce a statically-linked executable with no
  `PT_INTERP` / dynamic linker, no `.dynamic` section, no PLT/GOT.
  Source: same manual page.

- **`-s`** — strip, as flag #14 above.

For interest: the absolute floor for an ELF executable on Linux is
around 45 bytes if you hand-write the headers (Brian Raiter,
"A Whirlwind Tutorial on Creating Really Teensy ELF Executables for
Linux", <https://www.muppetlabs.com/~breadbox/software/tiny/teensy.html>).
That is the ultimate reference for "how small can it go" and worth a
slide if you have one to spare.

---

# Part 1.5 — Your flags stop at your code

**This is the most important experiment in the talk, and it was added last.**

`tiny_pomodoro` links `/usr/local/lib/libfltk.a` — a *pre-built* static archive.
Someone (you, months ago) compiled it with `-O3`, exceptions on, RTTI on, no size
flags at all, against **libstdc++**. The 15 flags in `CMakeLists.txt` are applied
to exactly **one file**: `src/main.cpp`, ~350 lines. Meanwhile **3.3 MB of
archive** — the thing that actually fills the binary — was compiled with none of
them.

`tiny_pomodoro_src` is the **same `main.cpp`**, but FLTK is compiled as part of
our build (`add_subdirectory`), so our flags reach it.

## The numbers (2026-07-12, clang-20)

| build | binary | vs pre-built |
|---|---:|---:|
| pre-built FLTK, all tiny flags | 850,536 B | — |
| FLTK from source, tiny flags **minus** `-flto=thin` | 419,464 B | **−51%** |
| FLTK from source, **all** tiny flags | **327,552 B** | **−61%** |

And the control that proves it is the *flags* doing the work, not the recompile:

| build | pre-built | from source |
|---|---:|---:|
| **release** (`-O3`, no size flags) | 1,112,096 B | 1,115,616 B (**+0.3%**) |
| **tiny** (all 15 flags) | 850,536 B | 327,552 B (**−61%**) |

In `release` there are no size flags, so compiling FLTK ourselves buys **nothing**
— it is marginally *bigger*. Building from source is not itself an optimisation.
It is what gives the optimisations something to work on.

## Decomposing the 523 KB

1. **−431 KB — our flags now apply to FLTK's code.** `-Oz` instead of `-O3`,
   `-fno-exceptions`, `-fno-rtti`, `-ffunction-sections` + `--gc-sections`,
   `--icf=all` all now act on the *library*, not just on `main.cpp`. This is the
   bulk of it, and it needs no LTO at all.
2. **−92 KB more — LTO can now see inside.** With `-flto=thin`, FLTK's object
   files are LLVM bitcode, so the optimiser inlines and DCEs *across the
   app/library boundary*. Verify:
   `file build/tiny/fltk_src/src/CMakeFiles/fltk.dir/Fl_Window.cxx.o`
   → **`LLVM IR bitcode`**, where the pre-built archive's member is
   → **`ELF 64-bit LSB relocatable`**.

## The bonus: one C++ runtime instead of two

The pre-built `libfltk.a` is a **libstdc++** build; the app is compiled
`-stdlib=libc++`. CMake's link line passes `-lc++` but not `-stdlib=libc++`, so
the driver *also* links its default libstdc++ — and `tiny_pomodoro` ends up
needing **both** runtimes:

```
$ readelf -d build/tiny/tiny_pomodoro     | grep -Ei 'c\+\+|stdc'
    libc++abi.so.1
    libstdc++.so.6          ← two C++ runtimes in one process
$ readelf -d build/tiny/tiny_pomodoro_src | grep -Ei 'c\+\+|stdc'
    libc++.so.1
    libc++abi.so.1          ← one
```

Compiling FLTK ourselves lets us build it with libc++ too, and the duplicate
runtime disappears. (This also explains a link failure you will hit if you try it
yourself: FLTK **must** get `-stdlib=libc++` in *every* build type, not just
`Tiny` — it is an ABI decision, not an optimisation. Get it wrong and the release
build dies with undefined `std::__throw_length_error` / `_Rb_tree_*` symbols,
because FLTK's `std::string` and the app's `std::string` are different types.)

## Trade-offs — say these out loud

- **Build time.** FLTK takes minutes to compile; the pre-built archive is
  instant. This is the real cost, and for most projects it is the right call to
  pay the size and keep the fast build.
- **`-fno-exceptions` / `-fno-rtti` on someone else's library** is a genuine risk.
  FLTK 1.4 happens to compile cleanly with both (zero errors, zero warnings) —
  I checked. Not every library will.
- **You now own the library's build.** Upgrades, patches, platform quirks.

## The line for the talk

*"I spent a week on fifteen flags. They applied to one file. The other 3.3
megabytes were compiled by someone else with none of them — and that someone else
was me, six months ago."*

---

# Part 2 — Measured: what each flag is actually worth

Everything above is what the flags are *supposed* to do. This is what they
*did*, on this machine, on 2026-07-12, with clang-20.

**Method — leave-one-out (ablation).** Build with the full tiny flag set, then
rebuild removing exactly one flag, and record how much the binary *grows*. This
attributes savings honestly and, unlike adding flags one at a time, does not
depend on the order you list them in. (Removing `-Oz` falls back to `-O2`.)

**Reproduce it live:** the script is trivial — a loop that drops one flag and
runs `stat -c%s`. Worth doing on stage if you have the time.

### Terminal build (`main_pro_plus.cpp`) — full tiny set = 9,968 B

| flag removed | binary | cost of removing |
|---|---:|---:|
| `-Wl,-s` | 13,240 | **+3,272** |
| `-fno-exceptions` | 11,096 | **+1,128** |
| `-fno-asynchronous-unwind-tables` | 10,496 | +528 |
| `-Oz` | 10,448 | +480 |
| `-flto=thin` | 10,400 | +432 |
| `-Wl,--as-needed` | 10,032 | +64 |
| `-Wl,--gc-sections` | 9,992 | +24 |
| `-ffunction-sections` | 9,968 | **0** |
| `-fdata-sections` | 9,968 | **0** |
| `-fno-rtti` | 9,968 | **0** |
| `-fvisibility=hidden` | 9,968 | **0** |
| `-fno-unwind-tables` | 9,968 | **0** |
| `-fmerge-all-constants` | 9,968 | **0** |
| `-Wl,--icf=all` | 9,968 | **0** |

### FLTK build (`main.cpp`) — full tiny set = 850,536 B

| flag removed | binary | cost of removing |
|---|---:|---:|
| `-Wl,-s` | 1,094,112 | **+243,576** |
| `-Wl,--icf=all` | 858,744 | **+8,208** |
| `-fno-exceptions` | 853,448 | +2,912 |
| `-fno-asynchronous-unwind-tables` | 851,400 | +864 |
| `-flto=thin` | 851,160 | +624 |
| `-Wl,--as-needed` | 851,160 | +624 |
| `-Oz` | 851,008 | +472 |
| `-Wl,--gc-sections` | 850,808 | +272 |
| the other six | 850,536 | **0** |

### What this actually means — the honest slide

1. **Stripping dominates everything.** `-Wl,-s` is 60% of the savings on the
   terminal build and **96%** on the FLTK build. The largest single win in a
   size-optimisation talk comes from deleting *symbol names* — data the kernel
   never even loads. Nearly a quarter of a megabyte of `tiny_pomodoro` is
   mangled C++ names.
2. **Six of the fourteen flags move zero bytes** on *both* targets. They are
   not useless in general — `-fno-rtti` needs polymorphism to bite,
   `-fmerge-all-constants` needs duplicate constants, `-ffunction-sections`
   only matters if `--gc-sections` has something to collect — this program just
   does not give them anything to do.
3. **The same flag is worth wildly different amounts on different programs.**
   `--icf=all`: **0 B** on the terminal build, **8,208 B** on FLTK (templates
   and thunks to fold). This is the argument for *measuring* rather than
   cargo-culting a flag list.
4. **`-ffunction-sections`/`-fdata-sections` show 0 because LTO got there
   first.** Remove LTO *and* `--gc-sections` together and the picture changes.
   Leave-one-out systematically *under*-credits redundant flags: if two flags
   catch the same bytes, dropping either one alone looks free. Say this out
   loud — it is the main weakness of the method, and someone will ask.

**A good closing beat for the flags section:** *"Fifteen flags. On this
program, six of them do nothing, and one of them — `strip` — is worth more than
all the others combined."*

---

## Background — FLTK and the cost of cross-platform abstractions

**What FLTK is.** "Fast Light Toolkit" (FLTK) — a C++ graphical user interface
(GUI) library originally written for Silicon Graphics' (SGI) IRIX in the mid-90s. The "fast and light" branding
made sense in that era (the alternative was Motif), but on modern
Linux it sits on top of, and pulls in, a large stack of platform glue.

> **CORRECTED 2026-07-12 — FLTK is linked STATICALLY here, not dynamically.**
> An earlier version of these notes said "FLTK arrives as a pre-built `.so`"
> and that `--gc-sections` "cannot prune symbols inside `libfltk.so`". Both
> are wrong. On this machine CMake resolves FLTK to
> **`/usr/local/lib/libfltk.a`** — a *static archive*. Check it yourself:
> `grep fltk build/tiny/CMakeFiles/tiny_pomodoro.dir/link.txt`. This changes
> the whole explanation of why the binary is big, so the old wording must not
> be used on stage.

**Why the FLTK pomodoro is ~830 KB.** Not because of dynamic-linking
overhead — because **FLTK's own machine code is copied into the executable**.
`libfltk.a` is a static archive, so the linker pulls in every archive member
that anything references, and those members reference more members: the widget
base classes, the event loop, the drawing layer, the font handling, the image
decoders. That transitive closure *is* the 830 KB. The binary is big because
it *contains* FLTK.

**What the platform libraries actually cost (measured, and it surprised me).**
The link line passes **38** libraries. The finished binary records only
**13** `DT_NEEDED` entries. `--as-needed` drops every one of these:

> gtk-3, gdk-3, pango-1.0, pangocairo-1.0, cairo, cairo-gobject, atk-1.0,
> gdk_pixbuf-2.0, gio-2.0, gobject-2.0, glib-2.0, harfbuzz, wayland-client,
> wayland-cursor, xkbcommon, dbus-1, Xext, z, dl, pthread

So the GTK/Wayland/DBus stack that the old slide listed as a dependency of
`tiny_pomodoro` **is not a dependency at all** — it is on the command line
because CMake's `pkg_check_modules` puts it there, and the linker throws it
away. Verify with `ldd build/tiny/tiny_pomodoro`. The size is FLTK's static
code, not a pile of shared libraries.

**A genuine wart worth mentioning.** `libfltk.a` was built against
**libstdc++**, but the app compiles with `-stdlib=libc++`. The CMake link line
passes `-lc++` but *not* `-stdlib=libc++`, so the driver also links its default
libstdc++ — and the binary ends up with **two C++ runtimes**:
`readelf -d build/tiny/tiny_pomodoro | grep -E 'c\+\+|stdc'` shows both
`libc++abi.so.1` and `libstdc++.so.6`. It works, but it is an accident, and
someone in the audience will spot it.

**Why the optimisations behave differently here.** `-flto=thin` still cannot
see into FLTK — not because it is a `.so`, but because the archive holds
*native object code*, not LLVM bitcode. `--gc-sections` and `--icf=all`,
however, absolutely *can* reach inside a static archive, and ICF earns its
keep here: removing `--icf=all` costs **+8,208 B** on the FLTK build versus
**0 B** on the terminal build, because FLTK's templates and thunks give it
something to fold. `--gc-sections` adds surprisingly little on top (+272 B) —
the linker's ordinary archive-member selection (pull only the `.o` members you
need) has already done most of that job.

**Trade-offs (for the talk).** FLTK is the right choice if you want a
native-looking GUI, file dialogs, accessibility, and clipboard support
without writing them yourself. It is the wrong choice if a small binary
is the goal. The 830 KB figure is *already* heavily optimised; the
unoptimised FLTK build is around 1.2 MB — and **unstripped it is 1,094 KB**,
so a quarter of a megabyte of it is nothing but symbol names.

**Sources.**
- FLTK home page: <https://www.fltk.org/>
- FLTK introduction (lists the dependency stack):
  <https://www.fltk.org/doc-1.3/intro.html>

---

## Background — X11 / Xlib (the layer below FLTK)

**What it is.** The original Linux display server protocol, dating to
1984. The X server runs as a separate process; `libX11` (Xlib) is the
client library that speaks the X protocol over a Unix-domain socket.

**Why it is much smaller than FLTK.** Xlib exposes *primitives*, not
widgets: open a connection, create a window, get a graphics context,
draw lines / rectangles / text, read keyboard and mouse events from
`XNextEvent`. There is no widget tree, no theming, no text shaping, no
clipboard abstraction, no file dialogs. The application has to draw
every button by hand — which is what `tiny_pomodoro_pro` does — but in
exchange the only library you link against is `libX11.so` plus a few
small extensions (`Xext`, `Xinerama`, `Xcursor`, `Xrender`, `Xfixes`,
`fontconfig`). The `tiny_pomodoro_pro` tiny build comes in at **17 KB**
— roughly 50× smaller than the FLTK build for the same feature set.

**Why you'd still pick it for a tiny binary.** Xlib is effectively the
floor for a Linux GUI: anything above it (FLTK, Qt, GTK) wraps it, and
anything below it is "no GUI" (terminal, framebuffer). For a
single-window utility with no theming requirements, it is the right
tool.

**Wayland note (for Q&A).** Wayland is the modern replacement for X11.
For a size-focused talk it is *not* a win — `libwayland-client` is
small, but the application must implement window decorations, input
dispatch, and surface buffer management itself, and most desktops still
run XWayland for X clients anyway. X11 is the simpler comparison point
for this talk.

**Sources.**
- X protocol reference (the canonical Xlib spec):
  <https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html>
- *Xlib Programming Manual* — Adrian Nye, O'Reilly 1992 (still the
  clearest tutorial introduction).
- `man X` — concise overview of how the protocol / server / client
  model fits together.

---

## Background — The ELF page-alignment floor

→ **Moved.** The full treatment now lives in **Part 0, "The ELF file format"**
(sections vs segments, why protection forces separate pages, the
page-occupancy table for `tiny_pomodoro_2`, the two orthogonal axes, and how
to go below the floor). It was rewritten there with corrected numbers; do not
rehearse from two copies. Only the sources are kept here.

**Sources.**
- ELF specification, "Program Header" section:
  <https://refspecs.linuxfoundation.org/elf/elf.pdf>
- System V x86-64 psABI, program-loading rules:
  <https://gitlab.com/x86-psABIs/x86-64-ABI>
- Brian Raiter, *A Whirlwind Tutorial on Creating Really Teensy ELF
  Executables for Linux* — the canonical 45-byte reference:
  <https://www.muppetlabs.com/~breadbox/software/tiny/teensy.html>
- `man elf(5)` — kernel-side perspective on what the loader uses from
  the file.

---

## Suggested further reading for the talk

- Brian Raiter, *A Whirlwind Tutorial on Creating Really Teensy ELF
  Executables for Linux* — the canonical tiny-ELF write-up.
  <https://www.muppetlabs.com/~breadbox/software/tiny/teensy.html>
- Ulrich Drepper, *How To Write Shared Libraries* — covers visibility,
  `--as-needed`, dynamic-section costs.
  <https://www.akkadia.org/drepper/dsohowto.pdf>
- LLD documentation — the canonical reference for `--icf`, LTO link
  flow, and section GC.
  <https://lld.llvm.org/>
- Bloaty McBloatface — the tool to *measure* what each flag bought you.
  <https://github.com/google/bloaty>
- Matt Godbolt, *Compiler Explorer* — useful for live demos of how
  `-Oz` differs from `-O3` on small snippets.
  <https://godbolt.org/>


