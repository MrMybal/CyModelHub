<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Build the CyModelHub manual

The source pages live in `manual/en/` and `manual/fr/`. Change the English page first and
increase its `revision` for content changes. Update the French text and its `sourceRevision`
in the same change. Keep page IDs, explicit heading anchors and media IDs shared.

Build the official [CyDoc checkout](https://github.com/MrMybal/CyDoc) with Node.js 22 or newer.
This export uses CyDoc 0.1.0 at commit `93f64486d61dae27a94ae1e5a05a20f6edd99914`.
From that checkout, run `npm ci` and `npm pack --pack-destination <temporary-folder>`.
Then run these commands from the CyModelHub root, substituting the generated archive path.

```sh
npm install --prefix .CyDocBuild --no-save --package-lock=false <path-to-cydoc-0.1.0.tgz>
node .CyDocBuild/build.mjs
```

You can also pass an already built CyDoc checkout as the first argument to
`build.mjs`. The script stages the four tracked screenshots from `docs/screenshots/` and
exports the reader and CyDoc license to `Documentation/`. To run CyDoc's `check` command,
pass the CyDoc directory followed by `check`. Both commands write the export.

Open `Documentation/Documentation.html` from disk, verify both languages and include the
new export together with any changes to the sources or screenshots.
