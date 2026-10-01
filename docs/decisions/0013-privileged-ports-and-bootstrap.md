# 0013 — `host_priv` / `device_master` → capabilities delegated at boot by the root task

Date: 2026-09-23
Status: accepted (2026-10-01)
Covers: §6 row *`host_priv`, `device_master` privileged ports*.

## Context

Mach has two ambient privileged ports: `host_priv` (privileged host control)
and `device_master` (the right to open any device). Whoever holds one holds
broad authority over the whole machine. §6 maps them onto explicit capabilities
handed out at boot by the root task.

**Evidence.** `task_get_special_port` fires 2 302 times and
`task_set_special_port` 7 085 times in the build; neither distinguishes which
port. No dedicated `host_priv` call appears. The privileged ports are acquired
once and then passed around as ordinary rights, which is precisely the problem:
they are indistinguishable from any other port right at the point of use, so
nothing observes how far they spread.

§9.1 already names the root task as trusted-for-everything and notes it "must
be small, do nothing but partition and delegate, and then drop what it no
longer needs". This decision is what that sentence means in practice.

## Options considered

**A — Keep a single privileged capability.** One `host_priv` equivalent, handed
to the servers that need it. Simple, and reproduces Mach's problem: the
authority is undivided, so every holder holds all of it.

**B — No privileged capability at all; every authority is a distinct
capability from the start.** There is no "privileged port", only IRQ, MMIO,
IO-port, DMA, frame, and task-creation capabilities, each over an explicit
resource.

**C — B, plus a boot-time root task that holds the complete set exactly once
and partitions it.**

## Decision

**C.**

- **At boot the kernel creates the root task and gives it the complete
  capability set**: all physical memory as frame capabilities, all IRQs, all IO
  and MMIO ranges, and the authority to create tasks. This is the only moment
  at which undivided authority exists.
- **The root task's whole job is to partition and delegate**, then drop what it
  does not keep. It starts RS, and hands RS the authority to start servers and
  grant each the specific hardware capabilities its class requires (§7.2, 0012).
  After that the root task holds nothing it does not need, and ideally exits or
  blocks forever.
- **There is no `host_priv` equivalent and no `device_master` equivalent.**
  There is no capability whose meaning is "privileged"; there are only
  capabilities over named resources. A server that needs one IRQ gets one IRQ.
- **Delegation is recorded, not ambient.** Because authority arrives as
  capabilities at task creation, the boot-time delegation graph is a static,
  reviewable artefact: who was given what, by whom. Write it down as part of
  the boot configuration; it is the thing a reviewer reads to check §9.1's
  claim that compromise below `auth` stays bounded.
- **RS's authority is the one that must be scrutinised.** §9.1 ranks RS second
  only to the root task, because it can start servers and grant hardware. Its
  capability set should be the minimum needed for the server classes it
  actually supervises, not "everything the root task had".

## Consequences

- Easier: the trust model in §9.1 becomes enforceable rather than aspirational,
  because "what can this component do" is answerable by listing its
  capabilities; no ambient authority means no accidental privilege leak
  through a passed port right.
- Harder: the boot sequence gains a genuine design problem — the delegation
  graph must be complete before anything runs, and getting it wrong means a
  server cannot do its job in a way that may only show up under load. This is
  Phase 2 and Phase 10 work and should have its own test: boot with each
  server's capability set reduced by one and confirm the failure is clean.
- Anything in the Hurd that fetches a privileged port and assumes it confers
  broad authority must be found and narrowed. `task_get_special_port`'s 2 302
  calls are the place to start looking, and the static call sites will say
  which are privileged-port fetches.

## Priority check

Respects the order. Option A is far less work in the Hurd and loses to
Resilience and to the §9 trust model: undivided authority makes driver
isolation (§7.4) and the bounded-compromise property (§9.1) unprovable, and
those outrank Reuse.
