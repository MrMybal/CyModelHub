---
id: overview
title: Get started with CyModelHub
description: Open CyModelHub, discover your installed models and find the file you need.
section: CyModelHub
order: 0
revision: 2
# SPDX-License-Identifier: GPL-3.0-only
---

## Open CyModelHub {#start}

CyModelHub keeps a catalogue of the AI models already on your computer. The desktop app,
command line and client library use the same local service. Install or register a model
once and other applications can ask for its path.

With a Windows build, keep the installed files together and launch `CyModelHub.exe`.
The desktop app starts the service as needed. Windows x64 is the verified platform for
this version. Linux and macOS are targets that have yet to be built or tested.

On Windows, select the CyModelHub icon near the clock and choose **Settings** to open
the settings directly. The window’s close button hides it. Select **Open CyModelHub**
to bring it back, or **Quit CyModelHub** to close the desktop app.

Use **F1** or **Settings → User manual (F1)** to open this manual. From a local copy
of the repository or installed files, you can also open `Documentation/Documentation.html`
directly. Keep its `DocData` folder next to it.

## Find your first model {#first-scan}

1. Open **Locations** to see the model stores detected on this computer.
2. Choose **Add… → Watch this folder** for a library stored elsewhere, or **Add this model**
   to register a single file or pipeline directory.
3. Choose **Scan** or press **F5** to refresh the catalogue. Select a model to see its
   classification and file details.

```image
{"id":"model-list","src":"assets/models.png","alt":"The CyModelHub catalogue with the model list and the selected model’s details","controls":"minimal"}
```

## Choose what to do next {#next}

- [Find and register installed models](doc:library#locations)
- [Organize, move and share models](doc:organize#classification)
- [Download a model](doc:install#download)
- [Connect scripts, clients and assistants](doc:integrate#cli)
- [Troubleshoot the catalogue or service](doc:troubleshoot#missing)
