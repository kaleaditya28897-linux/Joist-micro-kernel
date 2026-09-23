# 0000 — Project name: Joist

Date: 2026-09-23
Status: accepted

## Context

The kernel needed a name before the first public commit, because the name is
stamped into the repository, the C symbol prefix, and the library names, and
changing it later means touching every file.

The earlier working name was **Sherpa**. Two problems:

1. It is not distinctive. Several existing software projects use it, as does
   Sherpa Space. A name that collides is a searchability problem forever.
2. It borrows the name of a real people for a piece of infrastructure, and the
   mission statement leaned on exactly that association ("does the hard,
   unglamorous carrying"). That is worth not doing.

## Options considered

Candidates were checked against the systems/OS space, not just GitHub at large,
because a collision with an OS, an architecture, or an established pattern name
is far more costly than a collision with an unrelated tool.

| Candidate | Outcome |
|---|---|
| **Joist** | Chosen. Clear in the systems space. |
| Belay | Clear, strong resilience metaphor (the rope system that catches a fall). Runner-up. |
| Ferrule, Gnomon, Longeron | Clear but obscure or long. |
| Keel | Rejected — an existing edge OS + KVM hypervisor. |
| Cairn | Rejected — two Linux distributions and a Google project. |
| Plinth | Rejected — an existing "application kernel" doing capability dispatch. |
| Lintel | Rejected — an existing utility library, and reads like a lint tool. |
| Chassis | Rejected — "microservice chassis" is established pattern terminology for the shared framework services link against, which in this design is libports. Naming the kernel after its neighbour invites permanent confusion. |
| Baluster | Rejected — a baluster is decorative infill, not load-bearing; the metaphor claims the opposite of what a microkernel should. Also collides in speech with banister/ballast. |
| Spar | Rejected — collides with SPARC in speech and with SPARK (the Ada verification subset) in a document that already discusses formal verification. |
| Hubris, Bedrock, Keystone, Talos, Kestrel, Piton | Rejected — all taken in systems/security. |

## Decision

The kernel is **Joist**. C symbol prefix `joist_`, libraries `libjoist*`, glibc
port directory `sysdeps/joist`.

A joist is a plain horizontal load-bearing member. Nobody admires a joist and
nobody thinks about a joist, and everything above it depends on it holding.

## Consequences

- Easier: a short, unambiguous prefix; no confusion with an existing OS,
  architecture, or design pattern; no borrowed cultural name to defend.
- Harder: nothing material. The rename cost was paid before the first commit,
  which is the entire reason it was done now.
- `.github/copilot-instructions.md` and this plan both record the prefix; both
  must be updated together if it ever changes again.

## Priority check

Neutral with respect to Reliability > Resilience > POSIX > Reuse > Performance.
A naming decision does not trade against any of them; it was taken early
precisely so it never has to compete with them later.
