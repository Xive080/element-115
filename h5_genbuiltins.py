#!/usr/bin/env python3
# H5a0: lista de NOMBRES de builtins del servidor (funciones y metodos) sacada de las tablas reales del motor
# (game_mp/g_scr_main_mp.cpp functions/methods_3, tablas por clase de game/, game_mp/g_client_script_cmd_mp.cpp y los
# carriles SP de game_sp/). Sirve para la pasada de solo-compilacion de los GSC reales: un nombre que no esta aqui es
# una "unknown function" igual que en el juego. El tipo (0 normal / 1 developer) se conserva.
# uso: h5_genbuiltins.py <zdec/src> <salida.inc>
import os, re, sys

src, out = sys.argv[1], sys.argv[2]
FILES = [
    "game_mp/g_scr_main_mp.cpp", "game/actor_script_cmd.cpp", "game/g_scr_mover.cpp", "game/g_scr_helicopter.cpp",
    "game/g_hudelem.cpp", "game/g_scr_vehicle.cpp", "game_mp/g_client_script_cmd_mp.cpp",
    "game_sp/g_scr_sp_players.cpp", "game_sp/g_scr_sp_anim.cpp", "game_sp/g_scr_sp_entity.cpp",
    "game_sp/g_scr_sp_ai.cpp", "game_sp/g_scr_sp_notported.cpp",
]
head = re.compile(r'\b(BuiltinFunctionDef|BuiltinMethodDef)\s+\w+\s*\[\s*\d*\s*\]\s*=')
row = re.compile(r'\{\s*"([^"]+)"\s*,\s*&?\s*\w+\s*,\s*(\d+)\s*\}')
funcs, meths = {}, {}
for rel in FILES:
    kind = None
    with open(os.path.join(src, rel), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = head.search(line)
            if m:
                kind = funcs if m.group(1) == "BuiltinFunctionDef" else meths
                continue
            if kind is None:
                continue
            if line.strip().startswith("};"):
                kind = None
                continue
            for n, t in row.findall(line):
                kind.setdefault(n, int(t))   # primera aparicion gana (mismo orden que la busqueda del motor)
with open(out, "w") as o:
    o.write("// GENERADO por h5_genbuiltins.py desde las tablas de builtins del motor (no editar)\n")
    for n in sorted(funcs): o.write('H5_F("%s", %d)\n' % (n, funcs[n]))
    for n in sorted(meths): o.write('H5_M("%s", %d)\n' % (n, meths[n]))
print("h5_builtins: %d funciones, %d metodos" % (len(funcs), len(meths)))
