# Rédaction de la spec

Type: task
Status: resolved
Blocked by: 07

## Question

Assembler la spec finale de la refonte transport à partir des décisions des
tickets 03-06 : architecture, invariants, budget latence, plan de
validation par policy, points de divergence MD/MM, critères d'acceptation
chiffrés (baseline du 01). Destination du map — livrable prêt pour un effort
d'implémentation séparé (et pour le partage amont, forme à décider hors map).

## Answer

Fait le 2026-09-22. Spec livrée : `docs/design/parallel-transport-spec.md`
(en anglais — destinée au partage amont et cohérente avec les docs du
dépôt). Onze sections : objectif et critères d'acceptation chiffrés,
architecture (threads, D/L_lead), gates et liveness, transport de lien
(règle de pop 4 temps, PDRC, miroirs DCR), service HI08, policy MM,
control-plane quiesce, passe de visibilité mémoire, plan de migration 0-4
sous flag MDMM_TRANSPORT avec checkpoints chiffrés, questions ouvertes
d'implémentation, hors périmètre. Assemble les résolutions des tickets
01, 03, 04, 05, 06, 07 et 09 ; toutes ratifiées individuellement.
