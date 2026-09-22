# MD/MM Parallel Transport Redesign — Specification ("Convoi")

Status: ratified 2026-09-22. Produced by the `parallel-transport` wayfinder
map (`.scratch/parallel-transport/map.md`); every decision below was ratified
ticket by ticket (01-09). Architecture selected by a panel of three
independent designs judged under correctness/performance/feasibility lenses
(unanimous verdict), then hardened with mandatory grafts from the runner-ups.
Transport cartography: `docs/research/transport-map.md` (branch
`research/transport-map`).

## 1. Goal and acceptance criteria

Baseline (Ryzen 7 3700X, `pluginTester -seconds 30 -blocksize 128 -samplerate
44100`, offline): OsTIrus 23 % realtime, Machinedrum 94 %. Playback wall
split: UC 30.5 % / DSP1 33.7 % / DSP2 26.4 % / catch-ups 0.5 %.

- Parity metric: **most-loaded core** (the literal "audio-thread cost" is
  trivially winnable by emptying the audio thread).
- This effort delivers the parallel transport: expected **34-37 % realtime**
  (×2.6-2.8), critical path DSP1 ≈ 31.7 realtime points.
- Full Virus parity (23 %) additionally requires a separate component
  fast-forward stage (DSP1 −27 %, UC −20 %); out of scope here, chartered
  after this spec.
- Gates: `mdAudioFirmwareTest`/`mmAudioFirmwareTest`, `mdMidiTimingTest`,
  `mdHostRxTimingTest`, `mmSineFirmwareTest`/`mmSineMidiFirmwareTest`, MD+MM
  audio soaks — all green at every migration step. The 5 pre-existing ctest
  failures of the alpha branch (flash/panel SEGFAULTs, VST heap, sysex
  lifecycle) are baseline, not regressions.
- External MIDI sequencing (DAW-driven, internal sequencer stopped) is a
  first-class workload: the MIDI→UC→HI08→DSP2→link→DSP1 path is identical
  with or without the internal sequencer.

## 2. Architecture overview

Three execution domains: **audio thread** (ColdFire UC + coordination +
codec drain), **worker DSP1** (mixer), **worker DSP2** (producer). No 4th
thread; prepared (deferred-state) machines never own threads.

Every cross-domain delivery is a **machine-time-dated datum**. Two separate
budgets, deliberately distinct:

- **D** — content offset (the ratified 1-2 codec-frame internal latency,
  ticket 03). Applied on the DSP2→DSP1 direction ONLY; the DSP1→DSP2
  back-channel is purely causal (due = production time). Start D=1 frame,
  2 in reserve. Host latency of the plugin is unchanged.
- **L_lead** — observation-drift budget, equal to the shipped serial quantum
  envelope: 5.51 frames MD / 125 µs, 1.32 frames MM / 30 µs. Dating makes
  drift affect only buffering and control-edge observation instants, never
  the temporal position of data.

Workers free-run toward the published block target; waits are futex-style
(`std::atomic::wait`/WaitOnAddress, waiter-bit, edge-triggered wake only
when a waiter is parked). No per-quantum handoff (prototype lesson: one
handoff per quantum cost ~×45). All MD/MM divergences are `TransportPolicy`
constants (D, L_lead, thresholds, with D=0 fallback by construction) — never
divergent code paths.

## 3. Thread model, gates, liveness

- Audio thread: `processUC` intact (scheduled MIDI, panel, MIDI ingress,
  host pump, idle skip), publishes the integer block target, drains the DSP1
  ESSI1 codec ring (its only mandatory wait); DSP2 is never joined at block
  end.
- Workers: `execUntilCycles(next frame boundary)` (shipped bounded-JIT
  path), then gate checks and a release-store position publication
  (1/frame ≈ 44.1 k/s).
- Gates, checked at frame boundaries:
  1. pos ≤ block target (preserves the 64-frame host-input safety margin;
     JIT overshoot ≪ 64).
  2. pos ≤ posPeerDsp + L_lead.
  3. pos ≤ posUC + L_lead.
  4. MM only: hostTxBacklog ≤ 4, released after 200 000 UC cycles — the
     release MUST have an escape independent of the published UC position
     (mandatory graft: otherwise UC futex-parked → frozen UC position →
     clamp never releases → three-way freeze).
