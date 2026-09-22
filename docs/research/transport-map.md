# Cartographie du transport MD/MM

Document de référence pour la refonte « transport parallèle » (carte
`.scratch/parallel-transport/`, ticket 02). Il inventorie les points de
rendezvous entre les trois processeurs émulés — UC ColdFire, DSP1 (mixer),
DSP2 (producer) — tels qu'ils existent dans `source/elektron/md/mdLib/`, avec
pour chacun : qui l'appelle, depuis quel contexte d'exécution, à quelle
fréquence, et quelle contrainte il impose à une exécution parallèle.

Sources primaires : `mdhardware.cpp` / `mdhardware.h`, `mddsp.cpp`,
`mdtransportpolicy.h`, `mdmc.cpp`, et le submodule `source/dsp56300`
(`essi.h/.cpp`, `audio.h`, `esaiclock.h`, `hdi08.h/.cpp` côté DSP,
`mc68k/hdi08.h` côté UC). Les numéros de ligne se réfèrent à l'état de la
branche `release/md-mm-alpha` au commit 54dc3c45.

## 0. Vue d'ensemble

Trois processeurs, un seul thread. L'UC ColdFire tourne à 40 MHz
(`g_ucClockHz`, mdtypes.h:48), les deux DSP56303 à 101,6064 MHz. L'unité de
temps machine partagée est la frame codec à 44,1 kHz : une frame vaut
2304 cycles DSP (`g_dsp1CyclesPerEsaiFrame`, mdhardware.cpp:40) et
40 000 000 / 44 100 ≈ 907,03 cycles UC. Le slot codec ESSI1 fait 1152 cycles,
deux slots par frame (mddsp.cpp:44-49) ; le firmware dérive du diviseur ESSI0
un slot de lien inter-DSP de 96 cycles, soit jusqu'à 24 mots de lien par frame
et par direction.

Topologie (mdhardware.h:43-49) : DSP2 (producer, HI08 à 0x600000) produit les
voix et les pousse par ESSI0 vers DSP1 (mixer, 0x500000), qui pilote le codec
(ESSI1) et donc la sortie audio ; son compteur de frames ESSI1 est l'horloge
maîtresse. L'UC parle aux deux DSP par HI08, et DSP2 remonte ses réponses vers
l'UC via HREQ → IRQ4.

Le scheduler (`advance()` → boucle `schedStep()`, mdhardware.cpp:1210-1470,
1615-1629) est un interleaver déterministe : il maintient une horloge machine
en frames codec et, en boucle événementielle, fait avancer le processeur le
plus **en retard** d'un quantum borné. La synchronisation fine UC↔DSP se fait
à chaque accès HI08 (catch-up inline), la synchronisation DSP↔DSP à chaque
livraison de mot de lien. Il n'existe aucun thread d'arrière-plan : tout ce
qui suit s'exécute sur le thread appelant de `processAudio()`.

Répartition mesurée en playback (session du 2026-09-22, `temp/sp_playback.log`,
notes de la carte) : UC 30,5 % du wall, DSP1 33,7 %, DSP2 26,4 %, catch-ups
0,5 %.

```
thread audio hôte (unique)
processAudio(frames, latency)                          mdhardware.cpp:1158
  └─ renderHostAudio ─ queueHostAudioInput → timelines ADC [DSP1, DSP2]
      └─ advance(chunk)                                mdhardware.cpp:1615
          └─ while (schedStep())        laggard-first, quantum 125 µs MD / 30 µs MM
              ├─ UC en retard : boucle processUC()     mdhardware.cpp:1334-1424
              │    ├─ pumpScheduledMidi / panel / pumpMidiIngress
              │    ├─ pumpDsp2HostRequest  → HREQ→IRQ4  (queue 16 mots, seuil 3 MD / 1 MM)
              │    ├─ m_uc.exec()
              │    │    └─ accès HI08 → schedCatchUpDsp(i)   [exécute le DSP i inline]
              │    └─ skip idle BRA.B -2 (probe 1/16, MD : dspTxClear requis)
              └─ DSP en retard : execUntilCycles(quantum, clamp 100 000)
                   ├─ callback ESSI0 TX → schedCatchUpDspToDsp(cons, prod) puis enqueue
                   │      (rings 32768 frames ; drop-on-full / silence-on-empty)
                   └─ DSP1 : callback ESSI1 → onEssiCallbackMixer → schedDrainCodecOutput
```

Le lien inter-processeurs, vu à plat :

