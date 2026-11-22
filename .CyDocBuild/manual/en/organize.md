---
id: organize
title: Organize, move and share models
description: Edit classification, compare duplicate files, and use registered folders to move or link models.
section: Catalogue
parent: library
order: 20
revision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## Edit model details {#classification}

Select a model, then use **Classification** to edit its name, kind, role or family and add
tags or notes. Choose **Save**. Use the star to include it in **Favourites**. Manual
classification is retained on later scans. **Detect again** restores the detected
classification for that entry.

## Review duplicates {#duplicates}

Open **Duplicates** for files with identical content and the estimated space used by the
extra copies. Each row can open the containing folder or take you to the model record.

```image
{"id":"duplicate-list","src":"assets/duplicates.png","alt":"Two identical demo models in separate folders on the Duplicates page","controls":"minimal"}
```

For the selected model, **File → SHA-256 → Compute** runs a hash job. You can see its progress
under **Activity**. **Forget this entry** removes its record, tags and notes from the
catalogue. A future scan can rediscover it in a watched folder.

## Move or link a model {#share}

Open **Share and move** in the model details. Choose **Move…** to free
space at the original location. Across drives the service copies and verifies the model
before completing the move. Keep **Leave a link at the old place** enabled when other
software still uses the old path.

Choose **Link into a folder…** to make the model visible to another application without
a second copy. A hard link requires a file on the same filesystem. A symbolic link can
span drives if the operating system permits it. Register the destination folder in
[Locations](doc:library#locations) before moving, linking or downloading into it.
