// SPDX-FileCopyrightText: 2026 Cyberalien
// SPDX-License-Identifier: GPL-3.0-only

import { spawnSync } from 'node:child_process';
import { copyFile, mkdir } from 'node:fs/promises';
import { join, resolve } from 'node:path';

const workspace = import.meta.dirname;
const tool = resolve(process.argv[2] ?? join(workspace, 'node_modules', 'cydoc'));
const command = process.argv[3] ?? 'build';
if (!['build', 'check'].includes(command)) {
  throw new Error('Usage: node .CyDocBuild/build.mjs [CyDoc directory] [build|check]');
}

const assets = join(workspace, 'assets');
await mkdir(assets, { recursive: true });
for (const image of ['models.png', 'models.fr.png', 'duplicates.png', 'duplicates.fr.png']) {
  await copyFile(join(workspace, '..', 'docs', 'screenshots', image), join(assets, image));
}

const result = spawnSync(process.execPath, [join(tool, 'tools', 'cli.mjs'), command,
  join(workspace, 'cydoc.config.json')], { stdio: 'inherit' });
if (result.error) throw result.error;
if (result.status !== 0) process.exit(result.status ?? 1);

await copyFile(join(tool, 'LICENSE'), join(workspace, '..', 'Documentation', 'DocData', 'CyDoc.LICENSE.txt'));
