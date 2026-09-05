# Feuille de route

Ce qui reste à faire après la preuve de concept. L'état de ce qui marche
déjà est dans [WORKINPROGRESS.md](WORKINPROGRESS.md).

Ordre indicatif : les dépendances entre points comptent plus que la
numérotation. Le son et le multiclient touchent au protocole, donc mieux
vaut les faire avant de multiplier les clients qui devront les parler.

---

## A. Émulateurs

### A1. Azahar (3DS)
- [ ] Backend sur le modèle de `FrameDumperOpenGL`
- [ ] Tactile via `TouchPressed` / `TouchMoved` / `TouchReleased`
- [ ] Profil de boutons 3DS (ZL/ZR, circle pad, C-stick)

### A2. Cemu (Wii U)
- [ ] Backend sur `LatteRenderTarget_copyToBackbuffer(_, true)`
- [ ] Tracer et brancher le chemin tactile VPAD
- [ ] Profil de boutons Wii U

### A3. BIOS et fichiers système — bloqué

**Je ne les téléchargerai pas.** Les BIOS et firmwares 3DS et Wii U sont
des fichiers Nintendo sous copyright ; aller les chercher en ligne serait
du piratage, et je ne le ferai pas même si le projet est privé.

La voie légitime est de les extraire de tes propres consoles. Ce dont on
a besoin, et où le déposer :

| Console | Fichiers | Comment |
|---|---|---|
| DS | `bios7.bin`, `bios9.bin`, `firmware.bin` | **déjà en place**, dans `~/Téléchargements/bios/` |
| 3DS | `boot9.bin`, `boot11.bin`, `seeddb.bin`, `aes_keys.txt` | GodMode9 sur une 3DS à toi |
| Wii U | `otp.bin`, `seeprom.bin`, clés de titres | dumper la console |

Dépose-les où tu veux et donne-moi le chemin, ou mets-les dans
`~/Téléchargements/bios/<console>/`. Je configurerai les émulateurs pour
pointer dessus. Sans eux, Azahar et Cemu ne démarreront rien et les
backends ne seront testables que sur une mire.

---

## B. Protocole et serveur

Ces points changent le fil, donc ils passent avant les nouveaux clients.

### B1. Son — important
- [ ] Capter le son de l'émulateur
- [ ] Encoder (Opus, comme capture2cloud) et l'ajouter au protocole
- [ ] Lecture côté Android et Switch
- [ ] Barre de volume et bouton muet dans les menus

Le son a sa propre horloge et sa propre latence ; le mêler au flux vidéo
sur un seul chemin ferait dépendre l'un de l'autre. Un type de message
distinct, avec son horodatage, laisse chaque client décider de sa
synchronisation.

### B2. Multiclient
- [ ] Plusieurs clients simultanés, même logique que capture2cloud
- [ ] Un encodeur partagé, pas un par client
- [ ] Entrées fusionnées entre clients (même logique manette)

Aujourd'hui le serveur sert un client à la fois. L'encodeur est déjà
unique et indépendant du client — c'est la boucle d'envoi qui est liée à
une connexion. À découpler avant d'ajouter des clients.

### B3. Résolution
- [ ] Suivre le rendu interne des émulateurs (x2, x4, xN)
- [ ] Reconfigurer l'encodeur en direct quand la taille change
- [ ] Annoncer la nouvelle taille aux clients
- [ ] Choix de la résolution de réception côté client

Dépend du readback GPU : le rendu mis à l'échelle n'existe que sur les
renderers matériels, et notre pont ne lit que la RAM. Voir la limite
connue dans WORKINPROGRESS.

### B4. Port
- [ ] Si le port est pris au démarrage, incrémenter et réessayer
- [ ] Réglage du port dans chaque émulateur
- [ ] Interrupteur pour activer/désactiver le serveur, actif par défaut

Le premier point est petit et immédiat. Les deux autres demandent de
toucher aux interfaces de réglages de trois émulateurs différents — à
regarder au cas par cas, melonDS a un système de config typé qui s'y
prête.

### B5. Contrôles de l'hôte
- [ ] Vérifier que le clavier et la manette du PC continuent de marcher
      quand un client est connecté

Déjà le cas sur melonDS par construction : les boutons réseau sont
fusionnés avec `inputMask` local, et le tactile local garde la priorité.
À vérifier vraiment, et à reproduire sur les deux autres.

---

## C. Clients

### C1. Homebrew Switch (NRO)
- [ ] Décodage matériel
- [ ] Tactile et boutons virtuels
- [ ] Joy-Con pour les boutons normaux
- [ ] Son

Base : `switch_homebrew/` et `switch_stream.c` de capture2cloud.

### C2. App web JS
- [ ] Encodage VP8 en parallèle du H.264
- [ ] Transport WebRTC
- [ ] Page avec écran, tactile et boutons virtuels

Détail du raisonnement VP8 vs H.264 dans WORKINPROGRESS.

### C3. Interface des menus
- [ ] Reprendre l'ergonomie des menus de capture2cloud dans les trois
      clients (js / nro / apk), avec les mêmes options
- [ ] N'y garder que ce qui a du sens ici : pas de dongle, pas de carte
      de capture, ce projet n'en utilise pas

### C4. Boutons virtuels
- [ ] Affichables ou masquables
- [ ] Refléter une manette branchée quand il y en a une
- [ ] Déplacement explicite : cadre jaune autour des boutons pendant le
      réglage, comme capture2cloud
- [ ] Les boutons de façade (A/B/X/Y) restent groupés quand on les
      déplace — on bouge le losange, pas chaque bouton

### C5. Profils
- [ ] Plusieurs profils dans le menu des clients
- [ ] Basculer de l'un à l'autre quand plusieurs émulateurs tournent en
      même temps

Suppose que chaque émulateur écoute sur son propre port — donc dépend de
B4.

---

## D. Lanceur GTK

- [ ] Une app GTK qui préconfigure et lance les trois émulateurs
- [ ] Réglage du rendu interne (x2, x4, xN) avant lancement
- [ ] Le reste — charger une ROM, les autres options — reste à la charge
      de l'émulateur, on ne réimplémente pas ce qu'il fait déjà

---

## E. Publication

À la toute fin, quand l'ensemble marche.

- [ ] Publier sur GitHub en privé
- [ ] Publier aussi sur le serveur git perso (`192.168.2.101:2222`)
- [ ] Faire une release
- [ ] README renvoyant vers les forks d'émulateurs (`wozt/melonDS`,
      `wozt/azahar`, `wozt/Cemu`), qui sont privés — donc utilisable par
      toi seul, ce qui est assumé
- [x] `goal.md` renommé en `prompt.md` et exclu du dépôt
