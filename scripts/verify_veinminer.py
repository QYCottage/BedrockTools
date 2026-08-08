from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
checks = []

def expect(path, needle, label):
    text = (ROOT / path).read_text(encoding='utf-8')
    ok = needle in text
    checks.append((ok, label))
    if not ok:
        print(f'[FAIL] {label}: missing {needle!r} in {path}')
    else:
        print(f'[ OK ] {label}')

expect('include/bedrocktools/Version.hpp', 'Version = "1.5.0"', 'BedrockTools version 1.5.0')
expect('src/modules/ModuleRegistry.cpp', '#include "player/veinminer.hpp"', 'Vein Miner header registered')
expect('src/modules/ModuleRegistry.cpp', 'registry.emplace<VeinMinerModule>();', 'Vein Miner instantiated')
expect('include/bedrocktools/memory/Signatures.hpp', 'SurvivalModeDestroyBlock,', 'Destroy-block SignatureId declared')
expect('src/core/memory/Signatures.cpp', 'SignatureId::SurvivalModeDestroyBlock', 'Destroy-block signature registered')
expect('include/bedrocktools/sdk/offsets/World.hpp', 'namespace GameMode {', 'GameMode offsets namespace present')
expect('include/bedrocktools/sdk/offsets/World.hpp', 'mPlayer = 0x8', 'GameMode::mPlayer offset present')
expect('include/bedrocktools/sdk/offsets/World.hpp', 'mDestroyProgress = 0x24', 'GameMode::mDestroyProgress offset present')
expect('src/modules/player/veinminer.cpp', 'Module("Vein Miner"', 'Vein Miner module source present')
expect('src/modules/player/veinminer.cpp', 'SignatureId::BlockSourceGetBlock', 'Vein Miner block lookup wired')
expect('src/modules/player/veinminer.cpp', 'SignatureId::SurvivalModeDestroyBlock', 'Vein Miner destroy hook wired')
expect('src/modules/player/veinminer.cpp', '",Ores,Logs,Same Block"', 'Vein Miner radio menu config present')
expect('src/modules/player/veinminer.cpp', 'progress = 1.0f;', 'Queued survival break progress completion present')

xmake = (ROOT / 'xmake.lua').read_text(encoding='utf-8')
if 'src/modules/**.cpp' in xmake:
    print('[ OK ] xmake includes src/modules/**.cpp')
    checks.append((True, 'xmake module glob'))
else:
    print('[FAIL] xmake does not include src/modules/**.cpp')
    checks.append((False, 'xmake module glob'))

failed = [label for ok, label in checks if not ok]
if failed:
    print('\nVerification failed: ' + ', '.join(failed))
    sys.exit(1)
print(f'\nVein Miner integration checks passed ({len(checks)}/{len(checks)}).')
