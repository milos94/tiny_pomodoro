# Size Optimization Flags — Talk Notes

Reference notes for each of the 15 size-reduction flags used by `tiny_pomodoro`.
Each entry lists *what it does*, *why it shrinks the binary*, *what you give
up*, and *where to read more*.

Flags are grouped by **what they do**, not by which tool consumes them, so
flags whose effects overlap sit next to each other and the overlap can be
discussed in one place. Each cluster starts with a short note on how the
flags inside it interact.

---

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
do at the ELF level (function-granularity DCE) and what
`-fmerge-all-constants` does for `.rodata` blobs (cross-TU constant
merging). The section-based flags are still pulling weight because
**system libraries are not LTO'd** — FLTK, libc++, ALSA, X11 all arrive
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
- GCC docs `-ffunction-sections`:
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

**What it does.** Identical Code Folding. After all input sections are
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

These two travel together. Both remove ABI machinery that the C++
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
across its API (parts of the STL, for example). Code calling `new` must
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
polymorphic classes (or templates), this adds up quickly. Removing RTTI
also breaks a hidden retention edge in the linker dead-code elimination
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

## Cluster D — Unwind metadata (the only genuinely redundant pair)

The next two flags both disable DWARF call-frame information. The first
is strictly stronger than the second on x86-64 Linux — async tables are
the precise per-instruction form of sync tables, so they cannot exist
without the sync ones. Disabling `-funwind-tables` necessarily disables
the async variant too. Having both is **belt and suspenders**, not two
separate savings.

