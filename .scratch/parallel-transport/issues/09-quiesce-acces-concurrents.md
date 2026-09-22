# Quiesce et accès concurrents hors audio

Type: grilling
Status: resolved
Blocked by: 04

## Question

Le modèle « Convoi » (ticket 04) requiert une barrière quiesce() — parquer les
workers DSP à leurs gates — pour toute opération control-plane : save/restore
(getState), exchangePersistentFlashState, snapshots UI front panel, lectures
scorecard. À trancher :

- Forme exacte : parquer aux gates suffit-il, ou faut-il un point de coupe
  aligné sur une frontière de frame commune ?
- deferredPreparedState : boot en mode série, transfert des threads au commit —
  séquencement précis avec la bascule barriérée post-armement (greffe n°3 du
  ticket 04).
- Inventaire des lecteurs hors-audio actuels (UI front panel, state, scorecard)
  et de leurs besoins : lecture seule tolérant un léger retard vs mutation
  exigeant l'arrêt complet.
- Précondition de la phase 3 de migration (worker DSP1) — ordre relatif avec la
  rédaction de la spec (08) à fixer.

## Answer

Ratifié par l'utilisateur le 2026-09-22. Vérifié contre
mddevice.cpp:234-676 (getState, transactions setState, advance différé) et
mdhardware.h:152/289/319.

1. **Forme : barrière à frontière de frame COMMUNE**, pas « parquer aux
   gates ». Les gates laissent pos1 ≠ pos2 ≠ ucNow — un état déchiré jamais
   observable en série. `withMachinePaused(fn)` : publier une cible d'arrêt
   T = prochaine frontière de frame ≥ toutes les positions ; les workers
   courent jusqu'à T et parquent ; le thread audio rejoint. Équivalent exact
   de l'état inter-blocs du scheduler série. Reprise : republier la cible de
   bloc, réveiller.
2. **Politique par opération** :
   - `getState` (copyPatchRam/copyFlashData/copyUserFlash/overlays,
     mddevice.cpp:234-265) et `cancelMidiSysexTransfer` : quiesce complet —
     opérations rares, coût accepté.
   - `setState` : la préparation reste un Hardware NEUF hors-ligne
     (transactions, :269-343) ; la machine préparée boote et avance en MODE
     SÉRIE (advance :670-676, thread audio, inchangé) — les machines
     préparées ne reçoivent JAMAIS de threads, budget threads constant. Le
     commit = `exchangePersistentFlashState` + swap sous quiesce du live ;
     la machine promue reçoit ses workers via la bascule barriérée
     post-armement (greffe 3 du ticket 04).
   - Front panel/LCD : lectures UI via les snapshots async existants,
     bounded-stale accepté (affichage) ; entrées déjà en PanelInputQueue
     SPSC — inchangé.
   - Scorecard/diagnostics : lectures relaxed, tearing accepté, pas de
     quiesce.
3. **Appelant** : le control-plane reste dans le contexte device/audio entre
   blocs (verrou plugin existant) ; l'UI ne touche jamais la machine
   directement — patterns callAsync du repo inchangés.
4. **Séquencement** : spec du quiesce intégrée au 08 ; implémentation
   requise dès l'étape 2 de migration (premier worker = premier setState
   concurrent possible), générique avant l'étape 3.
