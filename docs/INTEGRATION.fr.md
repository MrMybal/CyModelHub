# Intégrer une application à CyModelHub

[English](INTEGRATION.md) · [Toutes les opérations](API.md) (en anglais) ·
[En-tête C++](../include/cymodelhub/client.hpp) · [Exemple complet](../examples/client/ensure_model.cpp)

Ce guide s'adresse à une application, ou à un hôte d'outils comme CyToolsCore, qui a besoin
d'**un modèle précis** et ne doit pas en garder une copie privée. Le service est commun à
toutes les applications de l'utilisateur ; les poids qu'il partage sont persistants.

Tout ce qui suit existe sous trois formes, avec les mêmes noms : le client C++
(`cymodelhub::Client`), HTTP local (`POST <url>/v1`) et les outils MCP.

## Le parcours, en bref

```cpp
#include <cymodelhub/client.hpp>
using cymodelhub::Availability;

cymodelhub::Package package;                 // ce dont l'application a besoin, écrit une fois
package.repository = "org/name";
package.revision   = "<commit>";             // un commit ne change pas ; une branche, si
package.variant    = "int6";
package.files = {{"model.fermion", 177000000, "<sha256>", "weights"},
                 {"config.json", {}, {}, "config"},
                 {"packed_manifest.json", {}, {}, "manifest"}};

cymodelhub::Requirement need;                // 1. chercher exactement ce modèle
need.repository = package.repository;
need.revision   = package.revision;
need.variant    = package.variant;
need.files      = package.files;
need.integrity  = "hashes";

cymodelhub::Client hub;                      // démarre le service s'il ne tourne pas
auto found = hub.check(need);
if (found.status == Availability::unverified && found.model) {
    hub.wait(hub.verify(found.model->id));   // présent mais jamais lu : le lire une fois
    found = hub.check(need);
}
if (found.ready()) {
    run(found.entry);                        // 2. utiliser le chemin partagé, tel quel
} else if (found.status == Availability::absent || found.status == Availability::installing ||
           found.status == Availability::incomplete) {
    cymodelhub::InstallRequest request;      // 3. l'installer, ou rejoindre l'installation en cours
    request.package = package;
    run(hub.wait(hub.install(request)).path);
    // 4. ou : le télécharger soi-même, puis
    //    run(hub.wait(hub.register_model(folder, package)).path);
} else {
    report(found.status_name, found.reasons, found.problems);
}
```

[`examples/client/ensure_model.cpp`](../examples/client/ensure_model.cpp) est ce même parcours,
sous la forme d'un programme qui se compile avec la seule bibliothèque cliente.

## Où vivent les poids partagés

- Les modèles ne sont installés que dans des **emplacements enregistrés** : le dépôt propre à
  CyModelHub (`<données>/models`) ou un dossier ajouté avec `roots_add`. Toute autre
  destination est refusée (`OutsideLocations`).
- Un modèle enregistré reste là où l'application l'a mis. Rien n'est copié ni déplacé.
- **Ne donnez jamais l'espace de travail temporaire d'une tâche** comme destination ni comme
  dossier d'un modèle enregistré, et n'enregistrez jamais un tel espace comme emplacement. Les
  poids partagés doivent survivre à la tâche : une tâche reçoit un chemin à lire, pas un
  dossier dont elle serait propriétaire.
- CyModelHub ne supprime jamais un fichier de modèle et n'en écrase jamais. Une application ne
  doit pas non plus supprimer ou réécrire un modèle partagé : d'autres applications utilisent
  les mêmes fichiers.

## Ce que signifient les réponses

`models_check` (`Client::check`) répond par un `status`. Une installation répond par une tâche
dont l'échec porte un `code`. Le client lève `cymodelhub::Error` avec les mêmes codes.

