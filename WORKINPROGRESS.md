# bottom_screen_server — suivi de travail

Streamer l'écran du bas des consoles Nintendo (DS, 3DS, Wii U) depuis les
émulateurs vers un téléphone Android ou un homebrew Switch 1, avec retour
des inputs tactiles et boutons virtuels.

Cahier des charges initial : [goal.md](goal.md).

---

## État (2026-09-04)

Phase 2 terminée. melonDS stream son vrai écran du bas, et le tactile
envoyé par le réseau pilote la console. Vérifié sur le firmware DS : le
tap franchit l'écran d'avertissement et arrive au menu.

### Fait

- Miroirs privés créés sur `github.com/wozt` : `melonDS`, `azahar`, `Cemu`.
- Clones locaux dans `emulators/`, `origin` = miroir privé (SSH),
  `upstream` = repo officiel (HTTPS).
- Submodules récupérés (`--depth 1`) : melonDS aucun, azahar 52, Cemu 9.
- Repérage des API framebuffer et tactile dans les trois émulateurs
  (voir « Points d'accroche » plus bas).
- melonDS compilé tel quel sur Debian 13 : 223 cibles, 55 s sur 16 cœurs,
  aucune erreur. Binaire `emulators/melonDS/build/melonDS`, version 1.1.
- Phase 1 : serveur, client Linux et test headless écrits en C, zéro
  warning avec `-Wall -Wextra`. Voir « Phase 1 : mesures » plus bas.
- Phase 2 : melonDS patché et fonctionnel. Voir « Phase 2 : melonDS ».

### Pas encore commencé

Les clients Android et Switch, UDP, et les deux autres émulateurs.

---

## Décisions arrêtées

**Pas de capture de fenêtre.** On récupère le framebuffer de l'écran du
bas directement dans l'émulateur, avant composition. Une capture X11 /
Wayland ajouterait une copie, une conversion et de la latence, pour un
résultat moins fiable (fenêtre masquée, changement de layout, etc.).

**H.264, pas VP8.** Les deux clients cibles — Android et Switch 1 — ont
un décodeur H.264 matériel. Le coût d'encodage est négligeable à ces
résolutions ; c'est le décodage côté client qui compte.

**Miroirs privés, pas de forks.** GitHub interdit qu'un fork d'un repo
public soit privé. Les trois repos sont donc des copies privées poussées
manuellement. Conséquence : pas de PR possible vers l'upstream depuis
ces repos, et la resynchro se fait à la main via le remote `upstream`.

**Résolutions natives, jamais d'étirement.** Le serveur envoie la
résolution native ; le client peut agrandir par multiples entiers, mais
le framebuffer décodé reste natif.

| Console | Résolution écran bas |
|---|---|
| Nintendo DS | 256 × 192 |
| Nintendo 3DS | 320 × 240 |
| Wii U GamePad | 854 × 480 |

Vérifié dans `emulators/azahar/src/core/3ds.h:16-19` : l'écran du bas
de la 3DS fait bien 320×240, c'est celui **du haut** qui fait 400×240.
Le 400×240 souvent cité pour le tactile est une erreur courante.

---

## Points d'accroche dans le code

Repéré par lecture du code, pas encore testé à l'exécution.

### melonDS — le plus simple des trois

Tout est déjà exposé proprement, sans rien à refactoriser.

| Quoi | Où |
|---|---|
| Framebuffers séparés haut / bas | `src/GPU.h:75` — `GPU::GetFramebuffers(void** top, void** bottom)` |
| Injection tactile | `src/NDS.h:419` — `NDS::TouchScreen(u16 x, u16 y)` |
| Relâchement tactile | `src/NDS.h:420` — `NDS::ReleaseScreen()` |
| Boutons | `src/NDS.h:422` — `NDS::SetKeyMask(u32 mask)` |
| Boucle où tout est appliqué | `src/frontend/qt_sdl/EmuThread.cpp:258` |

`GetFramebuffers` renvoie `true` si les framebuffers sont en RAM, `false`
si le renderer est GPU — dans ce cas les valeurs sont spécifiques au
renderer (handle de texture OpenGL). Il faudra gérer les deux cas, ou
forcer le renderer software pour le premier prototype.

`EmuThread.cpp:258` montre le modèle exact à suivre : le thread
d'émulation lit `emuInstance->isTouching` / `touchX` / `touchY` à chaque
frame. Injecter le tactile réseau revient à écrire dans ces champs
depuis le serveur — pas besoin de toucher au cœur de l'émulation.

### Azahar — bonne base, déjà un readback existant

| Quoi | Où |
|---|---|
| Tactile appuyé | `src/core/frontend/emu_window.h:200` — `TouchPressed(x, y)` |
| Tactile déplacé | `src/core/frontend/emu_window.h:210` — `TouchMoved(x, y)` |
| Tactile relâché | `src/core/frontend/emu_window.h:203` — `TouchReleased()` |
| Géométrie des écrans | `src/core/frontend/framebuffer_layout.h:28` — `struct FramebufferLayout` avec `bottom_screen` et `bottom_screen_enabled` |
| Textures des écrans | `src/video_core/renderer_opengl/renderer_opengl.h:98` — `std::array<ScreenInfo, 3> screen_infos` |
| Readback GPU déjà écrit | `src/video_core/renderer_opengl/frame_dumper_opengl.h:33` — `FrameDumperOpenGL`, avec PBO et `PresentLoop` |

`FrameDumperOpenGL` est la trouvaille intéressante : Azahar sait déjà
faire un readback de frames avec des PBO pour l'enregistrement vidéo.
C'est le modèle à copier — voire à réutiliser — pour extraire l'écran
du bas sans stall GPU.

`TouchPressed` prend des coordonnées **framebuffer**, pas des
coordonnées écran : il faudra passer par `FramebufferLayout` pour
convertir. Bon point, la conversion est déjà faite par l'émulateur.

### Cemu — le modèle correspond exactement au besoin

Cemu rend déjà l'écran GamePad séparément de l'écran TV, via un simple
booléen `padView` qui traverse tout le pipeline de rendu.

| Quoi | Où |
|---|---|
| Copie vers le backbuffer | `src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp:865` — `LatteRenderTarget_copyToBackbuffer(textureView, bool isPadView)` |
| Appel TV vs GamePad | même fichier, lignes 1010 (pad) et 1012 (TV) |
| Géométrie de la vue | même fichier, ligne 828 — `LatteRenderTarget_getScreenImageArea(..., bool padView)` |
| Interface renderer | `src/Cafe/HW/Latte/Renderer/Renderer.h:78` — `DrawBackbufferQuad(..., bool padView, ...)` |
| Lecture VPAD par le jeu | `src/Cafe/OS/libs/vpad/vpad.cpp:220` — `VPADRead()` |
| Validité du tactile | `src/Cafe/OS/libs/vpad/vpad.cpp:237` — `tpData.validity` |
| État tactile du pad | `src/input/InputManager.h:90` — `MouseInfo m_pad_touch` (position + `left_down`), lu via `get_mouse_position(bool pad_window)` |

Le point d'injection tactile le plus probable est `m_pad_touch` :
alimenter cette structure depuis le réseau au lieu de la souris de la
fenêtre GamePad. **Non vérifié** — le chemin complet entre `m_pad_touch`
et le remplissage de `tpData` dans `VPADRead` reste à tracer.

Côté vidéo, `LatteRenderTarget_copyToBackbuffer(texView, true)` ligne
1010 est le point unique où passe l'image du GamePad. C'est là qu'on
branche l'encodeur.

---

## Build sur Debian 13

### melonDS — vérifié le 2026-09-04

Le `BUILD.md` amont ne liste que Ubuntu, Fedora et Arch. Équivalent
Debian 13 (`libpcap-dev` remplace `libpcap0.8-dev`, qui n'est qu'un
paquet de transition) :

```bash
sudo apt install extra-cmake-modules libcurl4-gnutls-dev libpcap-dev \
  libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev \
  qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev
```

```bash
cd emulators/melonDS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Aucun correctif nécessaire, aucune dépendance hors dépôts Debian
stable. CMake détecte Wayland 1.23.1 et EGL 1.5 tout seul. `build/`
est déjà dans le `.gitignore` amont, donc l'arbre reste propre.

Toolchain de référence : cmake 3.31.6, g++ 14.2, ninja, Debian 13.6.

### Azahar et Cemu

Pas encore tentés. Cemu passe par vcpkg (submodule `dependencies/vcpkg`),
donc son premier build sera nettement plus long.

## Briques réutilisables de capture2cloud

`/home/wozt/dev/capture2cloud` contient déjà l'essentiel du côté client
et du transport.

| Brique | Fichier | Réutilisation |
|---|---|---|
| Protocole binaire natif | `c2s_protocol.h` | base du protocole vidéo + input |
| Client Switch homebrew | `switch_homebrew/` | squelette du client Switch |
| Stream Switch | `switch_stream.c`, `switch_stream.h` | transport direct socket, sans WebRTC |
| App Android native | `android/` | squelette du client Android |
| Boutons virtuels tactiles | `web/`, `page.html` | logique d'overlay multitouch, d-pad avec diagonales |
| Remapping manette | `gamepad_bridge.c` | mapping des manettes physiques |

Le chemin « socket + protocole binaire » déjà utilisé pour la Switch et
Android est exactement ce qu'il faut ici : pas de WebRTC, pas de jitter
buffer.

---

## Phase 1 : mesures

Mesuré le 2026-09-04, serveur et client sur la même machine, boucle
locale, DS 256×192 à 60 fps, `libx264` en `ultrafast` + `zerolatency`.

| | |
|---|---|
| Débit | 0,75 – 0,84 Mbit/s |
| Cadence | 60,0 fps, stable |
| Latence pipeline | **0,7 ms** |

Ce que ce 0,7 ms contient : conversion BGRA→YUV, encodage, transport TCP
sur la boucle locale, décodage. Ce qu'il ne contient pas : le Wi-Fi, et
la présentation à l'écran du client. Autrement dit le codec n'est pas le
problème — le budget de latence sera dépensé ailleurs, ce qui est
exactement ce qu'on voulait savoir avant d'aller plus loin.

Le débit mérite d'être relu quand la source sera un vrai jeu : la mire
est en grande partie statique, donc x264 la compresse très bien.

### Fichiers

| Fichier | Rôle |
|---|---|
| `bs_protocol.h` | protocole, inclus par les deux bouts |
| `bs_source.h` | le joint que les backends émulateurs rempliront |
| `bs_encoder.c/.h` | encodage H.264 via libavcodec |
| `bs_decoder.c/.h` | décodage, pour le client Linux uniquement |
| `bs_net.c/.h` | transport TCP et cadrage des messages |
| `testpattern.c` | source synthétique, affiche aussi les inputs reçus |
| `bottom_screen_server.c` | serveur |
| `bottom_screen_client.c` | client Linux SDL |
| `tests/smoke_client.c` | vérification bout en bout sans écran |

```bash
make                          # les deux binaires
make test                     # vérification headless
./bottom_screen_server --console ds
./bottom_screen_client --scale 3
```

### Décisions prises en écrivant le code

**`TCP_NODELAY`, systématiquement.** Sans lui, Nagle retient les petits
paquets jusqu'à 40 ms — plus de deux frames à 60 Hz, et parfaitement
invisible dans un test de débit.

**Pas de `AV_CODEC_FLAG_GLOBAL_HEADER`.** Avec ce drapeau, les SPS/PPS ne
vivent que dans l'extradata et n'apparaissent jamais dans le flux : un
client qui arrive en cours de route, ou qui a perdu le premier datagramme
en UDP, ne peut plus configurer son décodeur. Sans lui, x264 répète les
en-têtes avant chaque keyframe et n'importe quel client peut démarrer à
la keyframe suivante.

**Pas de B-frames.** Elles imposent de retenir une image pour coder la
suivante : une frame entière de latence pour un gain de compression dont
on n'a pas besoin à cette taille.

**Le client sort du YUV, pas du RGB.** SDL téléverse le YUV directement
au GPU qui fait la conversion en dessinant. Convertir en RGB côté CPU
ajouterait une passe pleine image par frame pour un résultat que le GPU
produit gratuitement.

**Le thread d'input est séparé de la boucle vidéo.** La boucle vidéo
passe son temps bloquée sur l'échéance de la frame suivante ; y lire
aussi l'input retiendrait chaque événement jusqu'à cette échéance.

**Mise à l'échelle par entiers uniquement.** 256×192 ×3 donne 768×576 et
chaque pixel source vaut exactement 9 pixels écran. Le reste est noir.

### Un bug que le test a attrapé

Le thread d'input positionnait le drapeau d'arrêt **global** quand le
client se déconnectait : le serveur s'arrêtait complètement dès le
premier client parti, au lieu de se remettre en écoute. Corrigé avec un
drapeau par connexion. C'est le genre de chose qu'un test manuel ne voit
pas — on relance le serveur sans y penser.

Ajouté dans la foulée : `bs_encoder_request_keyframe()`, appelé à chaque
connexion. Un client qui arrive n'a aucune image de référence et ne
décode rien jusqu'à la keyframe suivante, soit jusqu'à une seconde de
fenêtre noire.

### Limites connues

- Un seul client à la fois, et l'encodeur est partagé. Plusieurs clients
  simultanés demanderont soit un encodeur par client, soit un encodage
  partagé — à trancher quand le cas se présentera.
- TCP uniquement. Les champs de fragmentation existent dans l'en-tête
  mais ne sont pas utilisés.
- La latence affichée n'a de sens que si les deux bouts partagent une
  horloge, donc sur la même machine. Entre deux machines c'est la
  différence entre deux horloges monotones sans rapport.

## Phase 2 : melonDS

Mesuré le 2026-09-04 sur le firmware DS, renderer software, boucle
locale.

| | |
|---|---|
| Cadence | 60,2 fps |
| Débit | 0,57 Mbit/s (écran de menu) |
| Luma | min 0, max 255, moyenne 168 |

Le débit varie énormément selon le contenu : 0,03 Mbit/s sur l'écran noir
du démarrage, 0,57 sur le menu. Un vrai jeu en mouvement sera bien plus
haut — ces chiffres ne sont pas une prévision.

Les statistiques de luminance existent pour une raison précise : un flux
peut être parfaitement bien formé et ne rien montrer du tout. Si
l'émulateur avait passé un buffer vide, toutes les frames se
décoderaient, à une cadence plausible, et entièrement blanches. C'est
l'écart entre « le tuyau tourne » et « le tuyau transporte une image ».

### Comment le tactile a été prouvé

Le firmware DS s'arrête sur un écran d'avertissement qui attend un appui
tactile. En envoyant un tap depuis le réseau, l'écran est passé au menu
DS. C'est une preuve difficile à contester : rien d'autre ne pouvait
faire avancer cet écran.

Ça a d'ailleurs révélé un défaut du test : il envoyait des `TOUCH_DOWN`
sans jamais de `TOUCH_UP`. La console voyait donc un stylet posé et
jamais relevé, et un logiciel qui attend un appui attendait
indéfiniment. Le test fait maintenant de vrais taps.

### Ce qui a été modifié dans melonDS

Trois fichiers, et volontairement peu :

| Fichier | Changement |
|---|---|
| `src/frontend/qt_sdl/BottomScreenBridge.cpp/.h` | nouveau, le seul C++ du chemin |
| `src/frontend/qt_sdl/EmuThread.cpp` | 34 lignes : un include et deux hooks |
| `src/frontend/qt_sdl/CMakeLists.txt` | 39 lignes, intégration optionnelle |

`EmuInstance` n'est pas touché du tout. Ses champs d'input sont privés,
mais `EmuThread` est déjà déclaré `friend` — en accrochant là, on écrit
dans les mêmes champs que le frontend Qt, au même moment de la frame,
plutôt que d'élargir une interface.

Le pont ne démarre le serveur qu'à la première frame soumise, donc rien
ne s'ouvre tant qu'aucun jeu ne tourne.

```bash
BOTTOM_SCREEN=0      # désactiver
BOTTOM_SCREEN_PORT   # port d'écoute, défaut 5090
```

Ce sont des variables d'environnement et non un panneau de réglages :
une vraie interface Qt demanderait de toucher beaucoup plus de melonDS
que ce que ça vaut à ce stade.

### Le serveur est devenu une bibliothèque

La logique vivait dans un `main()`, ce qui allait tant que la seule
source était une mire. Un émulateur ne se réorganise pas autour du
`main` de quelqu'un d'autre, donc tout est passé dans `bs_server.c` et
tourne sur son propre thread. Le binaire autonome est maintenant un
`main` mince par-dessus le même code — les deux ne peuvent plus diverger.

`bs_mailbox.c` fait le joint entre les deux modèles. La mire fabrique
une frame quand on lui en demande une ; un émulateur, lui, finit sa frame
et passe à autre chose sans qu'on puisse le faire attendre. La boîte aux
lettres est en « le dernier gagne » : si deux frames arrivent avant que
le serveur en prenne une, la première est perdue. C'est voulu — une file
échangerait une frame perdue contre de la latence croissante, et sur un
écran de jeu, en retard est pire qu'absent.

### Limites connues

- **Renderer OpenGL non supporté.** `GetFramebuffers` renvoie `false` et
  le pointeur est alors un identifiant de texture : l'écran du bas est
  sur le GPU, il n'y a rien en RAM à streamer. Le pont l'écrit une fois
  sur la sortie d'erreur au lieu de streamer du vide. Le readback GPU
  reste à faire.
- **Les boutons ne sont pas vérifiés visuellement.** Le mapping est lu
  depuis l'ordre réel des touches de melonDS, et le masque est actif à
  l'état bas comme il se doit, mais aucun test n'a encore appuyé sur un
  bouton pour le voir agir.
- Le tactile local garde la priorité : un client ne pilote l'écran que
  si la souris n'est pas déjà dessus.

## Plan par phases

### Phase 0 — préparation
- [x] Miroirs privés des trois émulateurs
- [x] Clones locaux + submodules
- [x] Repérage des points d'accroche
- [x] Compiler melonDS tel quel sur Debian 13, sans modification

### Phase 1 — tuyau bout en bout, sans émulateur
- [x] Définir le protocole (`VIDEO_CONFIG`, paquets vidéo, paquets input)
- [x] Serveur : mire de test → H.264 → réseau
- [x] Client Linux : réseau → décodage → fenêtre
- [x] Mode TCP de debug avant l'UDP
- [x] Mesurer la latence de bout en bout

### Phase 2 — melonDS
- [x] Extraire le framebuffer du bas via `GetFramebuffers`
- [x] Injecter le tactile via `TouchScreen` / `ReleaseScreen`
- [x] Injecter les boutons via `SetKeyMask`
- [x] Garder l'écran du haut sur le PC, inchangé

### Phase 3 — client Android
- [ ] Décodage H.264 par MediaCodec
- [ ] Affichage sans étirement
- [ ] Tactile → réseau
- [ ] Boutons virtuels, profil DS
- [ ] Manette Bluetooth

### Phase 4 — client Switch homebrew
- [ ] Décodage matériel
- [ ] Tactile + boutons virtuels
- [ ] Joy-Con pour les boutons normaux

### Phase 5 — Azahar (3DS)
- [ ] Backend framebuffer sur le modèle de `FrameDumperOpenGL`
- [ ] Tactile via `TouchPressed` / `TouchMoved` / `TouchReleased`
- [ ] Profil de boutons 3DS (ZL/ZR, circle pad, C-stick)

### Phase 6 — Cemu (Wii U)
- [ ] Backend sur `LatteRenderTarget_copyToBackbuffer(_, true)`
- [ ] Tracer et brancher le chemin tactile VPAD
- [ ] Profil de boutons Wii U

---

## Questions ouvertes

**Bibliothèque partagée ou code dupliqué ?** L'idée d'un
`libbottomscreen.so` intégré aux trois émulateurs est séduisante, mais
les trois ont des systèmes de build et des contraintes de licence
différents. À trancher après melonDS — inutile de concevoir
l'abstraction avant d'avoir un cas qui marche.

**Readback CPU ou partage GPU ?** On commence par le readback CPU le
plus simple. À ces résolutions, ça peut suffire largement. On
n'optimise qu'après mesure.

**Détection des frames identiques.** Ne rien envoyer quand l'image du
bas n'a pas changé (menus, jeux statiques) peut économiser beaucoup.
À évaluer une fois le pipeline en place, pas avant.

**Resynchro avec l'upstream.** Nos modifications vivent sur des
branches dédiées, à décider : `bottom-screen` sur chaque miroir, rebasé
périodiquement sur `upstream/master`.

---

## Licences

| Émulateur | Licence | Contrainte |
|---|---|---|
| melonDS | GPL-3.0 | sources à publier si distribution de binaires modifiés |
| Azahar | GPL-2.0 | idem |
| Cemu | MPL-2.0 | seuls les fichiers modifiés seraient à publier |

Aucune contrainte tant que les modifications restent locales. Les
copies privées sont autorisées par les trois licences.
