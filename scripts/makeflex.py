#!/usr/bin/env python3

'''Given the original (full) rigid receptor and the output of --out_flex,
   which contains side chain coordinates from flexible docking with smina/gnina,
   output the full restored receptors with the flexible residues re-inserted.
   '''
   
import prody, argparse
from collections import defaultdict

parser = argparse.ArgumentParser(description='Assemble full receptor from flexible docking results.')
parser.add_argument('rigid',type=str,help='Rigid receptor (pdb)')
parser.add_argument('flex',type=str,help='Flexible sidechains from docking (pdb)')
parser.add_argument('out',type=str,help='Output file name (pdb)')
args = parser.parse_args()

rigidname = args.rigid
flexname = args.flex
outfile = args.out

out = open(outfile,'w')

flex = prody.parsePDB(flexname)
flexres = set(zip(flex.getChids(),flex.getResnums()))
backbone = {'CA','N','C','O','H','HN'}
PDBLINE = '%s%-4s%s%8.3f%8.3f%8.3f\n'

for ci in range(flex.numCoordsets()):
    which = defaultdict(int)
    out.write('MODEL %d\n'%ci)
    for line in open(rigidname):
        if line.startswith('ATOM'):
            chain = line[21]
            resnum = int(line[22:26].strip())
            aname = line[12:16].strip()
        if line.startswith('ATOM') and (chain,resnum) in flexres and aname not in backbone and not aname.startswith('H'):
            resatoms = flex[chain].select('resnum %d'%resnum)
            w = which[(chain,resnum)]
            which[(chain,resnum)] += 1 #update to next index
            atom = resatoms[w] #this is the atom to replace this line with
            c = atom.getCoordsets(ci)
            out.write(PDBLINE%(line[:13],atom.getName(),line[17:30],c[0],c[1],c[2]))
        else:
            out.write(line)
    out.write('ENDMDL\n')