- The implementation must state a deadlock-freedom argument: every wait
  predicate satisfiable by strictly positive peer progress; the global
  minimum is never gated.
- Boot: un-booted DSP = parked (`m_schedRunnable`). Boot runs under the
  serial scheduler; ownership transfers to workers at a **barriered
  switchover AFTER** MD OS 1.63 rendezvous arming
  (mdhardware.cpp:913-963), origin latching, and the first MM strobe
  (mandatory graft: the arming callback mutates the producer's ESSI from
  DSP1 context).

## 4. Link transport (ESSI0)

- `TimedLinkRing`, SPSC per direction, entries {word, stamp (LinkTime),
  epoch, flags fresh/dmaFed}. Producer stamps at TX in its own context
  (existing `setWriteTxCallback` seams; dsp56300 submodule untouched on
  this path).
- Four-step pop rule at consumer time W:
  (a) drop dead-epoch heads — purge by dating, not by depth;
  (b) headStamp + D ≤ W → pop;
  (c) else, if published producer position ≥ W − D → slot genuinely empty →
      hardware skip-on-empty (`setRxDataAvailableCallback`) — **silence is
      never fabricated**, boot included;
  (d) else futex on producer position — never taken in steady state thanks
      to the D slack.
- Guard relocation: the producer keeps only local tests and control mirrors
  (fresh = local `getLastTxWrittenMask`, epochs, DMA-armed via DCR mirror).
  Everything that read consumer state moves to the pop, consumer-side (ROE
  faithful to silicon on local RDF, await-fresh via the carried fresh flag,
  epoch-of-entry vs current-epoch compare).
- Removed: `schedCatchUpDspToDsp`, the on-demand idle clock-coupling
  callback (mdhardware.cpp:958-961; replaced by the L_lead gate),
  `m_schedInLinkDelivery`.
- Port C / PDRC: the mixer's Port C write publishes an atomic mailbox
  {level, epoch}; release happens in DSP2's `hostInputSource` context
  against the DMA4 mirror. Epoch machinery (pending/visible/strobe) is
  preserved unchanged. The MM strobe purge stays in mixer context (the
  consumer purges its OWN input ring — SPSC-safe, mdhardware.cpp:541-566);
  `m_mmLinkAwaitFresh`/`m_mmLinkStrobeEpoch` are already acquire/release.
- Anti-stall purge becomes dated: only words due ≤ consumer-now count;
  in-flight future words are invisible to it. The armed MD 1.63 path keeps
  its rendezvous bypass; MM keeps the 1024-frame window.
- New publications: **cross DCR mirrors** — dma4Active (mixer DCR read by
  the producer TX gate, mdhardware.cpp:453-455) and dma1Idle (producer DCR
  read by the mixer strobe, :548-550). `m_esaiFrameIndex` becomes atomic.

## 5. HI08 host service

1. **The pump stays on the audio thread.** `pumpDsp2HostRequest`
   (mdhardware.cpp:1055-1095) keeps its dirty-flag fast path and
   `notifyHostPumpStateChanged`. New content: take() of due words
   (readyCycle ≤ ucNow) from the staging queues, `m_rxData` bounded at 16,
   `relatchRx`, HREQ ≥ 3 (MD) / RXDF (MM) computed on `m_rxData` only,
   `setExternalIrq4`. A DSP running ahead never raises an edge in the UC's
   future — the TimedHostRx invariant extended to the MD. The mixer path is
   drained the same way (only the HREQ→IRQ4 wire is DSP2-specific).
2. **DSP-side staging, on the DSP's thread.** `setWriteTxCallback` (already
   in place for MM, mddsp.cpp:171-176) stamps {word, readyCycle =
   hostRxReadyCycle} into a dated SPSC queue per DSP. MD depth: 16
   (= hostReceiveQueueCapacityWords; max 32 in flight). The 4-word variant
   is rejected: it is the MM policy depth, and an MD park is illegal.
3. **MD backpressure = natural HTDE.** Staging queue full ⇒ HOTX not
   drained ⇒ the firmware paces itself on HTDE, as on silicon. Never park
   the MD worker (mdtransportpolicy.h: threshold is MM-only). MM keeps its
   depth-1 latch (TimedHostRx) plus the backlog>4 park with the liveness
   escape of §3.
