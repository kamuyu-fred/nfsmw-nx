# -*- coding: utf-8 -*-
# Rebuilds the address table of another edition from the two PGO profiles, without the Spanish PAL executable.
#
# tools/editions/pgo/traducir_perfil.py made pgo/<edition> from pgo/pal_es by renaming every function record
# through the address table of tools/editions/emparejar.py: the records keep their order and their control
# flow checksum, and only the name hash changes. That hash is GCC's crc32 of the assembler name
# (__imp__sub_XXXXXXXX for recompiled functions, sub_XXXXXXXX for hooks), and there are only ~3.4 million
# possible addresses, so it can be inverted by trying them all. Pairing the records of both profiles file by
# file gives back the pairs of that table for every function the profile has: the same pairs the released
# NRO of that edition was built with.
#
# What a profile cannot give: addresses that are not the start of a function. For those:
#   - inside a paired function: the same offset from its start (paired functions are the same code);
#   - .data (not in Japan): the same address, as emparejar.py does for PAL and USA. Its bounds come from the
#     other edition's image, which also tells .rdata addresses apart: those move, and are left for a manual look.
# .rdata pairs found by hand go in tools/editions/<edition>/rdata.json, each with its evidence. Anything else
# is listed for a manual look: crear_arbol.py refuses a tree with addresses it cannot translate.
#
# It also writes the Spanish function partition (generated/default/codegen.partition.json), read from the
# file names and record order of pgo/pal_es, which crear_arbol.py seeds the edition's partition with.
#
# Usage: mapa_desde_pgo.py <edition> <other image> <output folder>
#   <other image>: the decompressed executable (the PE image inside default.xex; see docs/editions.md)
#   writes <output>/tabla.tsv (emparejar.py's format), <output>/codegen.partition.json (Spanish partition)
#   and prints the addresses of app/ that have no translation.
import bisect
import glob
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pgo'))
import gcda  # noqa: E402

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(RAIZ, 'app')
PAT = re.compile(r'(?<![0-9A-Fa-f])82[0-9A-Fa-f]{6}(?![0-9A-Fa-f])')
RECOMP = re.compile(r'#generated#default#nfsmw_recomp\.(\d+)\.cpp\.gcda$')
INICIO_TEXTO, FIN_IMAGEN = 0x82000000, 0x82CD0000
# How far into a function an address may be to be translated by offset. The longest hooked function the app
# points inside of is well under this; a larger offset is more likely another function the profile lacks.
MAX_DESPLAZAMIENTO = 0x4000
# Values the code uses as bounds of the image, not as addresses: the same in every edition.
LIMITES = {0x82000000, 0x82CD0000, 0x83000000}


def tabla_crc():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) & 0xFFFFFFFF) ^ (0x04C11DB7 if c & 0x80000000 else 0)
        t.append(c)
    return t


def inversa_de_nombres():
    """{prefix: {ident: [addresses]}} for __imp__sub_XXXXXXXX and sub_XXXXXXXX (gcda.ident_publica).

    The hash has 31 bits and each prefix has ~3.4 million candidates, so about 0.2 % of the hashes match more
    than one address: every candidate is kept, and parejas_de_funciones() picks one.
    """
    t = tabla_crc()

    def actualizar(crc, datos):
        for b in datos:
            crc = ((crc << 8) & 0xFFFFFFFF) ^ t[((crc >> 24) ^ b) & 0xFF]
        return crc

    inversas = {}
    for prefijo in (b'__imp__sub_', b'sub_'):
        inversa = {}
        base = actualizar(0, prefijo)
        for alta in range(INICIO_TEXTO >> 16, FIN_IMAGEN >> 16):
            c = actualizar(base, b'%04X' % alta)
            for baja in range(0, 0x10000, 4):
                ident = actualizar(c, b'%04X' % baja + b'\0') & 0x7FFFFFFF
                d = (alta << 16) | baja
                previo = inversa.get(ident or 1)
                if previo is None:
                    inversa[ident or 1] = d
                elif isinstance(previo, list):
                    previo.append(d)
                else:
                    inversa[ident or 1] = [previo, d]
        inversas[prefijo.decode()] = inversa
    return inversas


