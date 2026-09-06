# Bottom Screen

Streamer l'écran du bas d'une console Nintendo depuis un émulateur vers
un téléphone, avec le tactile et les boutons qui repartent dans l'autre
sens.

Trois consoles, trois émulateurs, un seul protocole : melonDS pour la
DS, Azahar pour la 3DS, Cemu pour la Wii U. Le client ne sait pas
laquelle il regarde — le serveur annonce la console, la taille et les
boutons qui existent, et l'interface se construit là-dessus.

---

## Les émulateurs

Ça ne marche **qu'avec les forks**, sur la branche `bottom-screen`. Les
émulateurs officiels n'ont pas le pont.

| Console | Fork | Branche |
|---|---|---|
| Nintendo DS | [wozt/melonDS](https://github.com/wozt/melonDS) | `bottom-screen` |
| Nintendo 3DS | [wozt/azahar](https://github.com/wozt/azahar) | `bottom-screen` |
| Wii U | [wozt/Cemu](https://github.com/wozt/Cemu) | `bottom-screen` |

Chaque fork attend de trouver ce dépôt à côté de lui :

```
bottom_screen_server/
├── bs_server.c, bs_encoder.c, …      le cœur, en C
└── emulators/
    ├── melonDS/
    ├── azahar/
    └── Cemu/
```

Les trois compilent les mêmes fichiers C. Le pont propre à chaque
émulateur est le seul C++ du chemin, et il n'existe que parce que leurs
API le sont.

---

## Construire

```sh
sudo apt install libavcodec-dev libavutil-dev libswscale-dev \
                 libswresample-dev libsdl2-dev libgtk-3-dev
make
```

Ça donne le serveur autonome (avec une mire, pour travailler sans
émulateur), le client Linux et le lanceur.

Les émulateurs se compilent normalement, avec leurs propres
instructions. Le pont s'active tout seul.

---

## Le lanceur

```sh
make launcher/bs_launcher && ./launcher/bs_launcher
```

Il fait les trois choses qu'il faut régler avant chaque lancement —
diffusion, port, résolution interne — et rien de plus : charger un jeu
ou mapper une manette reste le travail de l'émulateur.

L'interrupteur et le port passent par l'environnement (`BOTTOM_SCREEN`,
`BOTTOM_SCREEN_PORT`), donc un lancement ne réécrit jamais un réglage
posé à la main. La résolution, elle, vit dans trois formats différents,
chacun avec son piège : le marqueur `\default` d'Azahar, le renderer
OpenGL de melonDS, la vue GamePad et l'API graphique de Cemu. Le
lanceur les connaît. Les fichiers touchés sont sauvegardés à côté.

Le port affiché est celui **réellement ouvert** : un serveur dont le
port est pris prend le suivant, ce qui permet aux trois émulateurs de
tourner ensemble.

`--set-resolution <émulateur> <n>` fait le réglage sans fenêtre.

---

## Les clients

**Android** — `android/`, Kotlin, décodage matériel via MediaCodec
directement dans une Surface. Boutons virtuels déplaçables, sticks quand
la console en a, son avec volume et sourdine, plein écran, et une liste
des serveurs connus pour ne pas retaper une adresse.

```sh
cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

**Linux** — `bottom_screen_client`, SDL2, pratique pour vérifier une
chaîne sans téléphone.

```sh
./bottom_screen_client --host 192.168.1.20 --port 5090
```

Quatre clients peuvent regarder en même temps. L'image est encodée une
fois pour tout le monde ; les boutons se combinent entre eux.

---

## Ce qui marche

Vidéo, tactile, boutons, sticks et son sur les trois émulateurs, du
serveur au téléphone.

La résolution interne suit sur les trois : monter le rendu en x2, x4 ou
plus change la taille du flux, et le serveur renégocie avec les clients
déjà connectés au lieu de les lâcher.

---

## Ce qui ne marche pas encore

- **Le menu 3DS.** Azahar plante à la connexion de l'Artic Setup Tool,
  donc les fichiers système ne sont pas installés. Les jeux et les
  homebrews tournent.
- **Cemu en Vulkan.** La lecture de la vue GamePad n'existe que sur le
  chemin OpenGL. Le lanceur bascule l'API pour cette raison.
- **Le homebrew Switch et le client web** ne sont pas écrits.

Le détail est dans [ROADMAP.md](ROADMAP.md).

---

## Le protocole

Un seul en-tête, [bs_protocol.h](bs_protocol.h), inclus des deux côtés.
TCP, port 5090 par défaut, H.264 pour l'image et Opus pour le son.

Un point qui se paie cher quand on l'oublie : les coordonnées tactiles
sont exprimées dans **l'espace annoncé par le serveur**, pas dans la
taille native de la console. Diviser par la seconde décale chaque appui
d'exactement le facteur de résolution, et ça ressemble à un problème de
calibration alors que c'est une multiplication.
