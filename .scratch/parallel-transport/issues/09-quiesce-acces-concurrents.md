# Quiesce et accès concurrents hors audio

Type: grilling
Status: open
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
