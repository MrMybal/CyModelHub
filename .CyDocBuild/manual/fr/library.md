---
id: library
title: Trouver et enregistrer les modèles
description: Choisissez les dossiers surveillés, enregistrez un modèle isolé et filtrez le catalogue.
section: Catalogue
parent: overview
order: 10
sourceRevision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## Choisir où chercher {#locations}

Dans **Emplacements**, CyModelHub liste les bibliothèques Hugging Face, Ollama et LM Studio
qu’il détecte, son propre dossier de stockage et les dossiers que vous enregistrez.
Pour ajouter une bibliothèque, par exemple celle de ComfyUI, choisissez **Ajouter…**,
sélectionnez le dossier puis cliquez sur **Surveiller ce dossier**. Le service lance son
analyse. Pour actualiser un seul emplacement, utilisez son bouton d’analyse dans **Emplacements**.

Utilisez **Ajouter ce modèle** pour un fichier individuel ou un dossier de pipeline Diffusers,
Transformers ou MLX situé hors des dossiers surveillés. L’enregistrement d’un modèle existant
laisse ses fichiers à leur place.

## Rechercher et examiner {#search}

Utilisez le champ de recherche et les catégories de la barre latérale pour filtrer par
domaine, rôle ou provenance. Sélectionnez une entrée pour voir son fichier, son classement et
ses notes. **Ouvrir le dossier** affiche son emplacement. **Copier le chemin** permet de
transmettre le chemin à une autre application. Vous pouvez aussi
[chercher ce chemin depuis un script](doc:integrate#cli).

## Formats reconnus {#formats}

CyModelHub lit les en-têtes GGUF et safetensors pour relever les informations disponibles,
comme l’architecture, la quantification et la longueur de contexte. Un pipeline pris en
charge comprenant plusieurs fichiers apparaît comme un seul modèle. D’autres formats peuvent
être classés à partir du nom de fichier et des dossiers qui le contiennent.

Le catalogue ne charge pas les poids et ne désérialise pas les fichiers pickle. Lorsqu’une
entrée est signalée comme fichier pickle, utilisez un format de confiance dans votre moteur
d’exécution. Si le classement automatique est erroné, [modifiez la fiche](doc:organize#classification).
