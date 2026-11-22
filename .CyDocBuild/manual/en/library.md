---
id: library
title: Find and register models
description: Choose watched folders, register a single model and narrow down the catalogue.
section: Catalogue
parent: overview
order: 10
revision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## Choose where to look {#locations}

Under **Locations**, CyModelHub lists the Hugging Face, Ollama and LM Studio stores it
can find, its own storage folder and any folders you register. To add a library such as a
ComfyUI models folder, choose **Add…**, select the folder, then choose **Watch this folder**.
The service starts scanning it. To refresh one location, use its scan button in **Locations**.

Use **Add this model** for an individual file or a Diffusers, Transformers or MLX pipeline
folder outside watched locations. Registering an existing model leaves its files in place.

## Search and inspect {#search}

Use the search field and the sidebar categories to narrow the list by purpose, role or
source. Select an entry for its file details, classification and notes. **Open the folder**
reveals its location. **Copy the path** gives another application the path to that model.
You can also [resolve the path from a script](doc:integrate#cli).

## What CyModelHub recognizes {#formats}

CyModelHub reads GGUF and safetensors headers for available information such as architecture,
quantization and context length. A supported multi-file pipeline appears as one model.
Other formats may be classified from filenames and their surrounding folders.

The catalogue does not load weights or unpickle files. When an entry is marked as a pickle
file, choose a trusted format for use in your runtime. If an automatic classification is
wrong, [edit the model record](doc:organize#classification).
