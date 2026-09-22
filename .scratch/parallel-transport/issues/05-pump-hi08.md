# Service HI08 sous découplage

Type: grilling
Status: open
Blocked by: 04

## Question

Concevoir le service host (pump HI08, `pumpDsp2HostRequest`, edge HREQ→IRQ4,
queue 16 mots, transparence du skip idle UC) dans le modèle de découplage
retenu en 04 : qui pompe, depuis quel thread, avec quelles garanties d'ordre
et de deadline, et comment le skip idle UC (82 % des cycles UC, à préserver)
reste transparent quand l'état TX du producer n'est plus lisible de façon
synchrone.
