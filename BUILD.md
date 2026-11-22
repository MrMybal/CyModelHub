# Compiler CyModelHub / Build CyModelHub

Git, CMake 3.24 ou plus récent et un compilateur C++20 sont nécessaires ; Python 3 pour le
test d'installation et les scripts. CMake récupère les versions épinglées de CyMCPSDK, de
CyUI et de SDL3.

Git, CMake 3.24 or newer and a C++20 compiler are required; Python 3 for the installation
test and the scripts. CMake fetches the pinned versions of CyMCPSDK, CyUI and SDL3.

Pour compiler sans réseau, indiquez des copies locales / To build offline, point at local
checkouts: `-DFETCHCONTENT_SOURCE_DIR_CYMCPSDK=...`, `-DFETCHCONTENT_SOURCE_DIR_CYUI=...`,
`-DFETCHCONTENT_SOURCE_DIR_SDL3=...`.

| Option | Défaut / Default | |
| --- | --- | --- |
| `CYMODELHUB_BUILD_SERVICE` | ON | Service, relais MCP, CLI / service, MCP relay, CLI |
| `CYMODELHUB_BUILD_APP` | ON | Application de bureau / desktop application (CyUI, SDL3) |
| `CYMODELHUB_BUILD_TESTS` | ON | Tests |

Intégré par `add_subdirectory`, seul le client `CyModelHub::Client` est compilé.
Embedded with `add_subdirectory`, only the `CyModelHub::Client` library is built.

### Windows

Visual Studio 2022 avec les outils C++ et le SDK Windows. Le script trouve Visual Studio, son
CMake et son Ninja, et utilise les dépôts voisins (`../CyMCPSDK`, `../CyUI` ou
`../CyImgui_Interface`) s'ils existent.

Visual Studio 2022 with the C++ tools and the Windows SDK. The script finds Visual Studio,
its CMake and its Ninja, and uses sibling checkouts when they exist.

```powershell
./scripts/build.ps1            # build/release, puis les tests / then the tests
./scripts/build.ps1 -NoApp     # sans l'application / without the desktop application
```

Pour installer la version de bureau dans `stage`, utilisez le CMake livré avec Visual Studio.
To install the desktop build in `stage`, use the CMake shipped with Visual Studio.

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$cyModelHubVS = & $vswhere -latest -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cyModelHubCMake = Join-Path $cyModelHubVS 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cyModelHubCMake --install build/release --component Hub --prefix stage
```

Lancez `stage/CyModelHub.exe` et ouvrez le manuel avec **F1**.
Run `stage/CyModelHub.exe` and press **F1** to open the manual.

Ou à la main / Or by hand:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --no-tests=error
cmake --install build --config Release --component Hub --prefix stage
```

Compilé et testé avec MSVC 19.40 sous Windows 10 x64.
Built and tested with MSVC 19.40 on Windows 10 x64.

### Linux

Non compilé à ce jour : ces commandes suivent celles des projets voisins.
Not built yet: these commands follow those of the sibling projects.

```bash
sudo apt-get install build-essential ninja-build libcurl4-openssl-dev libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev libwayland-dev wayland-protocols libxkbcommon-dev \
  libegl1-mesa-dev libgl1-mesa-dev libdbus-1-dev libdecor-0-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
cmake --install build --component Hub --prefix stage
```

### macOS

Non compilé à ce jour. Xcode ou ses outils en ligne de commande, avec une libc++ qui prend en
charge `std::jthread`.

Not built yet. Xcode or its command line tools, with a libc++ that supports `std::jthread`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
```

## Tests

| Test | |
| --- | --- |
| `core` | Détection des formats, analyse, catalogue et opérations, dans un seul processus. / Format detection, scanning, catalogue and operations, in one process. |
| `client` | Bout en bout : la bibliothèque démarre le vrai service ; API, MCP HTTP et relais MCP stdio. / End to end: the library starts the real service; API, MCP over HTTP and the MCP stdio relay. |
| `download` | Installation contre un serveur HTTP local (redirection, reprise, SHA-256). Aucun accès Internet. / Installation against a local HTTP server (redirect, resume, SHA-256). No Internet access. |

Les tests redirigent tous les dépôts de modèles connus vers un dossier temporaire : ils ne
lisent jamais vos modèles. / The tests redirect every well-known model store to a temporary
folder: they never read your models.

## Essayer sans modèles / Try it without models

```sh
python scripts/make_demo_library.py build/demo-library
CyModelHubCli --data build/demo-data add-root build/demo-library
CyModelHub --data build/demo-data
```

`--data` (ou la variable `CYMODELHUB_DATA`) isole le catalogue dans un dossier.
`--data` (or the `CYMODELHUB_DATA` variable) keeps the catalogue in a folder of its own.

## Scripts

| Script | |
| --- | --- |
| `scripts/build.ps1` | Compilation Windows / Windows build |
| `scripts/check_locales.py` | Traductions manquantes ou inutilisées / Missing or unused translations |
| `scripts/make_api_doc.py` | Régénère `docs/API.md` depuis le service / Regenerates `docs/API.md` from the service |
| `scripts/make_demo_library.py` | Faux modèles aux en-têtes réels / Fake models with real headers |
