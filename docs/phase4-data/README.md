# Phase 4 data

Raw evidence behind the numbers in `docs/phase4-plan.md` *Progress*, as for
Phases 1-3. Environments are in `docs/testbed.md`: the build VM `joist-dev2`
on the P53, and images booted on the P53's own CPU through KVM
(`tools/p53/boot.sh`).

## The design survey (2026-10-19): `survey.md`

Written by the P53's Claude Code for the plan's draft: eight questions on
Phase 4's kernel side (page-sized COW, the calls needed, revoke and the
batched copy, who pays for page tables, 0008's escape hatch, grants and
0006, the COW fault path, testing COW), each with options and a
recommendation. What it says of Joist cites the tree and was checked. What
it says of other systems is from memory, unchecked, and marked so where it
is least sure.

## The exit run (2026-10-30): `step7-exit-run/`

`tools/exit-run.sh` at f8d55b5, at the sizes the owner decided: `cowstress`
1000 rounds on 1, 2 and 4 CPUs and 250 on the checking kernel, booted on
the P53; the refinement test with 10 000 seeds (1 000 under msan and tsan),
on the workstation. All passed; its README has the figures.