| Situation | Comment elle apparaît |
| --- | --- |
| Le modèle est utilisable | `ready`, avec `path`, `root` et `entry` |
| Modèle absent | `absent` |
| Service indisponible | `Error` `ServiceUnavailable` (non installé, ou démarrage impossible) |
| Installation en cours | `installing`, avec la tâche (`job`) à rejoindre |
| Modèle incomplet | `incomplete`, avec les fichiers manquants dans `problems` ; code de tâche `ModelIncomplete` |
| Incompatibilité | `incompatible`, avec `reasons` (format, architecture, quantification, révision, variante) |
| Accès fournisseur requis | code de tâche `ProviderAccessRequired` (HTTP 401 ou 403 de la source) |
| Échec de validation | `invalid` ; codes de tâche `ValidationFailed`, `HashMismatch`, `SizeMismatch` |
| Pas encore prouvé | `unverified` : présent, mais la preuve demandée n'a jamais été établie |
| Plusieurs modèles différents répondent | `ambiguous`, avec `candidates` : aucun n'est choisi |
| La source a échoué | code de tâche `DownloadFailed` ; `Cancelled` ; `Interrupted` après un arrêt du service |

`models_resolve` (`Client::resolve`) est inchangé : c'est la recherche approximative, pour les
personnes et pour « quelque chose comme ça ». Elle renvoie la meilleure correspondance par nom
et ne doit pas servir à décider qu'un modèle précis est installé.

## Résolution stricte

`models_check` reçoit ce qui identifie le modèle et ce qu'il doit être.

- **Quel modèle** : `id`, ou l'un de `repository`, `name`, `file`, `sha256`. La comparaison est
  exacte (sans tenir compte de la casse), jamais « contient » ni « le plus proche ».
- **Ce qu'il doit être** : `format`, `architecture`, `quantization`, `type`, `modality`,
  `revision`, `variant`. Un modèle trouvé mais différent est `incompatible`, avec la raison.
  Un nom de branche est accepté comme `revision` quand c'est celui depuis lequel le modèle a
  été installé.
- **Ses fichiers** : `files`, avec la taille (`size`) et l'empreinte (`sha256`) attendues. Un
  fichier manquant donne `incomplete` ; une autre taille ou une autre empreinte donne `invalid`.
- **Avec quelle certitude** : `integrity`.
  - `none` : être sur le disque suffit. À n'utiliser que si la présence est tout ce qu'il faut.
  - `sizes` (par défaut) : les tailles attendues sont connues (par la demande, ou par le
    contrat ou le manifeste du modèle) et correspondent.
  - `hashes` : le SHA-256 des fichiers obligatoires a été calculé et ils n'ont pas changé depuis.

Un dossier ou un fichier seulement vu par une analyse est `present`, pas `ready` : avec
l'`integrity` par défaut il est `unverified` tant que l'application ne donne pas les tailles ou
les empreintes attendues, ou que `models_verify` ne l'a pas lu. `models_check` lui-même ne lit
aucun contenu ; il compare ce qui a été enregistré avec les tailles et les dates de
modification sur le disque : un fichier disparu ou modifié est donc vu tout de suite.

Quand plusieurs modèles installés satisfont la demande, des copies d'un même contenu comptent
pour un seul modèle ; des contenus différents donnent `ambiguous`.

## Modèles complets

Un modèle peut demander plusieurs fichiers : poids, fragments (shards), index, configuration,
tokenizer, manifeste. Le contrat (`package`) le dit explicitement :

```json
{
  "identity":       { "name": "Nova", "repository": "org/nova", "revision": "<commit>", "variant": "q4" },
  "provenance":     { "source": "huggingface", "url": "https://...", "application": "CyToolsCore" },
  "classification": { "format": "gguf", "type": "model", "modality": "text",
                      "architecture": "llama", "quantization": "Q4_K_M" },
  "entry":          "weights/model-00001-of-00002.gguf",
  "files": [
    { "path": "weights/model-00001-of-00002.gguf", "size": 4368439584, "sha256": "...", "role": "weights" },
    { "path": "weights/model-00002-of-00002.gguf", "size": 2147483648, "sha256": "...", "role": "shard" },
    { "path": "config.json", "role": "config" },
    { "path": "extras/readme.md", "role": "notes", "required": false }
  ],
  "dependencies": [ { "repository": "org/nova-vae", "revision": "<commit>", "required": true } ]
}
```

