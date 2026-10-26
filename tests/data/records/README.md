# A fixture target for the record decoders

One firmware image as the host sees it, without a board: the bytes a decoder reads, the symbols it finds them by.
Kvasir_SDK's `tools/test_kvasir_bench.py` feeds kvasir_bench's text goldens from it; the printer's own decoder tests
and the `records` control-protocol golden read the same bytes, so the text and the C++ decoding are tested on one
input.

| file | what |
|---|---|
| `fixture.json` | `memory`: the flash and RAM regions (address, what, bytes as hex); `code`: the text range; `registers`: the 19 core registers of a halt (r0-r12, sp, lr, pc, xpsr, msp, psp), inside an IT block of an exception handler |
| `fixture.nm` | the symbol table `llvm-nm -C -S --defined-only` prints for the image's ELF |
| `fixture.lld.map` | an lld map of the image: symbol lines with sizes |
| `fixture.gnu.map` | a GNU ld map of it: no sizes in the merged `.noInit`, no ring descriptors (gcc keeps none) |

What the image holds:

- `Kvasir::Fault::lastFault` (KFLT): a precise BusFault at `App::Motor::step()+0x24`, frame, callee-saved, fault
  registers, stack limits, snapshot taken; core 0, Armv8-M; and `lastStack<(unsigned short)64>` (KSTK) with 16 valid
  bytes.
- `Kvasir::Panic::lastPanic` (KPNC): cause 7 (fault), detail 3, raised from `App::check()`, at 123456789 us.
- `Kvasir::Health::lastStarvation` (KHLT), `lastRecovery` (KHRV, an `onStarved` a reset cut short), and
  `Kvasir::BootGuard::state` (KBTG, the last failure a starvation); the check names are strings in flash.
- `Kvasir::Ubsan::ubsanReports`: two reports.
- Both stacks painted (`Kvasir::StackUsage`): core 0 2 KiB with 608 bytes used, core 1 1 KiB with 128 used; the
  bottom word is the StackProtector's sentinel.
- Two `Kvasir::Trace::Ring`s: `edges` (4 fields, capacity 1024, 1500 written: a full-size 16 KiB storage that has
  wrapped) and `isr` (2 fields, capacity 8, 5 written).

Addresses are target addresses only. The bytes were written once by a generator kept with the plan that made them;
change them by hand only together with every golden that reads them.
