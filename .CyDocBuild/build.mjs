// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

import { spawnSync } from 'node:child_process';
import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';

const workspace = import.meta.dirname;
const project = join(workspace, '..');
const tool = resolve(process.argv[2] ?? join(workspace, 'node_modules', 'cydoc'));
const command = process.argv[3] ?? 'build';
if (!['build', 'check'].includes(command)) {
  throw new Error('Usage: node .CyDocBuild/build.mjs [CyDoc directory] [build|check]');
}

const assets = join(workspace, 'assets');
await mkdir(assets, { recursive: true });
for (const image of ['models.png', 'models.fr.png', 'duplicates.png', 'duplicates.fr.png']) {
  await copyFile(join(project, 'docs', 'screenshots', image), join(assets, image));
}
const logo = 'cymodelhub-icon.png';
await copyFile(join(project, 'assets', 'branding', logo), join(assets, logo));

const result = spawnSync(process.execPath, [join(tool, 'tools', 'cli.mjs'), command,
  join(workspace, 'cydoc.config.json')], { stdio: 'inherit' });
if (result.error) throw result.error;
if (result.status !== 0) process.exit(result.status ?? 1);

const output = join(project, 'Documentation');
await copyFile(join(tool, 'LICENSE'), join(output, 'DocData', 'CyDoc.LICENSE.txt'));

// CyDoc 0.1.0 uses a fixed browser icon in its HTML template.
const reader = join(output, 'Documentation.html');
const html = await readFile(reader, 'utf8');
await writeFile(reader, html.replace('href="DocData/assets/favicon.svg" type="image/svg+xml"',
  `href="DocData/assets/${logo}" type="image/png"`));