Seul `files` est obligatoire. Les chemins sont relatifs au dossier du modèle, avec des barres
obliques ; un chemin avec un lecteur, une barre initiale, `..`, une barre oblique inverse ou un
nom réservé est refusé (`InvalidManifest`), et un lien qui ferait sortir un téléchargement du
dossier est refusé (`UnsafePath`). `entry` est le fichier que le moteur ouvre ; laissez-le vide
quand le moteur prend le dossier. Les dépendances sont enregistrées avec le modèle ; elles ne
sont pas installées pour vous.

Après un contrôle, le modèle a :

- un **état** (`state`) : `ready`, `incomplete` (un fichier obligatoire manque) ou `invalid`
  (une taille ou une empreinte ne correspond pas, ou la structure d'un format connu est fausse) ;
- une **intégrité** (`integrity`) : `none`, `sizes` (les tailles annoncées correspondent),
  `recorded` (chaque fichier obligatoire a été lu et son SHA-256 enregistré, au moins un sans
  référence) ou `verified` (chaque fichier obligatoire correspond à un SHA-256 donné par le
  contrat ou annoncé par la source) ;
- par fichier, dans `package.files` : son état, sa taille et son empreinte, d'où chacune vient
  (`manifest` ou `measured`), et quand l'empreinte a été contrôlée pour la dernière fois.

L'empreinte structurelle d'un dossier (noms et tailles) ne sert qu'à repérer les copies. Elle
n'est jamais prise pour une vérification d'intégrité.

### Enregistrer un modèle que l'application possède déjà

`models_register` (`Client::register_model`) reçoit le dossier et le contrat. Chaque fichier
listé est cherché dans le dossier ; sa taille est comparée ; sauf si `verify` vaut `sizes`, il
est lu une fois pour calculer son SHA-256. La tâche se termine `done` avec le modèle **et son
état** : un modèle auquel il manque un fichier, ou dont un fichier est faux, est catalogué
`incomplete` ou `invalid`, jamais prêt. Rien n'est copié, déplacé ni réécrit, et les entrées de
fichiers isolés situés dans le dossier sont fondues dans l'unique entrée du modèle.

### Confier l'installation au Hub

`install_start` avec un `package` (`Client::install`) télécharge chaque fichier. Sans `baseUrl`,
les fichiers viennent du dépôt Hugging Face de l'identité, à sa révision ; avec `baseUrl`, ils
viennent de `baseUrl/chemin` (un miroir, ou `http://127.0.0.1` pour une source locale).

- Le modèle est assemblé dans `<nom>.incomplete`, à côté de sa place définitive.
- Chaque fichier reprend depuis son `.part` quand la source accepte les plages ; les fichiers
  déjà complets ne sont pas retéléchargés.
- Quand tous les fichiers sont là, **chacun est lu et contrôlé**. C'est seulement alors que le
  dossier prend son nom et que le modèle est publié `ready`. Jusque-là, `models_check` répond
  `absent` ou `installing`.
- Une installation échouée ou interrompue laisse son dossier provisoire ; la demande suivante
  pour le même modèle la continue. Un fichier du dossier provisoire qui échoue à son contrôle
  est retiré pour être retéléchargé.
- Un modèle publié qui a perdu un fichier est complété sur place par la même demande. Un
  fichier publié et endommagé n'est **jamais écrasé** : la tâche échoue avec `ValidationFailed`
  et le modèle reste `invalid` jusqu'à ce que quelqu'un remplace le fichier.

`models_verify` (`Client::verify`) contrôle à nouveau un modèle installé, à la demande.

## Une seule installation pour tout le monde

Chaque installation a une **identité** :

| Nature | Identité |
| --- | --- |
| Un fichier d'un dépôt | dépôt, révision (`main` si omise), fichier, SHA-256 attendu, variante |
| Un fichier à une adresse | l'adresse sans identifiants, SHA-256 attendu, variante |
| Un modèle complet | dépôt (ou l'adresse de base s'il n'y en a pas), révision, variante, et la liste de ses fichiers avec leur SHA-256 attendu |

Le nom affiché, le nom du dossier et la destination n'en font pas partie. Donnez un commit
comme `revision` pour une identité qui ne peut pas changer ; avec une branche, le commit auquel
la source l'a résolue est enregistré avec le modèle quand la source l'annonce.

- Une demande pour un modèle **en cours d'installation rejoint cette installation** : même
  tâche (`job`), `joined: true`, et un ticket (`ticket`) qui lui est propre. Tous les demandeurs
  obtiennent le même résultat.
- Une demande pour un modèle **déjà installé** l'obtient tout de suite : `installed: true`, le
  modèle (`model`), et une tâche déjà terminée, pour que chaque appelant attende de la même façon.
- **Une autre destination ne crée jamais une seconde copie.** La réponse nomme l'unique endroit
  (`destination`) et indique `destinationHonored: false`. Utilisez `models_link` pour rendre le
  modèle visible dans un autre dossier sans le copier.
- Deux modèles différents ne peuvent pas être écrits au même endroit en même temps
  (`DestinationBusy`).
- La réservation dure **jusqu'à l'arrêt réel de la tâche** et la finalisation de ses fichiers.
- Elle est écrite dans `<données>/installs.json`. Si le service s'arrête ou est tué, le service
  suivant liste l'installation comme `interrupted` (`installs_list`), et la demande suivante
  pour le même modèle la continue là où sont ses fichiers partiels (`resumed: true`).
- **Annulation** : un demandeur appelle `install_leave` avec son ticket (`Client::leave`). Le
  travail continue pour les autres et n'est annulé que lorsque le dernier s'en va. `jobs_cancel`
  sur une tâche attendue par plusieurs demandeurs répond `shared: true` et n'arrête rien, sauf
  avec `force` (ce que fait l'application de bureau, à la demande explicite de l'utilisateur).

## Demandes qui peuvent être envoyées deux fois

Une demande qui modifie quelque chose peut porter une `idempotencyKey` : 8 à 128 caractères
parmi les lettres, les chiffres, `.`, `_`, `:` et `-`, choisis au hasard, sans aucun secret.

| Renvoyée avec | Résultat |
| --- | --- |
| la même clé et les mêmes arguments | la première réponse, avec `replayed: true` ; aucun second effet. Pour une tâche, le même `job` et, une fois terminée, son issue (`outcome`) |
| la même clé et d'autres arguments | `IdempotencyConflict` |
| la même clé après un arrêt du service au milieu de sa tâche | le travail est repris (`resumed: true`) |

- Les clés sont conservées **7 jours** (2000 au plus, les plus anciennes partent d'abord) dans
  `<données>/requests.json`, lisible par l'utilisateur seul, et survivent aux redémarrages.
  `jobs_get` répond encore pour la tâche d'une demande à clé après un redémarrage.
- La clé est stockée sous forme de SHA-256. L'empreinte d'une demande est un hachage de ses
  arguments **sans `token`**. Aucune des deux ne permet de retrouver un identifiant.
- Une demande refusée (arguments incorrects, destination hors des emplacements) ne consomme
  pas sa clé.
- Les contrôles d'une opération sont appliqués de nouveau avant de rendre une réponse
  mémorisée : un `install_start` mémorisé dont la destination n'est plus un emplacement
  enregistré est refusé.

Le client C++ suit ces règles. `install()`, `register_model()` et `verify()` créent une clé et
la renvoient inchangée. `invoke()` ne répète une demande après une erreur réseau ou un délai
dépassé que si cela ne peut pas répéter un effet : la demande n'a jamais quitté le programme,
l'opération ne fait que lire, ou les arguments portent une `idempotencyKey`. Sinon l'erreur
remonte à l'appelant, qui sait si un nouvel envoi est acceptable.

## Modèles de parole Fermion (`.fermion`)

Un modèle Fermion (famille Phonon, `fermion-research`) est un **dossier** de trois fichiers :
`model.fermion`, `config.json` et `packed_manifest.json`. C'est le dossier que reçoit le moteur.

CyModelHub reconnaît la vraie structure du conteneur, qui n'a pas de nombre magique : une
longueur sur 64 bits non signés, petit-boutiste, un index JSON `{"format", "index": [{"n", "k",
"shape", "b"}]}`, puis les enregistrements. En ne lisant que l'index (quelques kilo-octets), il
vérifie que :

- l'index tient dans le fichier et est bien le JSON attendu ;
- le format est un format `fermion-` ; pour `fermion-five-value-parakeet-v1`, chaque type
  d'enregistrement (`fp16`, `int6`, `int8`, `five_value`) a une longueur que sa forme permet ;
- le fichier a exactement la longueur du champ de longueur, de l'index et des enregistrements
  réunis : un fichier tronqué et des octets en trop sont tous deux refusés.

Pour le dossier, il vérifie aussi que le manifeste, la configuration et le conteneur sont
d'accord (format, taille, empreinte), et que le conteneur a la taille annoncée par le
manifeste. Le SHA-256 du conteneur est comparé à `container_sha256` par `models_verify` ou
`models_register`. Les poids ne sont jamais chargés et rien d'autre que du JSON n'est désérialisé.

- Un dossier valide est une seule entrée du catalogue : format `fermion`, architecture
  `parakeet`, domaine `audio`, quantification telle que `five-value-int6`, état `ready`,
  intégrité `sizes` tant que ses empreintes ne sont pas contrôlées. `path` et `entry` sont le
  dossier.
- Un conteneur seul est catalogué `incomplete` : il lui faut sa configuration et son manifeste.
- Un conteneur tronqué, un manifeste qui annonce une autre taille ou un autre format, ou une
  configuration qui contredit le manifeste rendent le modèle `invalid`.

Fermion distribue ces modèles sous forme d'archives (`.fsq`, `.bps.tar.zst`) que son propre
outil décompresse. CyModelHub ne les décompresse pas : pour ces modèles, l'application
télécharge et décompresse comme aujourd'hui, puis enregistre le dossier (`models_register`).
Les autres applications le trouvent ensuite avec `models_check`.

## Identifiants

- `token` (pour les téléchargements protégés) est envoyé à l'origine de l'adresse pour laquelle
  il a été donné, et à aucun autre hôte vers lequel mène une redirection. Il n'est jamais écrit
  sur le disque, journalisé ni renvoyé.
- `HF_TOKEN` est utilisé pour `huggingface.co` quand aucun jeton n'est donné.
- Une adresse est stockée et affichée sans sa requête, son fragment ni sa partie utilisateur.
  Un identifiant placé dans la requête sert au téléchargement et n'est pas conservé.

## Limites

- Compilé et testé sous Windows seulement. Le code pour Linux et macOS n'a pas été compilé.
- L'installation depuis `huggingface.co` lui-même n'a pas été exercée : les tests utilisent un
  serveur local. Les fichiers d'un dépôt ne sont pas découverts : l'appelant les liste.
- Les dépendances sont enregistrées, ni résolues ni installées.
- Rien n'est réparé en supprimant ou en écrasant. Les dossiers provisoires (`.incomplete`) et
  les fichiers partiels (`.part`) des installations qui ne se terminent jamais restent jusqu'à
  ce que quelqu'un les retire.
- Un demandeur qui disparaît sans appeler `install_leave` garde son ticket : l'installation se
  termine, mais les demandeurs restants ne peuvent pas l'annuler sans `force`.
- Un changement de contenu est repéré par la taille et la date de modification (à la seconde
  près). `models_verify` lit les fichiers et constitue le vrai contrôle.
- Seuls les types d'enregistrement de `fermion-five-value-parakeet-v1` sont connus. Un autre
  format `fermion-` n'est contrôlé que pour sa taille globale.
- L'application de bureau n'affiche pas encore les nouveaux états ; elle liste un modèle complet
  comme une seule entrée.
