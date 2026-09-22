# Cartographie du transport actuel

Type: research
Status: resolved

## Question

Documenter précisément le modèle de transport existant dans
`source/elektron/md/mdLib/` (surtout `mdhardware.cpp`) : inventaire exhaustif
des points de rendezvous inter-processeurs et de leurs contraintes réelles.

À couvrir :
- Rings ESSI inter-DSP (blockingPush/blockingPop, on-demand wire semantics,
  profondeur 2048, idle words / clock rendezvous)
- Pump HI08 (`pumpDsp2HostRequest`, queue 16 mots, HREQ→IRQ4,
  `hostReceiveIrqMinWords`)
- Catch-ups (`schedCatchUpDsp`, `schedCatchUpDspToDsp`, fréquences mesurées :
  catchup 0,5 % du wall en playback, `blockedByDspTx` ~15/600k probes)
- Machinerie PDRC (epochs, pending/visible, strobe MM)
- Policies (`mdtransportpolicy.h`) : différences MD/MM chiffrées
- Timeline audio entrée hôte (`m_hostAudioInput*`, overflow/underflow —
  cause de l'échec du proto v2)
- Skip idle UC (`idleSelfBranchInstructions`, conditions de transparence MD)

Produit : document de référence sur branche `research/transport-map`, pointé
d'ici. C'est la matière première des tickets de design 03-06.

## Answer

Document complet : `docs/research/transport-map.md` sur la branche
`research/transport-map` (commit 06f9b5c9, non poussée). Découvertes clés :

- Tout le transport tient dans un invariant : le scheduler laggard-first
  (quantum 125 µs MD / 30 µs MM, clamp 100 000 cycles) borne la dérive entre
  processeurs, et ce sont ces bornes qui rendent suffisants la marge ADC de
  64 frames, la fenêtre DMA4 de 32 samples et le seuil HREQ de 3 mots.
- Les rendezvous sont des exécutions inline du pair dans le contexte de
  l'appelant : chaque accès HI08 de l'UC exécute le DSP cible
  (`schedCatchUpDsp`), chaque mot de lien exécute le consommateur
  (`schedCatchUpDspToDsp`, garde de réentrance). Coût faible (0,5 % wall)
  mais fréquence énorme — c'est le point dur d'une parallélisation.
- La profondeur des rings n'est jamais la contrainte : capacité réelle 32768
  frames (le « 2048 » du ticket vient d'un commentaire périmé,
  mdhardware.cpp:254). La contrainte est l'ordre et la DATATION des
  livraisons (wire on-demand MD OS 1.63, TimedHostRx MM, epochs PDRC).
- Les machines pending/visible/epochs (Port C MD, strobe MM) reconstruisent
  l'atomicité d'un edge GPIO que deux DSP réels observent simultanément —
  problème que le parallélisme recrée sous forme de visibilité cross-thread.
- Le MM est structurellement ~4× plus serré que le MD (30 µs, deadlines ESSI
  exactes, maxDoIterations 4, flux hôte lossless avec backpressure seuil 4
  mots / release 200 000 cycles UC) ; états MM déjà atomiques, états MD non.
- Seul vrai point bloquant restant : le ring codec ESSI1 du mixer, vidé par
  `schedDrainCodecOutput` pour ne jamais parquer l'unique thread.
