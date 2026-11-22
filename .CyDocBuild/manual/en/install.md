---
id: install
title: Download a model
description: Download from Hugging Face or a direct HTTPS URL and follow the job in Activity.
section: Catalogue
parent: library
order: 30
revision: 1
# SPDX-License-Identifier: GPL-3.0-only
---

## Start a download {#download}

1. Choose **Install…**.
2. For **Hugging Face**, enter the repository as `owner/name` and the exact file path in
   that repository. If access is gated, accept the repository terms in your Hugging Face
   account and provide an authorized Hugging Face token.
3. For **Address**, enter the direct HTTPS URL. Supply the publisher's SHA-256 when
   available so CyModelHub can check the downloaded file.
4. Choose the destination in a [registered location](doc:library#locations), then choose
   **Download**. The CyModelHub store is available as a destination.

## Follow progress {#progress}

Open **Activity** to see progress and failures. Use its cancel control to stop a running
download. CyModelHub can resume when a partial download is available and the server supports
it. Successful downloads are added to the catalogue. Select the new entry to copy its path
and use it with your model runtime.
