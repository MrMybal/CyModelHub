---
id: troubleshoot
title: Dépanner CyModelHub
description: Retrouvez les entrées manquantes et diagnostiquez les erreurs de service ou de téléchargement.
section: Aide
parent: overview
order: 50
sourceRevision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## Un modèle manque {#missing}

Vérifiez que le disque est monté, puis que le dossier est présent et activé dans
**Emplacements**. Lancez une analyse avec **F5** et effacez la recherche et les filtres.
Si vous aviez enregistré le fichier à la main avant de le déplacer, ajoutez son nouveau
chemin et oubliez l’ancienne entrée. Dans un dossier surveillé, une analyse peut retrouver
le modèle à son nouvel emplacement.

## L’application ne joint pas le service {#service}

Lancez `CyModelHubCli status` depuis le dossier d’installation. Vérifiez que
`CyModelHubService` se trouve à côté du programme client ou indiquez le chemin de
l’exécutable avec `CYMODELHUB_SERVICE`. Si vous utilisez `--data`, passez le même dossier à
l’application et à l’outil en ligne de commande. Vous pouvez arrêter l’instance pour ce
dossier avec `CyModelHubCli stop`, puis rouvrir l’application pour redémarrer le service.

## Un téléchargement échoue {#download-error}

Ouvrez **Activité** pour lire l’erreur. Vérifiez le chemin du fichier dans le dépôt ou son
adresse HTTPS, l’espace disque disponible et les droits d’écriture dans la destination
enregistrée. Pour un dépôt à accès restreint, vérifiez vos droits sur Hugging Face.
Si le contrôle d’intégrité échoue, comparez le SHA-256 saisi à la valeur publiée avant
de réessayer.

## Le manuel hors ligne ne s’ouvre pas {#offline}

Gardez `Documentation.html` et le dossier `DocData` complet dans le même dossier
`Documentation`. Ouvrez le fichier HTML depuis ce dossier local dans un navigateur.
