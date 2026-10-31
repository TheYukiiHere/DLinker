# DLinker SDL3 / FLink

Projet C++ utilisant SDL3. La première étape met en place FLink :
un protocole TCP binaire minimal entre Windows et Lubuntu.

## Compilation

Depuis la racine du projet :

```powershell
cmake -S . -B build -G Ninja
cmake --build build
```

SDL3 est telecharge automatiquement par CMake dans le dossier de dependances
du build.

> Le dossier `build` est propre à chaque machine. Ne copiez pas le dossier
> `build` de Windows vers Lubuntu.

## Execution

```powershell
.\build\DLinkerSDL3.exe .\FLink
```

Le deuxième argument est le dossier FLink local. Utilisez un chemin absolu si
vous lancez l'exécutable par double-clic :

```powershell
.\build\DLinkerSDL3.exe C:\path\to\DLinker\FLink
```

Le chemin utilisé et chaque étape sont écrits dans `FLink/sync.log`.
L'agent Lubuntu écrit les mêmes informations dans `FLink/agent.log`.

## Agent Lubuntu

Sur Lubuntu, compiler puis lancer l'agent :

```bash
sudo apt update
sudo apt install -y build-essential cmake libx11-dev libxtst-dev libasound2-dev pkg-config
cd ~/Desktop/DLinker
rm -rf build
cmake -S . -B build -G "Unix Makefiles" -DDLINKER_BUILD_CLIENT=OFF
cmake --build build --target flink-agent
mkdir -p "$HOME/FLink"
./build/flink-agent "$HOME/FLink"
```

`/chemin/vers/FLink` était uniquement un exemple : remplacez-le par le
dossier réel utilisé sur Lubuntu. Le dossier `build` doit être supprimé
uniquement sur Lubuntu quand il contient un cache créé sur Windows.

L’option `-DDLINKER_BUILD_CLIENT=OFF` évite de télécharger et compiler SDL3
sur Lubuntu : l’agent Linux n’en a pas besoin. Ninja n’est donc pas requis.

Pour la vidéo, l’agent doit être lancé dans la session graphique Lubuntu
connectée à X11. Vérifiez que `DISPLAY` est défini :

```bash
echo "$DISPLAY"
```

Le journal `~/FLink/agent.log` indique si X11 et ALSA ont été détectés,
la résolution capturée, les connexions et les erreurs de flux.
Les messages de la console sont colorés : vert pour les flux actifs, jaune
pour les avertissements et rouge pour les erreurs.

Le client Windows se connecte à `192.168.50.2:5002`, envoie `HELLO`, `PING`
et un manifeste FLink. L'agent répond par `PONG`, son manifeste, puis les
fichiers absents ou plus récents. Le client envoie ensuite ses fichiers
absents ou plus récents vers Lubuntu. Les chemins sont limités au dossier
`FLink` et les fichiers transférés sont limités à 256 MiB. Le client écrit
les détails de chaque synchronisation dans `FLink/sync.log`.

La synchronisation actuelle est effectuée à la connexion. Les suppressions et
la surveillance continue seront ajoutées avec le service de synchronisation
permanent. Dans la fenêtre SDL3, `Ctrl` + `Windows` + `D` minimise la fenêtre.

Après la synchronisation, le client reçoit les trames vidéo X11 et les paquets
audio PCM ALSA. Le journal Windows `FLink/sync.log` et la console du client
affichent la réception des images et de l’audio.

Les paramètres sont dans [flink.conf.txt](./flink.conf.txt), copié dans le
dossier `FLink` si nécessaire : IP, port, largeur/hauteur (`0` = écran
complet), FPS, audio et entrées. Le client envoie les mouvements de souris
et les touches lorsque la fenêtre est active.

Copiez ce fichier dans les deux dossiers FLink avant le lancement :

```bash
cp flink.conf.txt "$HOME/FLink/flink.conf.txt"
```

Après chaque modification du code, reconstruisez et relancez l’agent. Un ancien
binaire peut continuer à afficher `1280x720` et accepter l’ancien protocole :

```bash
pkill -f '/build/flink-agent' || true
rm -rf build
cmake -S . -B build -G "Unix Makefiles" -DDLINKER_BUILD_CLIENT=OFF
cmake --build build --target flink-agent
./build/flink-agent "$HOME/FLink"
```

Le journal doit afficher `resolution envoyee 1920x1080` si la configuration
contient `screen_width=0` et `screen_height=0`.

La configuration CMake recherche directement `asoundlib.h`/`libasound` et
`XTest.h`/`libXtst`. Si les paquets sont déjà installés mais que l’ancien
cache indique encore qu’ils sont absents, supprimez `build` avant de relancer
CMake.

Le client Windows et l’agent Lubuntu doivent être reconstruits depuis le même
commit. Le protocole actuel est la version 2 ; si Lubuntu affiche
`version incompatible`, reconstruisez aussi le client Windows.

La capture complète peut produire un trafic important : par exemple
1920x1080 en BGRA représente environ 8 Mo par image, avant compression.
