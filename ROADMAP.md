# Feuille de route

Ce qui reste à faire après la preuve de concept. L'état de ce qui marche
déjà est dans [WORKINPROGRESS.md](WORKINPROGRESS.md).

Ordre indicatif : les dépendances entre points comptent plus que la
numérotation. Le son et le multiclient touchent au protocole, donc mieux
vaut les faire avant de multiplier les clients qui devront les parler.

---

## A. Émulateurs

### A1. Azahar (3DS)
- [x] Backend : lecture de `screen_infos[2]`, avec rotation
- [x] Tactile via `TouchPressed` / `TouchMoved` / `TouchReleased`
- [x] Profil 3DS complet : ZL/ZR, circle pad, C-stick

### A2. Cemu (Wii U)
- [x] Backend sur `LatteRenderTarget_copyToBackbuffer(_, true)`
- [x] Tactile, boutons et sticks branchés et vérifiés
- [x] Profil de boutons Wii U, sticks compris

### A3bis. 3DS : la configuration système plante — en suspens

Session du 2026-09-05. L'Artic Setup Tool est la seule voie supportée
par Azahar pour installer les titres système, et elle plante
systématiquement. Tout ce qui était configurable a été éliminé :

| Vérifié | État |
|---|---|
| Luma3DS | v13.4, au-delà du v13.3.1 exigé |
| Artic Setup Tool | v1.0.3, la dernière publiée (avril 2025) |
| Confirmation par A sur la console | faite |
| État NAND partiel | nettoyé avant essai |
| Voie d'accès | dialogue **et** URL `articinio://` |
| Version d'Azahar | `2126.1-rc3` **et** stable `2126.0` |
| Réseau | jamais un octet au-dessus du bruit de fond |

Ce n'est donc ni le réseau, ni le débit, ni une erreur de manipulation.

**Ce qu'on sait du crash.** SIGSEGV reproductible. Sur le build `master`,
la pile était :

```
Service::HTTP::InstallInterfaces
 └─ HTTP_C::DecryptClCertA
     └─ NCCHContainer::AutoOpenNCCHNCSD
         └─ UniqueData::GetUniqueCryptoFileKeyIV   (unique_data.cpp:287)
             └─ Certificate::GetPublicKeyECC       ← SIGSEGV
```

`ct_cert` n'est pas lu depuis un fichier : il est **dérivé de l'OTP** par
`BuildECC()` (unique_data.cpp:180), vérifié contre la clé racine ligne
184, et invalidé si la vérification échoue. La garde ligne 292 teste
`IsValid()`. Le certificat passe donc la vérification puis casse à la
lecture de sa clé publique — une incohérence entre ce que le contrôle
accepte et ce que la lecture suppose.

Sur `2126.0` le crash arrive plus tôt encore, avant même l'écriture des
données uniques. La pile ci-dessus vaut pour l'autre binaire ; il faudra
refaire une passe `gdb` sur la stable avant de corriger quoi que ce soit.

**Piège à connaître.** Chaque tentative réécrit les données uniques
(`otp.bin`, `movable.sed`, `SecureInfo_A`, `LocalFriendCodeSeed_B`) avant
de planter. Un nettoyage manuel est donc défait au coup d'après, et
repartir sans nettoyer redonne le crash. Le dialogue d'Azahar appelle
`UninstallSystemFiles()` pour cette raison.

**Pistes, par ordre de coût croissant :**

- [ ] **Essayer l'autre 3DS.** Si le crash tient à la dérivation du
      certificat depuis cet OTP précis, une autre console tranche la
      question immédiatement. C'est le test le plus discriminant et le
      moins cher.
- [ ] Voir si l'outil peut servir sans passer par la mise à jour
      système — c'est ce chemin, avec ClCertA et le module NIM, qui
      casse.
- [ ] Ouvrir un ticket chez Azahar : on a un cas de reproduction net et
      une pile d'appel.
- [ ] En dernier recours, corriger le fork. Pas avant d'avoir la pile
      exacte du binaire concerné : une garde posée à l'aveugle
      déplacerait l'échec sans donner le menu HOME.

Rien de tout ça ne bloque le projet : le backend Azahar peut s'écrire
sur `FrameDumperOpenGL` et se valider plus tard.

**Note de version.** Le dépôt Azahar est resté sur le tag `2126.0`
(HEAD détaché). Avant d'écrire le backend il faudra une branche dédiée,
comme `bottom-screen` sur melonDS.

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

### B1. Son — fait pour melonDS et Android
- [x] Capter le son des trois émulateurs
- [x] Encoder en Opus et l'ajouter au protocole
- [x] Lecture côté Android (le homebrew Switch reste à faire)
- [x] Barre de volume et bouton muet dans le menu Android

Vérifié de bout en bout : `first audio decoded, 3840 of 3840 bytes
written`, soit un bloc Opus de 20 ms à 48 kHz écrit sans perte.

Deux choix à connaître. Le son est capté **avant** le volume et la
sourdine locaux de melonDS : couper le son sur le PC ne doit pas couper
celui du joueur.

Et le rééchantillonnage passe par `swresample`. Opus n'accepte que 8,
12, 16, 24 ou 48 kHz, et le DSP de la 3DS sort à 32728 Hz — ni l'une de
ces valeurs, ni même une fréquence ronde. Envoyer du 32 kHz brut est
donc impossible, la conversion est obligatoire, et autant qu'elle soit
correcte. melonDS et Cemu sortent déjà en 48 kHz et ne la traversent
pas ; ils lient tout de même la bibliothèque, parce que `bs_audio.c`
est partagé et que le lien se fait à la compilation, pas à l'exécution.

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
- [x] Suivre le rendu interne des émulateurs (x2, x4, xN)
- [x] Reconfigurer l'encodeur en direct quand la taille change
- [x] Annoncer la nouvelle taille aux clients (`STREAM_INFO`)
- [ ] Choix de la résolution de réception côté client

Dépend du readback GPU : le rendu mis à l'échelle n'existe que sur les
renderers matériels, et notre pont ne lit que la RAM. Voir la limite
connue dans WORKINPROGRESS.

### B4. Port
- [x] Si le port est pris au démarrage, incrémenter et réessayer
- [x] Réglage du port dans melonDS et Cemu (Azahar reste à faire)
- [x] Interrupteur actif par défaut, dans melonDS et Cemu

Le premier point est petit et immédiat. Les deux autres demandent de
toucher aux interfaces de réglages de trois émulateurs différents — à
regarder au cas par cas, melonDS a un système de config typé qui s'y
prête.

### B5. Contrôles de l'hôte
- [x] Les entrées réseau sont fusionnées avec les locales, pas
      substituées : une manette sur l'hôte continue de fonctionner

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