```
            ┌────────────── HI08 ──────────────┐        ┌── HI08 + HREQ→IRQ4 ──┐
            ▼                                  ▼        ▼                      │
     UC ColdFire 40 MHz                DSP1 mixer 0x500000              DSP2 producer 0x600000
            │  chaque accès HI08 :         │      ▲                          │
            │  schedCatchUpDsp(cible)      │      │ ESSI0 ring (32768)       │
            │                              │      ╘══ TX DSP2→DSP1 ══════════╡
            │                              ╞══ TX DSP1→DSP2 (idle: catch-up DSP2) ═▶
            │                              │
            │                    Port C bit 1 (block sync / strobe PDRC)
            │                              └───────── GPIO write ───────────▶ Port C DSP2
            │                              │
            │                        ESSI1 codec ──► ring sortie (bloquant) ──► schedDrainCodecOutput ──► hôte
            └── timelines ADC hôte ──► ESSI1 RX des DEUX DSP (une copie chacun)
```

## 1. Rings ESSI inter-DSP

**Support.** Chaque ESSI possède un ring d'entrée et un ring de sortie hérités
de `dsp56k::Audio` : `RingBuffer<Frame, 32768, Lock=true>`
(`Audio::RingBufferSize = 8192 * 4`, audio.h:268, 290-291). Le `push_back` et
le `pop_front` de ce ring sont **bloquants par sémaphore**
(ringbuffer.h:44-114) — c'est le modèle multi-thread des autres synths du
projet (thread DSP + thread audio), où le blocage EST le mécanisme de
rendezvous. Attention : le commentaire « 2048-deep transport »
(mdhardware.cpp:254) est périmé ; la capacité réelle est 32768, comme le dit
le commentaire du constructeur (mdhardware.cpp:151-153). Le chiffre 2048 de
l'énoncé du ticket vient de ce commentaire périmé.

**Sous le scheduler, jamais bloquant pour le lien.** Les deux DSP partagent le
thread : un push bloquant qui attend que le pair draine serait un deadlock.
Le constructeur (mdhardware.cpp:149-494) remplace donc les chemins bloquants
par des callbacks : `setWriteTxCallback` intercepte chaque frame TX ESSI0 et
l'enfile **non-bloquant** dans le ring d'entrée du pair (drop-on-full), et
`setReadRxCallback` (`blockingPop`, mdhardware.cpp:344-405) dépile
non-bloquant (silence-on-empty). La justesse d'ordre vient du catch-up avant
livraison (section 3) et, quand elle est armée, de la sémantique skip-on-empty
matériellement fidèle.

- **Qui / contexte.** Le callback TX du producteur s'exécute *dans la slice du
  DSP émetteur* (execTX de son ESSI0, cadencé par `EsxiClock` pendant
  `dsp().exec()`). Le callback RX s'exécute dans la slice du DSP récepteur.
- **Fréquence.** Un slot de lien = 96 cycles DSP → jusqu'à ~1,06 M mots/s par
  direction en fenêtre active ; en pratique le protocole MD travaille par
  fenêtres DMA4 de 32 samples et le MM par bursts entre strobes.
- **Amorçage.** Les rings d'entrée sont préremplis (`writeEmptyAudioIn(64)`,
  mddsp.cpp:162-163) pour qu'aucun côté du full-duplex ne bloque au boot, et
  `execTX` court avant `execRX` à chaque slot : chacun nourrit son voisin
  avant de pouvoir manquer d'entrée (mdhardware.cpp:154-159).

**Fine link et on-demand wire semantics.** `setFineLinkMode(true)`
(mddsp.cpp:57) fait dériver de CRA une période de mot inférieure au tick
codec ; `writeCRA` positionne alors `m_fastLinkRx` (essi.cpp:468-482) et le RX
saute les slots sans donnée (`setRxDataAvailableCallback`, mddsp.cpp:60-63).
Sur MD OS 1.63 uniquement (fingerprint testé, mdhardware.cpp:134-135), le
transport arme en plus les sémantiques « fil série on-demand » des deux côtés
lors d'un flush de fenêtre (`mdLinkWindowFlushed`, mdhardware.cpp:907-976) :
en mode MOD=1/DC=0 sans mot frais, le TX ne génère **aucune** frame sync — TDE
reste levé, la DMA de requête est servie, et rien ne part sur le fil
(essi.cpp:86-101). Un fil série n'a pas de mémoire : un mot émis pendant que
le récepteur est désarmé est perdu (d'où les nombreuses gardes de drop
détaillées en section 4).

