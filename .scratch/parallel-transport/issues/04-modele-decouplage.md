# Modèle de découplage cible

Type: grilling
Status: resolved
Blocked by: 02, 03

## Question

Choisir l'architecture de parallélisation. Candidats connus :

1. **Pipeline rings +1 frame** : DSP2 produit la frame N pendant que DSP1
   consomme N-1 ; rendezvous relâchés aux frontières de frame.
2. **Run-ahead borné** : DSP2 avance librement dans la fenêtre d'entrée audio
   déjà queueée, borné par le service HI08 (l'échec du proto v2 documente la
   borne dure : la queue host 16 mots).
3. **Thread-per-DSP style upstream** (`dspthread.cpp` d'Osirus/OsTIrus) adapté :
   rings profonds, silence jamais fabriqué (contrainte MD documentée dans
   `mdhardware.cpp:152`).

Contraintes issues du proto (branche `proto/parallel-dsp2`) : granularité de
handoff par quantum 125 µs = ~45× trop lent (cv churn) ; lead full-callback =
overflow queue host ; le couplage réel est RARE (catchup 0,5 %) mais le
handoff fin est fréquent. Le modèle doit tenir pour les deux policies
(MD relâchée / MM stricte) — divergences de policy à expliciter.

Consulter `mattpocock-skills:codebase-design` (modules profonds, seams).
Cœur de la spec.

## Answer

Ratifié par l'utilisateur le 2026-09-22, sur verdict unanime (3/3) d'un panel
3 designs indépendants × 3 juges comparatifs (correction / performance /
faisabilité), workflow `wf_25c8315c-01f`. Classement identique chez les trois
juges : Convoi > leashed run-ahead > dspthread adapté.

**Architecture retenue : « Convoi » — pipeline à livraisons datées, 3 threads.**

- Threads : audio = UC ColdFire + coordination (processUC intact, drain codec
  non bloquant), worker DSP1 (mixer), worker DSP2 (producer). Pas de 4e thread.
- Toute livraison inter-processeurs devient une donnée datée en temps machine :
  mots de lien ESSI0 dans un TimedLinkRing SPSC ({mot, stamp, epoch, flags
  fresh/dmaFed}), mots HI08 dans des files datées (généralisation de
  TimedHostRx aux deux machines et aux deux sens), edges Port C dans une
  mailbox atomique à epochs (machinerie PDRC conservée). Les catch-ups inline
  (schedCatchUpDsp/schedCatchUpDspToDsp) disparaissent : un consommateur
  consomme ce qui est mûr (stamp + D ≤ son temps local) et n'attend (futex,
  réveil edge-triggered) que si la position publiée du producteur n'a pas
  atteint le temps requis — jamais en régime, grâce au slack D.
- Séparation D / L_lead : D = décalage de contenu (budget ticket 03, 1-2
  frames), appliqué à la SEULE direction DSP2→DSP1 (greffe dspthread :
  back-channel DSP1→DSP2 causal pur, évite un round-trip 3 frames non
  ratifié) ; L_lead = budget de dérive d'observation, repris de l'enveloppe
  série (quantum policy : 5,51 frames MD / 1,32 MM). La dérive n'affecte plus
  la position temporelle des données — l'inversion qui répond à l'échec du
  proto.
- Règle de pop en 4 temps avec preuve de vacuité par position publiée du
  producteur : jamais de silence fabriqué (règle apparue indépendamment dans
  2 designs sur 3 — c'est LA règle du modèle). Gardes de drop lisant l'état du
  pair migrées côté consommateur, au pop.
- Divergences MD/MM = constantes TransportPolicy (D, L_lead, seuils,
  fallback D=0 par construction), pas des chemins de code. Submodule dsp56300
  non touché, hors question ouverte du hook DCR (miroirs DMA1/DMA4 : hook
  amont vs polling — à trancher dans la spec).

**Greffes obligatoires** (défauts relevés par les juges, réparables sans
changer la forme du design) :

1. Liveness MM : le clamp de libération du backlog (200 000 cycles) doit avoir
   une échappée indépendante de la position UC publiée — cycle de gel à trois
   (UC futex-parqué → position UC gelée → clamp jamais libéré) identifié par
   le juge correction.
2. Skip idle UC borné par le prochain readyCycle stagé (leashed) — sans quoi
   un edge HREQ→IRQ4 est retardable de dizaines de frames une fois le quantum
   disparu.
3. Bascule série→parallèle barriérée APRÈS l'armement du rendezvous MD 1.63
   (mdhardware.cpp:913-963), le latch d'origines et le premier strobe MM
   (leashed) — corrige la race d'armement du handoff unique.
4. HI08 MD : backpressure HTDE naturelle (dspthread) — queue pleine = HOTX non
   drainé = le firmware se pace ; jamais de park worker côté MD, la policy
   l'interdit (mdtransportpolicy.h, seuil MM-only).
5. Réveil event-driven sur mutation de statut HI08 + futex après N polls
   consécutifs — DSP1 (plus lent) traîne derrière l'UC en régime : condition
   de viabilité chiffrée par le juge performance, pas une option.
6. Publication ÉVÉNEMENTIELLE des stamps de drain HI08, jamais à la frame —
   sinon l'upload TurboMidi s'effondre à 1 mot/frame.

Filets : assert miroir==lecture directe en mode série (leashed), auto-fallback
série sur pic de télémétrie (leashed), checkpoint mesurable « DSP2 seul »
(~57-60 % realtime, dspthread), rollout MD d'abord / MM en série sous flag.

**Chemin de migration** (esquisse, à finaliser dans la spec 08) : étape 0
datation à threads constants D=0 (A/B série — périmètre HI08 MD à définir, un
DSP menant l'UC voit ses mots différés) ; étape 1 D>0 en série (valide le
budget latence orthogonalement au threading, canari mmSine*) ; étape 2 worker
DSP2 seul ; étape 3 worker DSP1 (précondition : quiesce, ticket 09) ; étape 4
MM strict. Flag MDMM_TRANSPORT=serial|dated|parallel.

**Plafond honnête, à porter au go/no-go (07)** : transport seul = 34-37 %
realtime (×2,5-2,9 depuis 94 %), chemin critique DSP1 ≈ 31,7 pts + résiduel
thread audio. La parité ×4,1 (23 %) exige un étage 2 de fast-forwards par
composant (DSP1 −27 %, UC −20 % : trampoline batch 128, maxDoIterations MM
4→64, fast-forward attente-ESSI) que ce transport débloque mais ne livre pas.
La métrique de charte « coût CPU thread audio » est gagnable trivialement en
vidant le thread audio — à re-cadrer (proposition : cœur le plus chargé)
avant le go/no-go.
