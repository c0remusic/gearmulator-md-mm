# Modèle de découplage cible

Type: grilling
Status: open
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