It is still defensible to write both: it documents intent ("we know what
both knobs are, and we want them off"), it is robust against
target-default changes between toolchain versions, and it costs nothing.
But in the talk this is the place to be honest: if you are counting
flags, these two are really one.

### 10. `-fno-unwind-tables`

**What it does.** Stops emitting the `.eh_frame` (DWARF call-frame
information) tables that an unwinder uses to walk the stack when an
exception propagates or when a debugger / profiler asks for a
backtrace.

**Why it shrinks the binary.** `.eh_frame` is *always* loaded at
runtime (it lives in a `PT_LOAD` segment), so it counts toward on-disk
size, RSS, and load time. On a release C++ binary it is frequently the
second-largest read-only section after `.rodata`.

**Overlap.** Strictly subsumes `-fno-asynchronous-unwind-tables` (#11).
See cluster intro.

**Trade-offs.** No usable `backtrace()` from the C runtime, profilers
that rely on frame unwinding (perf with `--call-graph=dwarf`) will fail
or fall back to frame-pointer walking. Useless if exceptions are still
enabled — the compiler will re-emit the tables.

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
emit much sparser CFI directives — only at points where the unwind
state actually changes for synchronous reasons.

**Overlap.** **Redundant with `-fno-unwind-tables` (#10).** Async
tables are a strict superset of sync tables; you cannot have async
without sync. On x86-64 Linux this is actually the *minimum* flag of
the pair, because the default is `-fasynchronous-unwind-tables`
(implying `-funwind-tables`). Either flag alone removes both kinds of
table. Keeping both is documentation, not savings.

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
object or executable, and cross-DSO calls to them are impossible.

**Why it shrinks the binary.** (a) Fewer entries in `.dynsym` /
`.dynstr`. (b) The optimiser is free to inline hidden functions,
devirtualise calls, and delete unused ones — because nothing outside
the module can take their address. (c) Calls become direct PC-relative
branches instead of going through the PLT/GOT.

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

The pure-asm `_ultra` build is the only one that touches NASM. Nothing
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

## Background — FLTK and the cost of cross-platform abstractions

**What FLTK is.** "Fast Light Toolkit" — a C++ GUI library originally
written for SGI's IRIX in the mid-90s. The "fast and light" branding
made sense in that era (the alternative was Motif), but on modern
Linux it sits on top of, and pulls in, a large stack of platform glue.

**Why the FLTK pomodoro is ~830 KB even after `-Oz` + LTO + ICF +
visibility + every other flag in the toolkit.** FLTK is portable, so it
has to talk to whichever backend the host system provides. On modern
Linux that means: X11 (the X server protocol), Cairo (vector drawing),
Pango (text shaping), Harfbuzz (font shaping for Pango), GLib (GTK
runtime), GTK3 (file-dialog widgets), Wayland (modern display protocol),
XKBCommon (keyboard mapping for Wayland), DBus (system messaging),
Fontconfig (font discovery), and ALSA (audio). Each one is a `-l<foo>`
on the command line; each contributes code, dynamic linker entries, and
static-initialiser overhead. Most of the size win from the 15 flags is
consumed before `main()` runs.

**Why the optimisations are less effective here.** `-flto=thin` cannot
see into FLTK because FLTK arrives as a pre-built `.so`. `--gc-sections`
can prune unused symbols in your *own* object files, but it cannot prune
symbols inside `libfltk.so`. `--icf=all` only folds duplicates that
reach the linker as input sections — anything resolved at runtime is
out of reach. `--as-needed` *is* useful here: it drops `-lfltk` if
nothing references it, but of course your code does reference it.

**Trade-offs (for the talk).** FLTK is the right choice if you want a
native-looking GUI, file dialogs, accessibility, and clipboard support
without writing them yourself. It is the wrong choice if a small binary
is the goal. The 830 KB figure is *already* heavily optimised; the
unoptimised FLTK build is around 1.2 MB.

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

This is the discovery that the `tiny_pomodoro_2` experiment forced us
to confront: even with every size flag enabled, a "do nothing"
executable still occupies ~8 KB on disk. Worth a slide because the
audience will assume the flags should keep delivering forever.

**Where the floor comes from.** Linux loads an ELF executable by
`mmap()`ing each `LOAD` program-header segment into the address space.
`mmap()` operates at *page* granularity — 4 KiB on x86-64. Each segment
needs its own page at minimum, because the loader sets memory
protection per page: read-only for the ELF header segment, read+execute
for `.text`, read-only for `.rodata`, read+write for `.data` / `.bss`.
A static, no-libc binary therefore needs at least three `LOAD`
segments (R, R+X, R) ⇒ at least three pages on disk ⇒ ~12 KB of
virtual footprint, and ~8 KB of on-disk file size after the last
page is trimmed.

**Concrete breakdown for `tiny_pomodoro_2`** — 122 bytes of actual
code, 8480 bytes on disk:

| Region | Real content | File offset / size |
|--------|--------------|--------------------|
| ELF header + 3 program headers | 232 B | 0x0000–0x0fff (page 1) |
| `.text` | 122 B | 0x1000–0x1fff (page 2) |
| `.rodata` (one BEL byte) | 1 B | 0x2000–~0x201f (partial) |
| Section headers + `.shstrtab` | ~64 B | end of file |

The 15 flags have done all they can — there is no more `.text` to fold
or GC. From this point, **shrinking the binary is no longer a compiler
problem; it is an ELF problem.**

**Confirming the floor empirically.** Compare `_ultra` (3046 B of
`.text`) with `_2` (122 B of `.text`): on disk they are 8992 B and
8480 B respectively. **25× more code, 6% bigger file.** Once your real
content fits inside one 4 KiB page, adding more code is essentially
free until you spill the next page.

**How to go below the floor.**

1. **Reduce `LOAD` segments.** Merge `.rodata` into the same segment
   as `.text` (`R+X` protection — slightly less safe, but the kernel
   doesn't care for this small a binary). Brings the file down to two
   pages, then one. Requires a custom linker script; not exposed via
   any standard flag.
2. **Overlap headers.** The ELF header has reserved/padding fields;
   the program-header table can be placed inside them. The linker
   won't do this; `objcopy` and hand-tools can.
3. **Hand-rolled ELF.** Brian Raiter's *Teensy ELF* gets to **45
   bytes** by abandoning every section, overlapping header fields,
   and using the `e_ident` padding as instructions. The result is
   unstrippable, ungdbable, and unportable across kernel versions —
   but it runs.

**The takeaway line for the talk.** *"The 15 flags get you to the page
floor. Below that, you're not optimising — you're hand-crafting ELF,
and that is a different talk."*

**Tools to investigate the floor on stage.**
- `readelf -lW <bin>` — shows LOAD segments and their alignment
  (`Align 0x1000` is the smoking gun).
- `readelf -SW <bin>` — sections, sizes, and offsets.
- `size <bin>` — `.text` / `.data` / `.bss` summary, easy to live-demo.
- `bloaty <bin>` — per-section / per-symbol attribution; great for the
  "where did the bytes go" slide.

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


