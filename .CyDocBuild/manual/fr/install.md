---
id: install
title: Télécharger un modèle
description: Téléchargez depuis Hugging Face ou une adresse HTTPS directe et suivez la tâche dans Activité.
section: Catalogue
parent: library
order: 30
sourceRevision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## Lancer un téléchargement {#download}

1. Choisissez **Installer…**.
2. Pour **Hugging Face**, indiquez le dépôt au format `propriétaire/nom` et le chemin exact
   du fichier dans ce dépôt. Si l’accès est restreint, acceptez les conditions du dépôt
   avec votre compte Hugging Face et fournissez un jeton Hugging Face autorisé.
3. Pour **Adresse**, entrez l’URL HTTPS directe. Lorsque l’éditeur fournit un SHA-256,
   saisissez-le pour que CyModelHub puisse vérifier le fichier téléchargé.
4. Choisissez une destination dans un [emplacement enregistré](doc:library#locations) puis
   cliquez sur **Télécharger**. Vous pouvez choisir le dossier de stockage de CyModelHub.

## Suivre le téléchargement {#progress}

Ouvrez **Activité** pour suivre la progression et consulter les erreurs. Utilisez le bouton
d’annulation pour arrêter un téléchargement en cours. CyModelHub peut reprendre depuis
un fichier partiel lorsque le serveur le permet. Une fois téléchargé, le modèle rejoint le
catalogue. Sélectionnez son entrée pour copier le chemin et l’utiliser dans votre moteur.
