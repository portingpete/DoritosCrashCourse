# Generator provenance

The two generator patches are copied unchanged from The Darkness Recomp,
revision `4f711819bbc6ea12633a3b031454b86b5c0d929e`. They apply to
hedge-dev/XenonRecomp `c5bfd90d87f2ed0db8cff5c19ea3aff0e161e527`.

Apply `xenonrecomp-darkness.patch` followed by
`xenonrecomp-corrections.patch`. These contain tool changes, no game code or
assets. They preserve explicit failures for unsupported instruction semantics.
The patches and upstream generator are GPLv3; see the project COPYING file.

The PPC context template is adapted from the same project. Doritos memory faults
and MMIO operations fail closed through the native runtime instead of silently
returning fabricated values.

The final `xenonrecomp-doritos.patch` adds missing update loads/stores, counted
branches with exact CR-bit selection, carry arithmetic, reverse halfword loads,
vector byte shifts, Xenon halfword-element loads, and signed-byte saturating
subtraction with sticky VSCR SAT. The supplied context template includes VSCR.
The emitted-instruction regression tests compile the generator's actual output
and compare it with independent boundary-value oracles.

`xenonrecomp-switch-order.patch` also recognizes the independent `rlwinm` index
operation scheduled between the `lis`/`addi` address pair in an inline absolute
switch. Its first observed failure was the original audio-format switch: it
looked like an indirect function call because its case bodies had no function
mapping. The replacement still requires exact register/mask patterns and an
inline table ending at the first case body.

`xenonrecomp-switch-bounds.patch` permits intervening shared error/default code
when the original unsigned compare and greater-than branch prove the exact
table count. It retains backward case edges and rejects ambiguous extents.

`xenonrecomp-lfdux.patch` implements indexed double loads with base-register
update after a successful access. Offset-switch recovery made an original
`lfdux` instruction reachable; the emitted regression contract now covers it.
