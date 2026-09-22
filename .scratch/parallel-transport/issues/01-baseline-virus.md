# Baseline Virus sur machine de référence

Type: task
Status: resolved

## Question

Mesurer le coût CPU réel de l'émulation Virus (OsTIrus VST3, installé dans
`C:\Program Files\Common Files\VST3\OsTIrus.vst3`, ROMs dans
`Documents\The Usual Suspects\OsTIrus\roms`) sur le Ryzen 7 3700X, avec une
méthodologie comparable aux mesures MD de session : CPU thread audio en idle et
en jeu (notes tenues + arpège), buffer 128 @ 44,1 kHz.

Hôte de mesure : `pluginTester.exe` (compilé dans
`temp/cmake_vs22/source/pluginTester/pluginTester_artefacts/Release/`) ou
standalone TUS si disponible. Produit : les chiffres cibles que la spec doit
atteindre (« parité Virus » devient un nombre).

## Answer

Mesure du 2026-09-22, Ryzen 7 3700X, `pluginTester.exe -seconds 30
-blocksize 128 -samplerate 44100`, rendu offline (fraction realtime = wall/30) :

| Plugin | Wall pour 30 s | Fraction realtime |
|---|---|---|
| OsTIrus.vst3 (installé, ROM TI utilisateur) | 6,9 s | **23 %** |
| Gearmulator MD.vst3 (optimisé, maxDo=64, optimizer off) | 28,3 s | **94 %** |

**Cible de parité : ~23 % d'un cœur. Écart à combler : ×4,1.**

Limites : mesure idle (pluginTester n'envoie pas de notes) — les deux plugins
comparés dans le même état. Le split playback MD mesuré en session (UC 30,5 /
DSP1 33,7 / DSP2 26,4 % du wall) montre que la parallélisation à 3 threads
plafonne vers ~34 % (le max des composants) : la parité ×4,1 exige
parallélisation ET réduction du travail par composant (fast-forwards). À
porter au go/no-go (07).