**Idle words / clock rendezvous.** Avant cette sémantique, les retransmissions
de registre retenu pendant les slots idle couplaient de fait les horloges des
deux DSP. Pour préserver ce couplage sans inventer de données RX, le mixer
reçoit `mixerEssi.setOnDemandTxIdleCallback([]{ schedCatchUpDspToDsp(1, 0); })`
(mdhardware.cpp:955-961) : chaque slot de lien où le TX on-demand de DSP1 n'a
pas de mot frais, on avance DSP2 (consommateur 1) jusqu'au temps de DSP1
(producteur 0). Sans cela, des slices grossières peuvent glisser d'un bloc de
32 samples (commentaire, mdhardware.cpp:956-958).

- **Qui / contexte.** Callback invoqué par `Essi::execTX` de DSP1, donc
  *pendant la slice DSP1*.
- **Fréquence.** Potentiellement chaque slot de lien idle (jusqu'à ~1 M/s au
  pire), mais l'early-out `alreadyAtTarget` (mdhardware.cpp:1580-1587) le
  réduit à un test de comparaison dans l'immense majorité des cas.
- **Contrainte parallélisation.** C'est un couplage d'horloges pur, sans
  donnée : en parallèle il devient soit inutile (chaque DSP a son thread qui
  avance seul), soit un point d'attente. Il documente surtout l'exigence
  réelle : les deux timelines DSP ne doivent pas dériver de plus d'un bloc
  DMA (32 samples) l'une par rapport à l'autre pendant une fenêtre active.

**Purge anti-stall du RX** (`blockingPop`, mdhardware.cpp:352-387). Un ring
profond ne peut apparaître que si le RX du consommateur a cessé de clocker
pendant que le fil courait (prefill du constructeur, transitoire
d'activation, reconfiguration ESSI) : le matériel perd ces mots par overrun et
reprend au flux courant, alors qu'un ring les rejouerait pour toujours. MD :
purge immédiate au-delà de 16. MM : ses bursts flow-controlled sont légitimes,
donc purge seulement si le ring n'est pas redevenu ≤ 16 depuis plus de
1024 frames codec. Le rendezvous MD actif court-circuite cette purge (il
transporte des edges futurs).

## 2. Pump HI08 (`pumpDsp2HostRequest`)

Le chemin DSP → UC. Chaque DSP a deux interfaces HI08 : la vraie (`hdi08()`,
côté DSP, dans le submodule) et le miroir UC (`m_hdiUC`, `mc68k::Hdi08`).
`pumpDsp2HostRequest` (mdhardware.cpp:1055-1095) fait trois choses : drainer
le TX des deux DSP vers les files côté UC, recalculer HREQ, et piloter la
ligne IRQ4.

- **Qui / contexte.** `processUC()` (mdhardware.cpp:1036-1042), donc *slice
  UC*, **avant** `m_uc.exec()` : les interruptions SIM sont injectées dans
  `exec()`, l'IRQ levée par le pump doit être visible à l'instruction que ce
  pas exécute.
- **Fréquence.** Sur MD : à chaque pas UC (des millions d'instructions par
  seconde), d'où le fast path : un simple load acquire de
  `m_schedulerHostPumpDirty` évite le RMW quand rien n'a bougé
  (mdhardware.cpp:1057-1067). Sur MM : seulement si le dirty flag est levé ou
  qu'un mot différé attend son timestamp. Le flag est armé par
  `notifyHostPumpStateChanged()`, câblé sur : écriture HOTX par un DSP
  (hdi08.cpp:466-467, 483-484), écriture ICR par l'UC, changement d'état RX
  côté UC (mddsp.cpp:271-279).
- **Queue 16 mots.** Sur MD, `pumpHostRx(_maxUcWords = 16)`
  (`hostReceiveQueueCapacityWords`, mdtransportpolicy.h:27-28) pousse les mots
  HOTX du DSP directement dans la file `m_rxData` côté UC, en contournant le
  latch RXDF à un mot qui plafonnerait la disponibilité à 1 et rendrait le
  seuil HREQ ≥ 3 inatteignable (mddsp.cpp:243-269). `relatchRx()` re-latche en
  ordre FIFO. La borne de 16 empêche la file de croître quand l'UC ne lit pas.
- **Edge HREQ → IRQ4.** `hreq = RREQ actif && hostRxWordsAvailable() >=
  hostReceiveIrqMinWords` puis `setExternalIrq4(hreq)`
  (mdhardware.cpp:1088-1094). Seuil : **3 mots sur MD** — attendre trois mots
  regroupe deux notifications de bloc en une seule demande de service
  (commentaire mdhardware.cpp:1071-1073) — contre **1 sur MM**, qui modélise
  le latch RXDF matériel. La ligne est level-sensitive côté UC, avec
  exactement une IRQ4 pendante tant qu'elle reste levée
  (mdmc.cpp:498-535).
- **MM : datation du mot.** `hdiTransferDSPtoUC` (mddsp.cpp:446-469) ne publie
  pas le mot immédiatement : il le *réserve* dans `TimedHostRx`
  (mdtimedhostrx.h) avec `readyCycle = hostRxReadyCycle(dsp,
  cycleDuWriteTX)`, et `take()` ne le rend visible que quand l'horloge UC
  atteint le timestamp du producteur. Un DSP avancé inline ne peut donc pas
  publier RXDF/HREQ « dans le futur » de l'UC. Le callback CVR MM
  (mddsp.cpp:95-103) applique la même règle au bit HC.
- **Contrainte parallélisation.** Le pump lit l'état de deux DSP et écrit
  l'état d'interruption de l'UC de manière synchrone à la frontière de chaque
  instruction. En parallèle, HREQ devient une publication cross-thread dont la
  latence de visibilité ajoute du retard à l'IRQ4 ; et la sémantique MM
  « visible seulement au timestamp du producteur » suppose une horloge
  machine lisible de façon cohérente depuis les deux côtés. C'est l'objet du
  ticket 05.

## 3. Catch-ups

Les deux primitives qui font converger les timelines. Toutes deux sont
monotones (jamais en arrière), bornées par
`catchUpMaxDspCycles = 100 000` cycles DSP (~1 ms machine,
mdtransportpolicy.h) et instrumentées par le scorecard
(mdtransportdiagnostics.h).

**`schedCatchUpDsp(dspIndex)`** (mdhardware.cpp:1485-1543) : avance le DSP
cible inline jusqu'au temps machine UC courant
(`dspCatchupDeadline`, mdhostclock.h:30-43, en coordonnées entières depuis
l'origine de boot pour éviter les erreurs d'arrondi flottant).

- **Qui.** Le pont HI08 de mddsp.cpp, à chaque accès de l'UC :
  `hdiTransferUCtoDSP` (chaque mot de donnée, :319), `hdiSendIrqToDSP` (chaque
  commande CVR, :380), `hdiUcReadIsr` (chaque lecture de statut — c'est le
  chemin des boucles de poll du firmware, :420), et le callback CVR MM (:98).
- **Contexte.** *Slice UC* : ces callbacks sont déclenchés par un accès
  mémoire ColdFire au milieu de `m_uc.exec()`. Le DSP cible s'exécute donc
  dans la pile d'appel de l'UC.
- **Fréquence.** Chaque accès HI08. Pendant le boot handshake et les polls
  d'ISR c'est le mécanisme qui fait converger UC-poll ↔ DSP-réponse en
  lockstep fin au lieu de laisser le DSP gelé pendant tout un quantum. Coût
  mesuré en playback : l'ensemble des catch-ups pèse ~0,5 % du wall (mesure de
  session citée par la carte).
