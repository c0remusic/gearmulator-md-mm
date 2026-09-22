# Validation du modèle contre les contraintes MM

Type: grilling
Status: open
Blocked by: 04

## Question

Vérifier que le modèle retenu en 04 tient sous la policy Monomachine :
deadlines ESSI exactes (`exactEssiCycleDeadlines`), quantum 30 µs, rendezvous
strobe PDRC (epochs `m_mmLinkStrobeEpoch`), backpressure host-TX. Précédent :
`maxDoIterations=64` a cassé `mmSineFirmwareTest`/`mmSineMidiFirmwareTest`
(commit 54dc3c45) — la marge MM est étroite et mesurable par cette paire.

Sortie : soit le modèle unifié tient tel quel, soit liste explicite des points
de policy divergents (et leur coût), soit signal de re-design du 04.