def candidatos(inversa, ident):
    c = inversa.get(ident)
    return [] if c is None else c if isinstance(c, list) else [c]


def parejas_de_funciones(edicion, inversas, es_codigo):
    """PAL -> other pairs, and the Spanish partition {address: file index}.

    Recompiled functions (generated files) are named __imp__sub_, hooks (app files) sub_. When a hash has more
    than one candidate, the pair kept is the one whose distance (other - PAL) matches the nearest unambiguous
    pair of the same file: editions move code in long runs with the same distance.
    """
    mapa, reparto, dudosas = {}, {}, []
    for ruta_pal in sorted(glob.glob(os.path.join(RAIZ, 'pgo', 'pal_es', '*.gcda'))):
        nombre = os.path.basename(ruta_pal)
        ruta_otra = os.path.join(RAIZ, 'pgo', edicion, nombre)
        m = RECOMP.search(nombre)
        inversa = inversas['__imp__sub_' if '#generated#' in nombre else 'sub_']
        pal = gcda.funciones(gcda.leer(ruta_pal)[1])
        if not os.path.exists(ruta_otra):
            continue
        otra = gcda.funciones(gcda.leer(ruta_otra)[1])
        # traducir_perfil.py only removes records (functions without a counterpart) and never reorders them:
        # walk both lists together, matching by control flow checksum.
        filas = []
        j = 0
        for _, ident, _, cfg, _ in pal:
            k = j
            while k < len(otra) and otra[k][3] != cfg:
                k += 1
            if k == len(otra):
                filas.append((candidatos(inversa, ident), None))
                continue
            j = k + 1
            filas.append((candidatos(inversa, ident),
                          [d for d in candidatos(inversa, otra[k][1]) if es_codigo(d)]))
        seguras = [(a[0], b[0]) for a, b in filas if b and len(a) == 1 and len(b) == 1]
        for a, b in filas:
            if not a:
                continue
            if b is None:
                # no counterpart in the other edition: only the partition needs it
                if len(a) == 1 and m:
                    reparto[a[0]] = int(m.group(1))
                continue
            if not b:
                continue
            if len(a) == 1 and len(b) == 1:
                par = (a[0], b[0])
            else:
                mejor = None
                for x in a:
                    for y in b:
                        cerca = min(seguras, key=lambda s: abs(s[0] - x), default=None)
                        if cerca is None:
                            continue
                        error = abs((y - x) - (cerca[1] - cerca[0]))
                        if mejor is None or error < mejor[0]:
                            mejor = (error, (x, y))
                if mejor is None or mejor[0] > 0x100:
                    dudosas.append((nombre, a, b))
                    continue
                par = mejor[1]
            mapa[par[0]] = par[1]
            if m:
                reparto[par[0]] = int(m.group(1))
    return mapa, reparto, dudosas


def secciones(ruta):
    img = open(ruta, 'rb').read()
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    n = struct.unpack_from('<H', img, pe + 6)[0]
    opt = struct.unpack_from('<H', img, pe + 20)[0]
    out = []
    for i in range(n):
        at = pe + 24 + opt + i * 40
        nombre = img[at:at + 8].rstrip(b'\0').decode()
        tam, va = struct.unpack_from('<II', img, at + 8)
        codigo = bool(struct.unpack_from('<I', img, at + 36)[0] & 0x20000000)  # IMAGE_SCN_MEM_EXECUTE
        out.append((nombre, INICIO_TEXTO + va, INICIO_TEXTO + va + tam, codigo))
    return out


def seccion_de(sec, d):
    return next((s for s in sec if s[1] <= d < s[2]), None)