- **Gate MM.** Un DSP dont `hostTxBacklog() > 4` n'avance pas non plus en
  catch-up (mdhardware.cpp:1524-1530) — les boucles de catch-up sont justement
  ce qui permettait à un DSP de distancer l'UC de milliers de mots.

**`schedCatchUpDspToDsp(consumer, producer)`** (mdhardware.cpp:1545-1613) :
avance le **consommateur** jusqu'au temps machine du producteur avant qu'une
frame de lien n'entre dans son ring — une frame n'est jamais consommée
« avant » (en temps DSP) d'avoir été produite, ni un quantum entier en retard.

- **Qui.** `pushToInput` (deux fois : chemin rendezvous MD :241, chemin
  legacy :257) et le callback idle on-demand (:960).
- **Contexte.** *Slice du DSP producteur* (callback TX ESSI0) — le
  consommateur s'exécute dans la pile du producteur. Un catch-up peut donc
  s'imbriquer : DSP1 avancé inline peut lui-même émettre sur son TX.
- **Garde de réentrance.** `m_schedInLinkDelivery` (bool simple,
  mdhardware.cpp:1559-1563, 1594-1600) : les pushes back-channel du
  consommateur pendant son propre catch-up n'entrent pas dans un second
  catch-up ; ils enfilent non-bloquant et le DSP sera rattrapé à sa prochaine
  frame de lien ou par le scheduler.
- **Fréquence / coût.** Chaque mot de lien livré. La plupart des écritures
  arrivent alors que la slice ordinaire du consommateur a déjà atteint ce
  timestamp : l'early-out `alreadyAtTarget` (:1580-1587) évite même le coût de
  la garde.
- **Effets de bord datés.** Le catch-up exécuté au milieu d'un callback déjà
  « snapshotté » peut ouvrir une nouvelle fenêtre de réception (MD) ou croiser
  un nouveau strobe (MM) ; le mot courant appartient alors à l'intervalle
  terminé et est jeté (gardes :262-269 et :274-281, re-lecture de
  `m_mmLinkStrobeEpoch`).

