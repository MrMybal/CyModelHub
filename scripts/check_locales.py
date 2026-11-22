# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: GPL-3.0-only

"""List the interface strings of the application and check the translation files.

    python scripts/check_locales.py            # report missing and unused entries, exit 1 on missing
    python scripts/check_locales.py --keys     # print every English string, one JSON line each

The English text in the source is the key. A language is one file in locales/:
{"locale": "de", "name": "Deutsch", "translations": {"English text": "Deutscher Text"}}.
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = [ROOT / "src" / "app_ui.cpp"]
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def literal_at(text, position):
    """Parse adjacent C string literals starting at position; return (value, end) or None."""
    parts = []
    while True:
        while position < len(text) and text[position] in " \t\r\n":
            position += 1
        match = LITERAL.match(text, position)
        if not match:
            break
        parts.append(match.group(1))
        position = match.end()
    if not parts:
        return None
    value = "".join(parts).encode("utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
    return value, position


def keys_of(text):
    keys = set()
    for match in re.finditer(r"(?<![.\w])(T|L|invoke|modalButtons|sidebar_choices)\(", text):
        first = literal_at(text, match.end())
        if not first:
            continue
        keys.add(first[0])
        if match.group(1) == "modalButtons":
            rest = text[first[1]:]
            if rest.lstrip().startswith(","):
                second = literal_at(text, first[1] + rest.index(",") + 1)
                if second:
                    keys.add(second[0])
    # Labels of the classification tables: {"value", "Label", icon::name}.
    for match in re.finditer(r'\{"[a-z0-9_]+",\s*"([^"]+)",\s*icon::', text):
        keys.add(match.group(1))
    keys.discard("")
    return keys


def main():
    keys = set()
    for source in SOURCES:
        keys |= keys_of(source.read_text(encoding="utf-8"))
    if "--keys" in sys.argv:
        for key in sorted(keys):
            print(json.dumps(key, ensure_ascii=False))
        return 0
    failed = False
    for file in sorted((ROOT / "locales").glob("*.json")):
        translations = json.loads(file.read_text(encoding="utf-8"))["translations"]
        missing = sorted(keys - translations.keys())
        unused = sorted(translations.keys() - keys)
        print(f"{file.name}: {len(translations)} entries, {len(missing)} missing, {len(unused)} unused")
        for key in missing:
            print("  missing:", json.dumps(key, ensure_ascii=False))
        for key in unused:
            print("  unused: ", json.dumps(key, ensure_ascii=False))
        failed = failed or bool(missing)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