4. **UC→DSP: one dated in-order stream per DSP** {DataWord|Cvr|IcrWrite},
   stamped in UC time, applied by the worker at max(stamp, posDsp) —
   replaces the inline runs of `writeWordToDsp`/`hdiSendIrqToDSP`
   (mddsp.cpp:326-413). Host-command arbitration and the MM pre-CVR HORX
   drain stay DSP-side, executed naturally on the DSP thread.
   `waitForHostCommandIdle` becomes a wait on published
   hostCommandBusy/hostCommandAcceptedCycle.
5. **Status reads (`hdiUcReadIsr`, mddsp.cpp:415-444): EXACT semantics.**
   waitForDspTime(idx, ucNow), then read the published snapshot (RXDF,
   TXDE/TRDY recomposed from HORX depth, HF2/HF3). Publication is
   **eventful** on every host-port mutation, never per-frame (otherwise
   TurboMidi collapses to 1 word/frame). Futex waiter-bit wake; N
   consecutive polls ⇒ park on journal advance. Bounded-stale reads are the
   documented fallback only if the scorecard shows prohibitive churn at
   migration step 2 — measured, not speculated.
6. **onUCRxEmpty(_needMoreData)** (mddsp.cpp:281-313): wait posDsp ≥ ucNow
   (clamp `catchUpMaxDspCycles` kept), then take the due word; absent at
   that machine time ⇒ RXDF stays clear and the firmware re-polls, as on
   silicon.
7. **UC idle skip** (mdhardware.cpp:1352-1408): the `dspTxClear` guard
   becomes local (no due staged words + dirty clear + no deferred); a stale
   non-empty view delays the skip by one probe (benign), never a wrongful
   skip. Skip bound = min(target, clamp, scheduled-MIDI deadline, **next
   staged readyCycle across both DSPs**) — restores the bound the quantum's
   disappearance removed. The per-step pump stays a no-op on empty queues:
   the 82 % idle-skip transparency is preserved.

Deadline guarantee: a word is visible at its readyCycle and IRQ4 fires at
the latest on the next probe; envelope ≤ the current serial quantum
(5.51 frames MD / 1.32 MM), never worse than shipped.

## 6. Monomachine policy

The unified model holds with zero divergent code paths; divergences are
`TransportPolicy` constants plus the following facts:

- `exactEssiCycleDeadlines` and `maxDoIterations=4` unchanged (per-DSP
  EsxiClock ticks in the owner thread's peripheral exec; frequent dispatcher
  exits give free gate granularity). 4→64 is a stage-2 experiment gated by
  mmSine* (precedent: commit 54dc3c45).
- L_lead MM = 1.32 frames; estimated ~7 000 parks/s — far below the
  prototype's fatal regime; measure at step 4, sweep L_lead under mmSine*
  if churn shows.
- **Only unproven point of the whole model**: the strobe→burst round trip
  at L_lead + D_mm ≈ 2.3 frames vs the serial 1.32 envelope. Canary:
  `mmSineFirmwareTest`/`mmSineMidiFirmwareTest`; fallback **D_mm=0 by
  construction** (degenerates to pure wall-clock elasticity, still
  parallel; cost: more gate parks).
- Host-TX backpressure (threshold 4, release 200 000 UC cycles) moves into
  the worker with the §3 liveness escape. TimedHostRx depth 1 and HREQ
  threshold 1 kept — the MM mechanism is the model §5 generalizes.
- MM states are already atomic; no visibility pass needed (unlike the MD
  bare bools). The nested-catch-up epoch re-check disappears with nesting
  itself: a plain epoch compare at the dated pop.

## 7. Control plane (quiesce)

- **Common-frame barrier**, not "park at gates": `withMachinePaused(fn)`
  publishes a stop target T = next frame boundary ≥ all positions; workers
  run to T and park; the audio thread joins. Captured state is exactly what
  the serial scheduler exposes between blocks. Resume republishes the block
  target and wakes.
