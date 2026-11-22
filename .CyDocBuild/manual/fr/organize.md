---
id: organize
title: Classer, déplacer et partager les modèles
description: Corrigez le classement, examinez les doublons et déplacez ou liez des modèles dans les dossiers enregistrés.
section: Catalogue
parent: library
order: 20
sourceRevision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## Modifier la fiche d’un modèle {#classification}

Sélectionnez un modèle. Dans **Classement**, modifiez son nom, son domaine, son rôle
ou sa famille. Vous pouvez aussi ajouter des étiquettes et des notes. Cliquez sur
**Enregistrer**. L’étoile ajoute le modèle aux **Favoris**. Les analyses suivantes conservent
vos corrections. **Détecter à nouveau** rétablit le classement détecté pour cette entrée.

## Examiner les doublons {#duplicates}

Ouvrez **Doublons** pour voir les fichiers au contenu identique et l’espace estimé des
copies supplémentaires. Depuis chaque ligne, vous pouvez ouvrir le dossier ou la fiche
du modèle correspondant.

```image
{"id":"duplicate-list","src":"assets/duplicates.fr.png","alt":"Deux modèles de démonstration identiques dans des dossiers différents, sur la page Doublons","controls":"minimal"}
```

Dans la fiche du modèle, **Fichier → SHA-256 → Calculer** lance un calcul dont vous pouvez
suivre l’avancement dans **Activité**. **Oublier cette entrée** retire la fiche, ses étiquettes
et ses notes du catalogue. Une analyse peut retrouver le modèle s’il est dans un dossier surveillé.

## Déplacer ou lier un modèle {#share}

Ouvrez **Partager et déplacer** dans la fiche. Choisissez **Déplacer…** pour libérer de
l’espace à l’emplacement d’origine. Entre deux disques, le service copie et vérifie le modèle
avant de terminer le déplacement. Laissez **Laisser un lien à l’ancien emplacement** activé
si un autre logiciel utilise encore l’ancien chemin.

Choisissez **Lier dans un dossier…** pour qu’une autre application voie le modèle sans
copie supplémentaire. Un lien physique nécessite un fichier sur le même système de fichiers.
Un lien symbolique peut traverser les disques si le système le permet. Enregistrez le dossier
de destination dans [Emplacements](doc:library#locations) avant d’y déplacer, lier ou
télécharger un modèle.
