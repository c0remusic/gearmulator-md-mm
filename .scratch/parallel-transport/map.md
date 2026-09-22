# Map : refonte transport parallèle MD/MM

Label: wayfinder:map

## Destination

Spec de refonte du transport MD/MM permettant l'exécution parallèle des trois
processeurs émulés (UC + DSP1 + DSP2), précédée d'un go/no-go étayé.
Critère de succès : parité de coût CPU thread audio avec l'émulation Virus
(OsTIrus) mesurée sur la machine de référence (Ryzen 7 3700X), audio conforme
aux suites existantes (soaks, timing), zéro régression.

## Notes

- Domaine : émulation DSP56303/Coldfire, scheduler déterministe single-thread
  actuel dans `source/elektron/md/mdLib/mdhardware.cpp` (voir ticket 02).
- Skills à consulter par session : `mattpocock-skills:codebase-design` pour les
  tickets de design (04-06) ; `grilling` + `domain-modeling` par défaut.
- Garde-fou : `mdAudioFirmwareTest`/`mmAudioFirmwareTest` + `mmSine*` + suites
  timing ; 5 échecs ctest préexistants documentés (SEGFAULTs flash/panel,
  heap corruption VST, sysex lifecycle) — baseline, pas des régressions.
- Design unifié d'emblée : chaque décision instruite contre les DEUX policies
  (MD relâchée 125 µs, MM stricte 30 µs + deadlines ESSI exactes + strobe PDRC).
- Données de session (2026-09-22) : split playback UC 30,5 % / DSP1 33,7 % /
  DSP2 26,4 %, catchup 0,5 % (`temp/sp_playback.log`) ; 3 designs de
  parallélisation par slices invalidés — branche `proto/parallel-dsp2`
  (commit 8807c954) ; acquis mergés : 4ecc289e, 54dc3c45.
- Partage amont (joelanders) : APRÈS la spec (décision de cadrage Q3=B).

## Decisions so far

<!-- une ligne par ticket résolu : [titre](issues/NN-slug.md) : gist -->

- [Baseline Virus sur machine de référence](issues/01-baseline-virus.md) :
  OsTIrus = 23 % realtime, MD = 94 % (offline, idle, 3700X) — parité = ×4,1 à
  combler ; parallélisation seule plafonne ~34 %, fast-forwards requis en plus.
- [Cartographie du transport actuel](issues/02-cartographie-transport.md) : la
  contrainte n'est pas la profondeur des rings (32768, pas 2048) mais l'ordre
  et la datation des livraisons — rendezvous = exécutions inline du pair
  (catch-ups à chaque accès HI08, livraison lien datée, epochs PDRC),
  fréquence énorme pour 0,5 % du wall ; doc :
  `docs/research/transport-map.md` (branche `research/transport-map`).
- [Budget de latence interne DSP2→DSP1](issues/03-budget-latence.md) : cible
  1-2 frames codec ; bornes dures : ring < 16 frames (purge MD), dérive
  d'horloge < 32 samples (fenêtre DMA4) ; latence host inchangée ; ratifié.

## Not yet specified

- Stratégie de validation/migration incrémentale (feature flag, A/B série vs
  parallèle, plan de tests au-delà des suites existantes) — dépend du modèle
  de découplage retenu (04).
- Accès concurrents hors audio : snapshots UI front panel, state save/restore,
  `deferredPreparedState` — à instruire une fois le modèle choisi.
- Forme du partage amont (PR, issue, discussion) — après spec.

## Out of scope

- Issue amont immédiate avec les mesures (cadrage Q3 : spec d'abord).
- Micro-optimisations JIT restantes (fast-forward poll GPIO `0xbb-0xbf`,
  spécialisation DO-memset) — effort séparé si repris.
- Boot-warmup (pics de lancement ×200 budget) — indépendant du transport,
  effort séparé déjà identifié en session.
- Correction des 5 échecs ctest préexistants de la branche alpha.
