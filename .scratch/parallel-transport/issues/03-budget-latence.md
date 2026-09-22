# Budget de latence interne DSP2→DSP1

Type: grilling
Status: resolved
Blocked by: 02

## Question

Trancher la borne de latence interne admissible entre le producer (DSP2) et le
mixer (DSP1) : 0 frame (lockstep actuel — a tué le proto), 1 frame codec
(~23 µs machine), ou plus. Décision TECHNIQUE, à instruire sur critères
mesurables, pas sur préférence : impact sur le timing séquenceur/MIDI perçu,
sur les tests timing existants (`mdMidiTimingTest`, `mdHostRxTimingTest`,
policies deadlines MM), et sur la profondeur des rings nécessaire.

L'utilisateur a explicitement délégué cette décision au technique (Q4 du
charting). La résolution passe par l'analyse de la cartographie (02) et, si
besoin, un mini-prototype de mesure.

## Answer

Ratifié par l'utilisateur le 2026-09-22, instruit par
`docs/research/transport-map.md` (branche `research/transport-map`) :

**Cible de design : 1-2 frames codec de latence interne DSP2→DSP1**
(23-45 µs machine). Bornes dures issues de la cartographie :

- Profondeur résiduelle du ring de lien < 16 frames — au-delà, purge
  anti-stall MD immédiate (`blockingPop`, mdhardware.cpp:352-387).
- Dérive d'horloge entre les deux timelines DSP < 1 fenêtre DMA4 (32 samples)
  en fenêtre active.
- Marge entrée hôte : 64 frames (Port D) — non contraignante à 1-2 frames.

Latence host du plugin inchangée : le pipeline retarde le contenu producer de
1-2 frames dans le mix interne (décalage voix/séquenceur 23-45 µs, ordre de
grandeur sous le seuil perceptible ~1 ms et sous la variance du hardware).

Gates de validation : `mdMidiTimingTest`, `mdHostRxTimingTest`, soaks audio
MD+MM, paire `mmSineFirmwareTest`/`mmSineMidiFirmwareTest`.
