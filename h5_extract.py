#!/usr/bin/env python3
# H5a: extrae una funcion del motor tal cual (por llaves) para compilarla en el harness sin el resto de su TU.
# uso: h5_extract.py <fichero.cpp> <firma exacta de la primera linea[||otra firma...]> <salida.inc> [globales...]
# Copia tambien los #include del fichero y la definicion de nivel superior (una linea con ';') de cada global nombrado.
import sys
noinc = '--noinc' in sys.argv; argv = [a for a in sys.argv if a != '--noinc']   # --noinc: sin los #include del fichero
src, sig, out = argv[1], argv[2], argv[3]; names = argv[4:]
L = open(src, encoding='utf-8', errors='replace').read().split('\n')
sigs = sig.split('||')   # varias funciones: "firma1||firma2"
bodies = []
for sg in sigs:
    i = next(k for k, l in enumerate(L) if l.strip() == sg)
    depth = 0; body = ['#line %d "%s"' % (i + 1, src)]; started = False
    for l in L[i:]:
        body.append(l)
        depth += l.count('{') - l.count('}')
        if '{' in l: started = True
        if started and depth == 0: break
    bodies += body
import os, re
inc = [] if noinc else [l for l in L if l.startswith('#include')]
inc = [l if not re.match(r'#include\s+"', l) else '#include "%s"' % os.path.join(os.path.dirname(os.path.abspath(src)), l.split('"')[1]) for l in inc]
glob = []
for n in names:
    g = next((l for l in L if l and not l[0].isspace() and re.search(r'\b%s\b' % re.escape(n), l) and l.split('//')[0].rstrip().endswith(';') and '(' not in l.split('//')[0].split('=')[0]), None)
    if g is None: sys.exit('no encuentro la global ' + n)
    glob.append(g)
txt = '// GENERADO por h5_extract.py desde %s (no editar)\n' % src + '\n'.join(inc) + '\n' + '\n'.join(glob) + '\n' + '\n'.join(bodies) + '\n'
try:
    if open(out).read() == txt: sys.exit(0)
except FileNotFoundError: pass
open(out, 'w').write(txt)
