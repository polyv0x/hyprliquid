#!/usr/bin/env python3
"""Embed Liquid material variants as typed constants and read-only API data."""
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent.parent
variants = json.loads((root / 'materials/liquid.json').read_text())['variants']
declarations = []


def typed(value, name):
    if isinstance(value, dict):
        members = [(key, *typed(item, name + key[0].upper() + key[1:])) for key, item in value.items()]
        declarations.append('struct ' + name + ' {\n' + ''.join(
            f'    {kind} {key};\n' for key, kind, _ in members) +
            f'    bool operator==(const {name}&) const = default;\n' + '};\n')
        return name, '{' + ', '.join(init for _, _, init in members) + '}'
    if isinstance(value, list):
        items = [typed(item, name + 'Element') for item in value]
        assert items and all(kind == items[0][0] for kind, _ in items)
        return f'std::array<{items[0][0]}, {len(items)}>', '{{' + ', '.join(init for _, init in items) + '}}'
    if value is None:
        return 'std::optional<double>', 'std::nullopt'
    if isinstance(value, bool):
        return 'bool', str(value).lower()
    assert isinstance(value, (int, float))
    return 'double', repr(float(value))


_, default = typed(variants['default'], 'SLiquidMaterial')
schema = declarations.copy()
declarations.clear()
_, dark = typed(variants['dark'], 'SLiquidMaterial')
assert declarations == schema, 'Material variants must use the same typed schema'
Path(sys.argv[1]).write_text(
    '// Generated from materials/liquid.json by scripts/embed-material.py.\n#pragma once\n'
    '#include <array>\n#include <optional>\n' + ''.join(schema) +
    f'inline constexpr SLiquidMaterial LIQUID_DEFAULT = {default};\n'
    f'inline constexpr SLiquidMaterial LIQUID_DARK = {dark};\n'
    'inline constexpr const char* LIQUID_VARIANTS_JSON = R"MATERIAL(' +
    json.dumps(variants, separators=(',', ':')) + ')MATERIAL";\n')
