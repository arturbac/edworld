"""Run EDVR's tools/gen_exports.py (MIT, copied beside this file) with the Windows d3d11 export list taken
from a released EDVR d3d11.dll: there is no Windows d3d11.dll on Linux, and EDVR's release exports exactly
the system list plus its own edvr*/ffx* names, which are filtered back out here."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_exports as g

orig = g.PeFile.exports
def filtered(self):
    dll, exports = orig(self)
    keep = [(n, o, f) for (n, o, f) in exports
            if n and not n.startswith(("edvr", "ffx", "GetFfx"))]
    return "d3d11.dll", keep
g.PeFile.exports = filtered
sys.exit(g.main(sys.argv[1:]))
