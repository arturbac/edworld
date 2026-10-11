"""Run EDVR's tools/gen_exports.py (MIT, copied beside this file, unchanged) with the Windows d3d11 export names
from a text list (tools/d3d11_exports.txt) instead of a dll: there is no Windows d3d11.dll on Linux, and the
build needs no other binary."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_exports as g


class NameList:
    """stands in for gen_exports.PeFile: the source's bytes are the list, one name a line, # a comment"""
    def __init__(self, data: bytes):
        lines = (line.split("#", 1)[0].strip() for line in data.decode("ascii").splitlines())
        self.names = [n for n in lines if n]
        if len(set(self.names)) != len(self.names):
            raise g.PeError("a name listed twice")

    def exports(self):
        return "d3d11.dll", [(n, 1 + i, False) for i, n in enumerate(self.names)]


g.PeFile = NameList
sys.exit(g.main(sys.argv[1:]))
