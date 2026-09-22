# Validation du modèle contre les contraintes MM

Type: grilling
Status: resolved
Blocked by: 04

## Question

Vérifier que le modèle retenu en 04 tient sous la policy Monomachine :
deadlines ESSI exactes (`exactEssiCycleDeadlines`), quantum 30 µs, rendezvous
strobe PDRC (epochs `m_mmLinkStrobeEpoch`), backpressure host-TX. Précédent :
`maxDoIterations=64` a cassé `mmSineFirmwareTest`/`mmSineMidiFirmwareTest`
(commit 54dc3c45) — la marge MM est étroite et mesurable par cette paire.

Sortie : soit le modèle unifié tient tel quel, soit liste explicite des points
de policy divergents (et leur coût), soit signal de re-design du 04.

## Answer

Ratifié par l'utilisateur le 2026-09-22. Vérifié contre le code
(mdtransportpolicy.h ; mdhardware.cpp:440-580 — gate TX producteur, strobe
PDRC, forward Port C).

**Le modèle unifié tient. Zéro chemin de code divergent : les divergences
restent des constantes TransportPolicy existantes, plus trois points chiffrés
ci-dessous. Pas de signal de re-design du 04.**

Contraintes MM passées contre « Convoi » :

1. `exactEssiCycleDeadlines` : EsxiClock per-DSP, tick dans l'exec
   périphérique du thread propriétaire (mddsp.cpp:50-51). Orthogonal au
   threading, conservé tel quel au handover.
2. Quantum 30 µs → L_lead MM = 1,32 frame : churn estimé ~7 000 parks/s
   (juge performance) — largement sous le seuil mortel du proto (cv
   round-trip par quantum ; ici futex bit-waiter, réveil seulement si un
   waiter est parqué). À mesurer à l'étape 4 de migration ; sweep de L_lead
   sous mmSine* si le churn se confirme.
3. `maxDoIterations=4` conservé : les sorties fréquentes du dispatcher
   donnent gratuitement la granularité de contrôle des gates. 4→64 =
   expérience de l'étage 2, post-quantum, gate mmSine* (précédent 54dc3c45).
4. Strobe PDRC : détection d'edge et purge restent dans le contexte DSP1 —
   le consommateur purge son PROPRE ring d'entrée, SPSC-sûr
   (mdhardware.cpp:541-566) ; `m_mmLinkAwaitFresh`/`m_mmLinkStrobeEpoch`
   déjà atomiques acquire/release (:451, :563-564) — seul chemin du
   transport déjà thread-prêt. Publications nouvelles requises : miroirs DCR
   croisés — dma4Active (DCR du mixer lu par le gate TX producteur
   :453-455), dma1Idle (DCR du producteur lu par le strobe mixer :548-550).
   Le forward Port C mixer→producteur (:567) devient la mailbox datée du 04.
5. Round-trip strobe→burst = L_lead + D_mm ≈ 2,3 frames contre une enveloppe
   série de 1,32 : SEUL point non prouvé du modèle. Divergence assumée —
   canari `mmSineFirmwareTest`/`mmSineMidiFirmwareTest`, fallback **D_mm=0
   par construction** (dégénère vers l'élasticité wall-clock pure, toujours
   parallèle ; coût : parks plus fréquents en butée de gate).
6. Backpressure host-TX (seuil 4, release 200 000 cycles UC) : le park migre
   dans le worker, l'échappée de liveness de la greffe 1 du 04 est
   obligatoire. TimedHostRx profondeur 1 et seuil HREQ 1 conservés — le
   mécanisme MM est déjà LE modèle que le ticket 05 généralise.
7. Ordre CVR / HC pending-future : drain HORX pré-CVR reste DSP-side,
   in-order sur le flux daté (ticket 05, pt 4) ; ReadCvrCallback alimenté
   par hostCommandAcceptedCycle publié, logique inchangée (mddsp.cpp:95-103).
8. États MM déjà atomiques : aucune passe de visibilité requise
   (contrairement aux bools MD nus). Le re-check d'époque post-catch-up
   imbriqué disparaît avec l'imbrication elle-même — simple comparaison
   d'epoch au pop daté.

Filet de déploiement : rollout MD d'abord, MM en série sous flag tant que ses
gates sont rouges. Ordre des gates MM : paire mmSine* d'abord, puis
mmAudioFirmwareTest, soaks MM, suites timing.
