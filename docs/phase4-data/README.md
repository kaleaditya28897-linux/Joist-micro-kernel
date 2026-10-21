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