**Runs inline bornés du pont HI08** (mddsp.cpp), même famille mais côté Dsp :
`onUCRxEmpty` (lecture UC bloquante → exécuter le DSP jusqu'à production de la
réponse ou fin de la commande en vol, clamp 100 000, :281-313),
`writeWordToDsp` (drainer HRX avant d'y poser le mot suivant, :326-343),
`waitForHostCommandIdle` (un CVR ne double pas une commande en vol, :345-362),
et sur MM le drain de HORX avant dispatch CVR (clamp ×4, :385-397).

- **Contrainte parallélisation.** C'est le cœur du problème : chaque
  rendezvous est aujourd'hui « j'exécute le pair dans mon contexte jusqu'à ce
  que sa timeline me rejoigne ». En parallèle, « avancer le pair » devient
  « attendre le pair » (ou spéculer/rejouer). Le coût CPU est marginal
  (0,5 %) ; la difficulté est la **fréquence** des points (chaque accès HI08,
  chaque mot de lien en fenêtre active) et le fait que la convergence du boot
  handshake en dépend.

## 4. Machinerie Port C / PDRC

Le lien série est piloté par un signal GPIO : DSP1 écrit son Port C, dont le
bit 1 porte le block sync du lien (une transition par 147 456 cycles DSP,
soit 128 périodes de mot codec = 64 frames, mdhardware.cpp:502-510). Le
callback `setCallbackDspWrite` (mdhardware.cpp:525-568) forwarde ce niveau
vers le Port C de DSP2 (`hostWrite`). Sous scheduler grossier, DSP2 peut être
des dizaines de milliers de cycles en avance quand DSP1 lève la requête ;
toute la machinerie qui suit reconstruit l'atomicité de l'edge que deux DSP
réels observent simultanément.

**Côté MD (rendezvous on-demand actif)** — pending/visible/epochs :

- Une transition n'est **pas** transmise immédiatement : elle devient
  `m_mdProducerPortCPending` avec son niveau et son époque
  (`m_mdProducerPortCPendingEpoch = m_mdLinkFlushEpoch`,
  mdhardware.cpp:528-538).
- Elle n'est **released** que quand DSP2 lit lui-même son Port C (host input
  source posé à l'armement, :930-946) **et** que la DMA4 du mixer est armée :
  alors `m_mdProducerPortCVisible` prend le niveau et
  `m_mdProducerPortCReleaseEpoch = pendingEpoch`. Le strobe devient visible
  dans le contexte d'exécution de DSP2, au moment où sa boucle de poll
  l'observe réellement.
- La livraison d'un mot de lien exige `releasedForWindow = !pending &&
  releaseEpoch == flushEpoch` plus DMA4 active et un mot TX frais
  (:212-244) ; tout le reste est jeté avec un compteur de diagnostic dédié.
- `m_mdLinkFlushEpoch` s'incrémente à chaque flush de fenêtre : une lecture
  RX0 avec DMA4 désarmée (protocole spécifique MD DSP1) purge les données de
  lien stagées et appelle `mdLinkWindowFlushed()` (`setRxConsumeCallback`,
  mddsp.cpp:68-85 ; mdhardware.cpp:907-976). Un pin non-released à la fenêtre
  suivante est jeté (:965-971).
- S'y ajoutent l'overrun récepteur fidèle au silicium (un seul mot RX non
  collecté ; le mot arrivant est détruit, ROE levé — DSP56303UM tableau 7-5,
  :283-308) et le drop des retransmissions retenues après flush
  (`m_mdLinkAwaitFresh`, :318-328).

**Côté MM — strobe + await-fresh** :

- Détection d'edge dans le contexte mixer (`m_mmLinkStrobeLevel`) : si DMA4
  (mixer) et DMA1 (producer) sont idle, le ring est purgé (le registre RX est
  profond d'un mot, pas une FIFO d'archive), `m_mmLinkAwaitFresh` passe à
  true et `m_mmLinkStrobeEpoch` s'incrémente (:541-566).
- Côté TX de DSP2 : tant que `m_mmLinkAwaitFresh`, tout mot émis sans DMA
  active des deux côtés ou sans donnée fraîche est supprimé — c'est le préfixe
  d'underruns de registre retenu que seul le scheduler grossier crée ; le
  premier mot alimenté par DMA désarme la garde et les sémantiques normales
  reprennent (:444-477).
- L'époque strobe re-vérifiée après un catch-up imbriqué (:274-281) annule la
  livraison d'un mot appartenant à l'intervalle précédent.
- **Contrainte parallélisation.** Ces machines à époques existent uniquement
  parce que le scheduler est grossier ; un transport parallèle recrée le même
  problème sous une autre forme (visibilité cross-thread de l'edge). Deux
  options se dessinent pour le ticket 04 : conserver les époques (elles sont
  déjà exprimées en termes d'événements, pas de threads), ou synchroniser
  les timelines des deux DSP au moment du strobe. À noter : les états MM
  (`m_mmLinkAwaitFresh`, `m_mmLinkStrobeEpoch`) sont déjà des atomiques avec
  acquire/release, les états MD sont des bools/uint64 nus — un portage
  parallèle du chemin MD demande une passe de visibilité mémoire complète.

## 5. Policies : MD contre MM

`mdtransportpolicy.h:24-29` regroupe toutes les constantes de transport :

| Paramètre | Machinedrum | Monomachine | Effet |
|---|---|---|---|
| `backgroundQuantumMicroseconds` | 125 µs | 30 µs | Taille de slice : ~5,51 frames codec (~5000 cycles UC / ~12 700 cycles DSP) contre ~1,32 frame (~1200 / ~3048) |
| `catchUpMaxDspCycles` | 100 000 | 100 000 | Clamp de tous les catch-ups et runs inline HI08 |
| `hostReceiveIrqMinWords` | 3 | 1 | Seuil HREQ→IRQ4 (§2) |
| `hostReceiveQueueCapacityWords` | 16 | 16 | Borne de la file RX côté UC (§2) |
| `hostTransmitBackpressureThresholdWords` | 4 | 4 | MM uniquement : backlog TX qui parque un DSP (§8) ; MD n'applique jamais ce gate |
| `hostTransmitBackpressureReleaseUcCycles` | 200 000 | 200 000 | Clamp du park (5 ms UC) : une phase UC qui ne draine pas ne peut pas affamer le codec |
| `exactEssiCycleDeadlines` | false | true | MM : le dispatcher JIT s'arrête exactement aux deadlines de cycle ESSI (`EsxiClock::setExactCycleDeadlineEnabled`, mddsp.cpp:50-51, esaiclock.h:44) |

Deux paramètres JIT complètent le tableau (mddsp.cpp:115-133) :
`maxInstructionsPerBlock = 32` pour les deux machines (les boucles serrées
doivent rendre la main assez souvent pour que l'EssiClock tique), et
`maxDoIterations = 64` sur MD contre **4** sur MM : avec le quantum de 30 µs
et les deadlines exactes, 64 fait échouer `mmSineFirmwareTest` /
`mmSineMidiFirmwareTest`. Le MM est donc structurellement plus serré : environ
1,3 frame de marge de désynchronisation tolérée, contre 5,5 pour le MD, plus
un flux hôte sans perte (flow control + datation TimedHostRx) là où le MD
tolère du best effort. Toute conception parallèle doit être instruite contre
les deux jeux de contraintes à la fois (décision déjà actée dans la carte).

## 6. Timeline audio d'entrée hôte

Deux timelines indépendantes, une par DSP (`m_hostAudioInput[2]`,
`RealtimeHostAudioInputTimeline`, capacité 65 536 frames,
mdhostaudioqueue.h:113-150) : le bus ADC du codec atteint les deux DSP — DSP1
pour le metering, DSP2 pour l'enregistrement UW RAM (et les pistes 1-3 du MM
tournent sur le producer). Chaque récepteur a sa copie pour que l'ordre du
scheduler ne puisse pas voler la frame du pair (mdhardware.cpp:488-494).

- **Écriture.** `queueHostAudioInput(chunk)` depuis `renderHostAudio`, juste
  avant chaque `advance(chunk)` (mdhardware.cpp:1137-1156, 1170-1175) —
  contexte : thread audio hôte, hors scheduler. Sans source connectée, seul le
  curseur avance (silence synthétisé à la lecture, pas de frames de zéros).
- **Lecture.** Callback `codecInput` posé sur l'ESSI1 RX de chaque DSP
  (mdhardware.cpp:407-436, 491-494) — contexte : slice du DSP concerné, une
  frame par frame codec (44 100/s). L'adresse lue est
  `clockOrigin + frameIndex − latency` ; l'origine est (re)calée sur
  `schedDspFramePos` quand `frameIndex` saute (reconfiguration ESSI).
- **Marge de sécurité.** `latency = latenceHôte + g_hostAudioInputSafetyFrames
  (64)`, clampée à la capacité (mdhardware.h:30-33, mdhardware.cpp:1120-1135) :
  une slice peut finir son bloc JIT **après** la cible de cycle, donc lire
  quelques frames « d'avance » ; le look-ahead de 64 frames garantit que ces
  lectures ne demandent jamais des samples d'un callback hôte futur.
- **Overflow.** `append` droppe (et resette la timeline si le temps machine a
  avancé sans input) → `m_hostAudioInputOverflow[dsp]`
  (mdhardware.cpp:1146-1154). **Underflow** : `readAt` échoue alors qu'une
  source existe et qu'on n'est pas avant le début du flux →
  `m_hostAudioInputUnderflow[dsp]` (:427-428). Les deux sont de la télémétrie
  par DSP, exposée au Device.
- **Contrainte parallélisation.** C'est la contrainte qui a tué le proto v2
  (note du ticket) : la position de lecture d'un DSP est aujourd'hui bornée
  par construction — le scheduler n'avance jamais un DSP de plus d'un quantum
  au-delà du bloc hôte courant. En exécution libre, un DSP qui file en avant
  dépasse la marge de 64 frames et lit de l'audio pas encore produit
  (underflow), ou un DSP en retard laisse l'append recycler la fenêtre
  (overflow). Le budget de latence/lookahead est l'objet du ticket 03.

## 7. Skip idle UC

Quand le firmware ColdFire est dans son idle loop (`BRA.B -2`), exécuter les
instructions une à une est du travail pur perdu. Le fast-forward :

- **Éligibilité** (`idleSelfBranchInstructions`, mdmc.cpp:441-475) :
  uniquement le point fixe architectural — `ir == 0x60fe`, `pc == ppc`, pas de
  trace/PMMU/NMI/IRQ éligible, relecture mémoire de l'opcode ; aucun recours à
  une adresse firmware ou une signature. Le saut s'arrête **strictement
  avant** la prochaine interruption timer ou la fin de caractère UART (le
  chemin normal doit matérialiser l'événement), et ne se déclenche pas sous
  8 instructions.
- **Avance** (`advanceIdleSelfBranch`, mdmc.cpp:477-485) : ajoute
  `instructions × 2` cycles et rejoue la SIM ; aucun registre ni mémoire ne
  change. L'appelant préserve l'horloge vue par la dernière mise à jour SIM
  (`m_schedUcCyclesDone += cycles − 2` avant, `+= 2` après,
  mdhardware.cpp:1397-1406).
- **Qui / contexte / fréquence.** La boucle UC de `schedStep`
  (mdhardware.cpp:1352-1408), *slice UC*, sonde **une itération sur 16**
  (`probeCount & 15`). Conditions supplémentaires : ≥ 16 cycles restants dans
  la slice, pas de restore projet en attente, pas de wake pump, pas de RX
  différé, pas de transfert sysex en cours, curseur MIDI à zéro ; et la
  deadline du prochain événement MIDI schedulé borne le saut (:1381-1387).
  Chaque instruction omise garde un poll des entrées externes (panel, MIDI,
  sysex — boucle :1392-1396).
- **Transparence MD** (`dspTxClear`, mdhardware.cpp:1356-1368) : le MD pompe
  son host request à chaque pas ; sauter des pas alors qu'un DSP détient un
  mot TX non pompé retarderait l'edge HREQ→IRQ4 que le firmware idle attend
  peut-être. Le skip n'est donc autorisé (MD) que si les deux registres TX
  sont vides — le pump est alors un no-op et le saut est transparent. Mesure
  de session citée par le ticket : `blockedByDspTx` ~15 probes sur 600 000 —
  la garde ne coûte pratiquement rien. Le MM saute sans cette condition (son
  pump est event-driven).
- **Contrainte parallélisation.** Le skip repose sur une connaissance
  instantanée de l'état TX des deux DSP et des files d'entrée. En parallèle,
  cette observation devient racée ; il faudra soit la rendre conservatrice
  (rater un skip est bénin, sauter à tort ne l'est pas), soit déplacer le
  fast-forward derrière le mécanisme de wake du pump.

## 8. Scheduler (`advance` / `schedStep` / `processUC`)

**`advance(machineFrames)`** (mdhardware.cpp:1615-1629) : incrémente la cible
`m_schedFramesTotal`, boucle `while(schedStep())`, puis drain final du codec,
avancement de la capture flash usine et publication du front panel (deux
opérations try-lock : jamais d'attente du thread émulation sur un lecteur UI).

**`schedStep()`** (mdhardware.cpp:1270-1470), une itération :

1. Positions en frames codec : UC = `cyclesUC / 907,03` ; DSP =
   `originFrame + (cycles − originCycles) / 2304`. L'origine d'un DSP est
   latchée au moment où il devient runnable (fin de boot pendant une slice
   UC) : il démarre « maintenant » et son compteur de cycles est rate-locké à
   l'horloge à partir de là (:1279-1292). Un DSP pas encore booté est parqué à
   la cible, jamais choisi comme laggard.
2. **Park backpressure MM** (:1296-1324) : un DSP dont `hostTxBacklog() > 4`
   est parqué (position forcée à la cible) jusqu'à ce que l'UC draine, avec
   clamp de libération à 200 000 cycles UC pour que le codec ne meure pas de
   faim. `hostTxBacklog` = TX DSP + file RX UC + mot timed différé
   (mddsp.cpp:237-241). Chemin MD volontairement intact.
3. **Laggard-first** : le plus en retard (UC=0, DSP1=1, DSP2=2 ; l'ordre des
   comparaisons favorise l'UC à égalité) avance vers
   `min(pos + quantum, cible)`. Retourne false quand tout le monde a atteint
   l'horloge partagée.
4. **Slice UC** : boucle `processUC()` jusqu'à la sous-cible ou le clamp
   (100 000 cycles), avec le probe idle 1/16 (§7).
5. **Slice DSP** : `execUntilCycles(stopCyc)` — dispatcher JIT borné par
   cycles, chemin par défaut ; `GEARMULATOR_MDMM_BOUNDED_JIT=0` retombe sur la
   boucle `exec()` historique (:1443-1450, choix shippé :124-126). Après une
   slice DSP1, drain du ring codec (:1463-1464).

**`processUC()`** (mdhardware.cpp:999-1053), un pas UC :

1. Livraison du MIDI schedulé et des paquets panel vers l'UART2 (gelée si un
   restore projet est en attente — l'input ne doit pas muter la machine
   bootstrap puis disparaître au restore).
2. `pumpMidiIngress()` si une source MIDI est active (arbitrage wire sysex /
   événements / realtime).
3. `pumpDsp2HostRequest()` (§2) — avant l'exec.
4. `m_uc.exec()` : une instruction ColdFire + SIM + injection d'interruptions.
   C'est pendant cet appel que les accès HI08 déclenchent les catch-ups (§3).
5. Service du transfert sysex pacé, comptabilité `m_schedUcCyclesDone`.

**Le ring codec, seul vrai point bloquant restant.** La sortie ESSI1 du mixer
utilise le `push_back` bloquant du ring Audio ; `schedDrainCodecOutput()`
(mdhardware.cpp:1247-1268) est donc appelé depuis le callback ESSI1 du mixer
(`onEssiCallbackMixer`, chaque frame codec, :1102-1109), après chaque slice
DSP1 et en fin d'`advance()` — l'invariant est que ce ring reste toujours
assez peu profond pour que le producteur ne parque jamais l'unique thread.

**Contrainte parallélisation (synthèse).** Le scheduler actuel garantit trois
invariants qu'un transport parallèle devra reproduire autrement :

1. **Borne de dérive** : aucun processeur ne dépasse la cible, aucun ne
   traîne de plus d'un quantum + clamp derrière un pair qui a besoin de lui —
   c'est ce qui rend suffisants la marge ADC de 64 frames (§6), la fenêtre
   DMA de 32 samples (§1/§4) et le seuil HREQ de 3 mots (§2).
2. **Rendezvous exacts en temps machine** : livraison de lien datée
   (catch-up-before-delivery ou wire on-demand), mot hôte MM visible à son
   timestamp, strobe PDRC atomique par époques. La profondeur des rings
   (32768) n'est jamais la contrainte ; l'ordre et la datation le sont.
3. **Réactivité UC↔DSP** : les polls du firmware convergent parce que chaque
   accès HI08 exécute le pair inline. Fréquence élevée, coût faible (0,5 %
   wall) — le remplacement parallèle doit être aussi bon marché au point
   d'appel, sinon il inverse le gain.

## Annexe : divergences relevées entre le ticket et le code

- « Profondeur 2048 » (ticket, et commentaire mdhardware.cpp:254) : la
  capacité réelle des rings ESSI est **32768** frames
  (`Audio::RingBufferSize`, audio.h:268) ; le commentaire de la ligne 254 est
  un vestige d'un état antérieur.
- « ~907.03 cycles UC par frame » : le commentaire mdhardware.cpp:39 écrit
  `g_ucClockHz/44100` ; avec 40 MHz cela donne 907,029…, cohérent.
- Les mesures « catchup 0,5 % du wall » et « blockedByDspTx ~15/600k probes »
  ne figurent pas dans le code : ce sont des mesures de session
  (2026-09-22, `temp/sp_playback.log`) reprises par la carte et le ticket.
