---
id: troubleshoot
title: Troubleshoot CyModelHub
description: Recover missing catalogue entries and diagnose local service and download failures.
section: Help
parent: overview
order: 50
revision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## A model is missing {#missing}

Check that the drive is mounted and the folder is present and enabled in **Locations**.
Run a scan with **F5** and clear any search terms or filters. For a manual entry whose
file has moved, add the new path and forget the stale entry. For a watched folder, a
scan can rediscover the model at its new location.

## The application cannot reach the service {#service}

Run `CyModelHubCli status` from the installation directory. Check that
`CyModelHubService` is next to the client program or set `CYMODELHUB_SERVICE` to the service
executable. If using `--data`, pass the same folder to the app and the CLI. You can run
`CyModelHubCli stop` to stop the instance for that data folder, then reopen the app and
let it start the service again.

## A download fails {#download-error}

Open **Activity** to read the job error. Check the repository file path or direct HTTPS
address, available disk space, and the destination's registration and write permissions.
For gated repositories, check your access rights on Hugging Face. If a checksum fails,
compare the supplied SHA-256 with the publisher's value before trying again.

## The offline manual does not open {#offline}

Keep `Documentation.html` and the complete `DocData` directory in the same
`Documentation` folder. Open the HTML file in a browser from this local folder.
