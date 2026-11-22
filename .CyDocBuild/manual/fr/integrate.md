---
id: integrate
title: Connecter des scripts, applications et assistants
description: Retrouvez les chemins des modèles via la ligne de commande, le client C++, HTTP local ou MCP.
section: Intégration
parent: overview
order: 40
sourceRevision: 1
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
Utilisez `jobs_get` pour suivre cette tâche ou `Client::wait_job()` en C++.
