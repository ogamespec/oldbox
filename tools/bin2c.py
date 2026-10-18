#!/usr/bin/env python3
import sys
src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
with open(dst, 'w') as f:
    f.write('// Generated from ' + src + ' -- do not edit.\n')
    f.write('#pragma once\n#include <cstddef>\n#include <cstdint>\n')
    f.write('namespace oldbox { inline constexpr std::uint8_t ' + name + '[] = {\n')
    for i in range(0, len(data), 16):
        f.write('  ' + ','.join('0x%02x' % b for b in data[i:i+16]) + ',\n')
    f.write('}; inline constexpr std::size_t ' + name + '_size = sizeof(' + name + '); }\n')
