---
id: integrate
title: Connecter des scripts, applications et assistants
description: Retrouvez les chemins des modèles via la ligne de commande, le client C++, HTTP local ou MCP.
section: Intégration
parent: overview
order: 40
sourceRevision: 2
# SPDX-License-Identifier: GPL-3.0-only
---

## Ligne de commande {#cli}

Lancez l’outil depuis le dossier d’installation ou ajoutez-le au chemin de recherche des
commandes de votre terminal. L’outil et l’application de bureau accèdent au même catalogue
lorsqu’ils utilisent le même dossier de données.

```text
CyModelHubCli status
CyModelHubCli list --type lora
CyModelHubCli where sd_xl_base_1.0
CyModelHubCli operations
```

`where` affiche le chemin du fichier ou du dossier qui correspond le mieux. Utilisez
`list --json` pour obtenir des données structurées. `operations` liste toutes les
opérations et les schémas de leurs paramètres.

## Client C++ {#cpp}

Le client est sous licence MIT et requiert C++20. Ajoutez le dépôt source de CyModelHub
à votre projet CMake avec `add_subdirectory`, puis liez `CyModelHub::Client`.

```cpp
#include <cymodelhub/client.hpp>

cymodelhub::Client hub;
auto model = hub.resolve("sd_xl_base_1.0", "model", "image");
if (model) {
    const auto& installed_path = model->path;
    // Transmettez installed_path au moteur de votre application.
}
```

`resolve()` renvoie une valeur vide en l’absence de résultat. Les erreurs du service
peuvent être interceptées en tant que `cymodelhub::Error`. `Client::invoke()` prend
le nom d’une opération et ses arguments JSON. Si l’exécutable du service est installé
ailleurs, indiquez son chemin dans `CYMODELHUB_SERVICE` ou dans `cymodelhub::Options`.

## Un modèle précis pour une application {#strict}

`resolve()` renvoie le nom le plus proche. Une application qui a besoin d'un modèle précis
pose plutôt la question stricte : quel modèle, ce qu'il doit être, et quels fichiers elle ouvre.

```cpp
cymodelhub::Requirement need;
need.repository = "org/name";
need.revision = "<commit>";
need.files = {{"model.gguf", 4368439584, "<sha256>"}};

auto found = hub.check(need);
if (found.ready()) {
    // found.entry est le fichier ou le dossier partagé. Utilisez-le là où il est.
}
```

La réponse n'est jamais un modèle ressemblant. Son état est l'un des suivants.

| État | Signification |
| --- | --- |
| `ready` | Le modèle est installé et contrôlé. Utilisez `path` ou `entry`. |
| `absent` | Il n'est pas installé. |
| `installing` | Une autre application l'installe. `install()` rejoint cette installation. |
| `incomplete` | Des fichiers obligatoires manquent. |
| `invalid` | Une taille ou un SHA-256 ne correspond pas. |
| `unverified` | Il est sur le disque, mais n'a jamais été contrôlé. Lancez `verify()`. |
| `incompatible` | Il est installé dans un autre format, une autre quantification, révision ou variante. |
| `ambiguous` | Plusieurs modèles différents correspondent. Aucun n'est choisi. |

Un modèle fait de plusieurs fichiers est décrit par un `Package` qui liste chaque fichier. Il
n'est annoncé `ready` que lorsque chaque fichier obligatoire est présent et contrôlé.

- `install()` télécharge le modèle, ou rejoint l'installation qu'une autre application a
  lancée. Le modèle est téléchargé une fois et stocké une fois.
- `register_model()` catalogue un modèle que votre application a déjà téléchargé, là où il
  se trouve. Rien n'est copié ni déplacé.
- `leave()` cesse d'attendre. L'installation continue pour les autres applications.

Les modèles partagés sont persistants. N'en installez et n'en enregistrez aucun dans le
dossier temporaire d'une tâche, et ne supprimez ni ne réécrivez un modèle partagé : d'autres
applications utilisent les mêmes fichiers.

## Assistants et HTTP local {#mcp}

Pour un client MCP compatible HTTP, utilisez **Réglages → Copier la configuration MCP**.
La configuration copiée contient un jeton d’accès local. Gardez-le privé. Un client qui
lance un serveur stdio peut utiliser l’exécutable `CyModelHubMcp` installé.

Dans les autres langages, envoyez du JSON à `POST <url>/v1` avec les en-têtes
`Content-Type: application/json` et `Authorization: Bearer <token>`. L’URL de bouclage
et le jeton se trouvent dans le fichier `service.json` du dossier de données. Cette
requête liste les adaptateurs LoRA installés.

```json
{"method":"invoke","name":"models_list","arguments":{"type":"lora"}}
```

Les opérations longues, comme `install_start`, renvoient un identifiant de tâche.
Utilisez `jobs_get` pour suivre cette tâche ou `Client::wait_job()` en C++. Une tâche qui a
échoué porte un `code` qui dit pourquoi, par exemple `ProviderAccessRequired` quand la source
demande un jeton d'accès.

Une opération qui modifie quelque chose accepte une `idempotencyKey`. Si vous renvoyez la
demande avec la même clé et les mêmes arguments, vous recevez la première réponse et rien ne
se produit deux fois. Utilisez une nouvelle clé aléatoire pour chaque nouvelle demande.