- Per-operation policy:
  - `getState` (copyPatchRam/copyFlashData/copyUserFlash/overlays,
    mddevice.cpp:234-265) and `cancelMidiSysexTransfer`: full quiesce.
  - `setState`: preparation stays an offline new Hardware (transactions,
    mddevice.cpp:269-343); prepared machines boot and advance in SERIAL
    mode (advance, :670-676) and never own threads. Commit =
    `exchangePersistentFlashState` + swap under quiesce of the live
    machine; the promoted machine receives its workers via the §3 barriered
    switchover.
  - Front panel/LCD: UI reads via the existing async snapshots,
    bounded-stale accepted; inputs already SPSC (PanelInputQueue).
  - Scorecard/diagnostics: relaxed reads, tearing accepted.
- Callers: control plane stays in the device/audio context between blocks
  (existing plugin lock); the UI never touches the machine directly
  (`callAsync` patterns unchanged).

## 8. Memory-visibility pass

Promote the MD bare bools into per-owner structs (`Dsp1LinkState`,
`Dsp2LinkState`, `UcHostState`) rather than scattered atomics — an
auditable shape for the promotion (flush epoch, Port C pending/visible
packed, on-demand rendezvous flags).

## 9. Migration plan

Flag `MDMM_TRANSPORT=serial|dated|parallel`
(precedent: `GEARMULATOR_MDMM_BOUNDED_JIT`, mdhardware.cpp:124-126).
Every step gated by the full suites of §1.

0. **Dating at constant threads, D=0.** TimedLinkRing + stamps +
   consumer-side disposal + published positions, all on the current single
   thread. Link path must reproduce serial audio byte-for-byte
   (catch-up-before-delivery ⇒ stamp ≤ consumer time in serial). The MD
   HI08 dating is NOT a no-op in serial (a DSP leading the UC gets its
   words deferred, IRQ4 shifts): the A/B perimeter must either exclude
   HI08 or accept an explicit re-baseline. Ships the §8 visibility pass.
1. **D>0 in serial.** Validates firmware tolerance to the content offset
   orthogonally to threading (canary mmSine*, timing suites).
2. **Worker DSP2 only** (UC+DSP1 stay serial on the audio thread).
   Checkpoint: ~57-60 % realtime. Implements quiesce (first concurrent
   setState is possible here). Scorecard measures parks/s vs the prototype
   regime.
3. **Worker DSP1.** Full pipeline; checkpoint ~34-37 %. Codec ring
   cross-thread; SPSC hardening pass on `RealtimeHostAudioInputTimeline`
   (append on audio thread / readAt on workers).
4. **MM strict.** L_lead/D_mm tuning; mmSine* pair first, then
   mmAudioFirmwareTest, MM soaks, timing suites.

Rollout: MD first; MM stays serial under the flag while its gates are red.
Auto-fallback to serial mode on telemetry spikes (purge, underflow, FIFO
overflow). Validation instrumentation: TransportScorecard extended
(parks/wakes per gate, waitForDspTime waits, drops by dated cause,
stationary TimedLinkRing depth); mirror==direct asserts in serial mode for
every published status snapshot; offline TSan on the firmware soaks
(mandatory given the MD bare-bool history).

## 10. Open implementation questions

- DCR mirrors: write-observer hook in the dsp56300 submodule's dma.cpp
  (zero-cost when unset, but shared with upstream) vs an mdLib-side polling
  mirror (less exact; sufficient for MD, to be proven for MM).
- Final L_lead/D values per machine and direction (start: L_lead = quantum,
  D = 1 frame DSP2→DSP1, causal back-channel, D_mm = 1); sweep under soaks.
- Step-0 A/B perimeter against the 5 pre-existing ctest failures.
- Post-boot UC poll density (scorecard `coldFireToDsp`) to size the N-polls
  futex escape.
- `execUntilCycles` overshoot (≤ 32-instruction JIT blocks) counted inside
  L_lead.

## 11. Out of scope

- **Stage 2 — component fast-forwards** (batch-128 trampoline, MM
  maxDoIterations 4→64, ESSI-wait fast-forward conditioned on a prior JIT
  profile): separate effort chartered after this spec; required for the
  23 % parity.
- Boot warm-up (launch spikes ×200 budget) — independent effort, already
  identified.
- Remaining JIT micro-optimizations (GPIO poll 0xbb-0xbf fast-forward,
  DO-memset specialization).
- Fixing the 5 pre-existing ctest failures.
- Plugin UI effort (Overbridge-style panel; orthogonal to transport).
- Upstream sharing form (PR/issue/discussion): decided outside this spec,
  after it, per the charter.
