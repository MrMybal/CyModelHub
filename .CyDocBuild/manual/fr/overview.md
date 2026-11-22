---
id: overview
title: Premiers pas avec CyModelHub
description: Ouvrez CyModelHub, repérez vos modèles déjà installés et retrouvez le fichier recherché.
section: CyModelHub
order: 0
sourceRevision: 3
# SPDX-License-Identifier: GPL-3.0-only
---

## Ouvrir CyModelHub {#start}

CyModelHub tient un catalogue des modèles d’IA déjà présents sur votre ordinateur.
L’application de bureau, la ligne de commande et la bibliothèque cliente utilisent le même
service local. Une fois le modèle installé ou enregistré, vos applications peuvent demander son chemin.

Sous Windows, conservez les fichiers installés ensemble et lancez `CyModelHub.exe`.
L’application démarre le service au besoin. Cette version a été vérifiée sous Windows x64.
Linux et macOS sont des cibles qui restent à compiler et à tester.

Sous Windows, cliquez sur l’icône CyModelHub près de l’horloge, puis sur **Réglages**.
La croix masque la fenêtre. Choisissez **Ouvrir CyModelHub** pour la réafficher ou
**Quitter CyModelHub** pour fermer l’application de bureau.

Appuyez sur **F1** ou choisissez **Réglages → Manuel utilisateur (F1)** pour ouvrir ce manuel.
Depuis une copie locale du dépôt ou du dossier installé, vous pouvez aussi ouvrir
`Documentation/Documentation.html` directement. Laissez le dossier `DocData` à côté du fichier.

## Mettre à jour l’application Windows {#app-updates}

Ouvrez **Réglages → Mises à jour de l’application → Vérifier les mises à jour**. Vous
pouvez inclure les préversions. Si un installateur plus récent est disponible,
choisissez **Télécharger la mise à jour**. Après la vérification du téléchargement,
choisissez **Installer la mise à jour** pour fermer l’application et ouvrir l’installateur.
Attendez d’abord la fin des tâches en cours. L’installateur conserve le catalogue et les réglages.

Si vous utilisez la version portable 0.1.0, quittez-la depuis la zone de notification,
attendez la fin des tâches, puis lancez `CyModelHubCli stop` avant d’ouvrir l’installateur
pour la première fois.

## Trouver votre premier modèle {#first-scan}

1. Ouvrez **Emplacements** pour voir les bibliothèques détectées sur cet ordinateur.
2. Choisissez **Ajouter… → Surveiller ce dossier** pour une bibliothèque rangée ailleurs,
   ou **Ajouter ce modèle** pour enregistrer un fichier ou un dossier de pipeline.
3. Cliquez sur **Analyser** ou appuyez sur **F5** pour actualiser le catalogue.
   Sélectionnez un modèle pour voir son classement et les détails de son fichier.

```image
{"id":"model-list","src":"assets/models.fr.png","alt":"Catalogue CyModelHub avec la liste des modèles et la fiche du modèle sélectionné","controls":"minimal"}
```

## Continuer {#next}

- [Trouver et enregistrer des modèles installés](doc:library#locations)
- [Classer, déplacer et partager des modèles](doc:organize#classification)
- [Télécharger un modèle](doc:install#download)
- [Connecter des scripts, des applications et des assistants](doc:integrate#cli)
- [Dépanner le catalogue ou le service](doc:troubleshoot#missing)
