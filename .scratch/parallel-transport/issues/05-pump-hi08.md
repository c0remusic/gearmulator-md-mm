# Service HI08 sous découplage

Type: grilling
Status: resolved
Blocked by: 04

## Question

Concevoir le service host (pump HI08, `pumpDsp2HostRequest`, edge HREQ→IRQ4,
queue 16 mots, transparence du skip idle UC) dans le modèle de découplage
retenu en 04 : qui pompe, depuis quel thread, avec quelles garanties d'ordre
et de deadline, et comment le skip idle UC (82 % des cycles UC, à préserver)
reste transparent quand l'état TX du producer n'est plus lisible de façon
synchrone.

## Answer

Ratifié par l'utilisateur le 2026-09-22. Vérifié contre le code
(mddsp.cpp:243-479, mdhardware.cpp:1055-1095, :1352-1408). Le service HI08
sous « Convoi » (ticket 04) :

1. **Qui pompe : le thread audio.** `pumpDsp2HostRequest`
   (mdhardware.cpp:1055-1095) reste sur le thread audio, fast path dirty-flag
   et `notifyHostPumpStateChanged` inchangés. Contenu nouveau : take() des
   mots dus (readyCycle ≤ ucNow) depuis les files de staging, `m_rxData`
   borné 16, `relatchRx`, HREQ ≥ 3 (MD) / RXDF (MM) compté sur `m_rxData`
   seul, `setExternalIrq4`. Un DSP en avance ne lève jamais un edge dans le
   futur de l'UC — invariant TimedHostRx étendu au MD. Le chemin mixer est
   drainé de la même façon (seul le fil HREQ→IRQ4 est spécifique à DSP2).
2. **Staging côté DSP, sur son thread.** `setWriteTxCallback` (déjà posé pour
   le MM, mddsp.cpp:171-176) stampe {mot, readyCycle = hostRxReadyCycle} dans
   une file SPSC datée par DSP. Profondeur MD : 16
   (= hostReceiveQueueCapacityWords, miroir de la borne `m_rxData` ; en vol
   max 32). L'alternative 4 mots est rejetée : c'est la profondeur de la
   policy MM, dont le park est illégal côté MD.
3. **Backpressure MD = HTDE naturelle.** File de staging pleine ⇒ HOTX non
   drainé ⇒ le firmware se pace sur HTDE, comme sur silicium. Jamais de park
   du worker MD (mdtransportpolicy.h : seuil MM-only). MM : profondeur 1
   conservée (latch RXDF, TimedHostRx actuel) + park backlog>4 avec
   l'échappée de liveness de la greffe 1 du ticket 04.
4. **UC→DSP : flux daté unique par DSP** {DataWord|Cvr|IcrWrite}, stampé en
   temps UC, consommé en ordre programme par le worker à max(stamp, posDsp) —
   remplace les runs inline de `writeWordToDsp`/`hdiSendIrqToDSP`
   (mddsp.cpp:326-413). L'arbitration host-command et le drain HORX pré-CVR
   MM restent DSP-side, exécutés naturellement sur le thread du DSP.
   `waitForHostCommandIdle` devient une attente sur hostCommandBusy /
   hostCommandAcceptedCycle publiés.
5. **Statuts (`hdiUcReadIsr`, mddsp.cpp:415-444) : sémantique EXACTE.**
   waitForDspTime(idx, ucNow) puis lecture du snapshot publié (RXDF, TXDE/TRDY
   recomposés de la profondeur HORX, HF2/HF3). Publication ÉVÉNEMENTIELLE à
   chaque mutation du port hôte, jamais à la frame (sinon TurboMidi s'effondre
   à 1 mot/frame — greffe 6). Réveil futex bit-waiter ; N polls consécutifs ⇒
   park sur avancée (greffe 5). Bounded-stale = repli documenté si le
   scorecard montre un churn rédhibitoire à l'étape 2 de migration — à
   mesurer, pas à spéculer.
6. **onUCRxEmpty(_needMoreData)** (mddsp.cpp:281-313) : attendre
   posDsp ≥ ucNow (clamp catchUpMaxDspCycles conservé), puis prendre le mot
   dû ; absent à ce temps machine ⇒ RXDF reste clair et le firmware re-polle,
   comme sur silicium.
7. **Skip idle UC** (mdhardware.cpp:1352-1408) : la garde `dspTxClear`
   devient locale — files de staging sans mot dû + dirty clear + pas de
   deferred ; une vue périmée non-vide retarde le skip d'une probe (bénin),
   jamais de skip à tort. Borne du saut = min(cible, clamp, deadline MIDI
   programmée, PROCHAIN readyCycle stagé des deux DSPs) — greffe 2, restaure
   la borne que la disparition du quantum supprimait. Le pump par étape reste
   no-op files vides : transparence des 82 % préservée.

**Garantie de deadline** : un mot est visible à son readyCycle et l'IRQ4 part
au plus tard à la probe suivante ; enveloppe ≤ quantum série actuel
(5,51 frames MD / 1,32 MM), jamais pire que le shippé.