def direcciones_de_app():
    usadas = {}
    rutas = glob.glob(os.path.join(APP, 'src', '**', '*.*'), recursive=True)
    rutas += [os.path.join(APP, f) for f in ('overrides.toml', 'huecos.toml', 'orden_funciones.ld',
                                             'nfsmw.toml', 'CMakeLists.txt')]
    for ruta in rutas:
        if not os.path.isfile(ruta):
            continue
        for m in PAT.finditer(open(ruta, encoding='utf-8', errors='ignore').read()):
            usadas.setdefault(int(m.group(), 16), os.path.relpath(ruta, RAIZ))
    return usadas


def main():
    edicion, imagen, salida = sys.argv[1:4]
    os.makedirs(salida, exist_ok=True)
    sec = secciones(imagen)

    def es_codigo(d):
        s = seccion_de(sec, d)
        return s is not None and s[3]

    def es_data(d):
        s = seccion_de(sec, d)
        return s is not None and s[0] == '.data'
    print('inverting the name hashes (~40 s)...')
    inversas = inversa_de_nombres()
    mapa, reparto, dudosas = parejas_de_funciones(edicion, inversas, es_codigo)
    print('function pairs: %d; Spanish partition: %d functions; left out as ambiguous: %d'
          % (len(mapa), len(reparto), len(dudosas)))

    filas = {a: (b, 'exacta (perfil)') for a, b in mapa.items()}
    manual = os.path.join(RAIZ, 'tools', 'editions', edicion, 'rdata.json')
    if os.path.exists(manual):
        for a, b in json.load(open(manual, encoding='utf-8'))['parejas'].items():
            filas[int(a, 16)] = (int(b, 16), 'referencias (tools/editions/%s/rdata.json)' % edicion)
    inicios = sorted(mapa)
    sin_traducir = []
    for d, fichero in sorted(direcciones_de_app().items()):
        if d in filas:
            continue
        i = bisect.bisect_right(inicios, d) - 1
        if d in LIMITES:
            filas[d] = (d, 'constante (limite de la imagen, no una direccion)')
        elif i >= 0 and d - inicios[i] < MAX_DESPLAZAMIENTO and es_codigo(d):
            base = inicios[i]
            filas[d] = (mapa[base] + (d - base), 'exacta (desplazamiento +0x%X de %08X)' % (d - base, base))
        elif es_data(d) and edicion != 'jpn':
            filas[d] = (d, 'misma direccion (.data)')
        else:
            sin_traducir.append((d, fichero))

    with open(os.path.join(salida, 'tabla.tsv'), 'w', encoding='utf-8') as f:
        f.write('pal\tseccion\totra\testado\n')
        for a in sorted(filas):
            b, estado = filas[a]
            seccion = (seccion_de(sec, a) or ('-',))[0]
            f.write('%08X\t%s\t%08X\t%s\n' % (a, seccion, b, estado))
    archivos = max(reparto.values()) + 1
    with open(os.path.join(salida, 'codegen.partition.json'), 'w', encoding='utf-8') as f:
        json.dump({'version': 2, 'file_count': archivos, 'max_file_bytes': 1048576,
                   'assignments': {'%08X' % a: n for a, n in sorted(reparto.items())}}, f, indent=2)
        f.write('\n')
    por_estado = {}
    for _, estado in filas.values():
        clave = estado.split(' (')[0] + (' (desplazamiento)' if 'desplazamiento' in estado else '')
        por_estado[clave] = por_estado.get(clave, 0) + 1
    print('table: %d rows %s -> %s' % (len(filas), por_estado, os.path.join(salida, 'tabla.tsv')))
    print('partition: %d files -> %s' % (archivos, os.path.join(salida, 'codegen.partition.json')))
    if sin_traducir:
        print('NO TRANSLATION (%d), first file that mentions each:' % len(sin_traducir))
        for d, fichero in sin_traducir:
            print('  %08X  %s' % (d, fichero))


if __name__ == '__main__':
    main()
