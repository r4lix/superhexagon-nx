#!/bin/bash
# Build superhexagon-nx.nro (run through devkitPro's msys bash).
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM DEVKITA64=/opt/devkitpro/devkitA64
export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH
cd "$(dirname "$0")/.." && make 2>&1 | grep -E "error|Error|undefined|built|\.nro|warning: unused" | head -${LINES_MAX:-60}
