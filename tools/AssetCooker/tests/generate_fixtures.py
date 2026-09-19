"""Generate original, deterministic glTF fixtures; no third-party model license needed."""
import base64
import copy
import json
import math
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[3]
GAME = ROOT / "games/GltfShowcase"
OUT = GAME / "assets/models"
OUT.mkdir(parents=True, exist_ok=True)

def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))

pixels = bytearray()
for y in range(8):
    pixels.append(0)
    for x in range(8):
        pixels.extend(([240, 70, 35, 255] if (x // 2 + y // 2) % 2 else [30, 180, 235, 255]))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', 8, 8, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')
(OUT / 'checker.png').write_bytes(png)
binary = bytearray()
views, accessors = [], []

def view(data):
    binary.extend(b'\0' * ((-len(binary)) % 4))
    i = len(views)
    views.append(dict(buffer=0, byteOffset=len(binary), byteLength=len(data)))
    binary.extend(data)
    return i

def accessor(values, shape, component=5126, bounds=False):
    flat = [v for row in values for v in row] if isinstance(values[0], (list, tuple)) else values
    fmt = {5126: 'f', 5123: 'H', 5121: 'B'}[component]
    record = dict(bufferView=view(struct.pack('<' + fmt * len(flat), *flat)), componentType=component, count=len(values), type=shape)
    if bounds:
        rows = values if isinstance(values[0], (list, tuple)) else [[v] for v in values]
        record['min'] = [min(c) for c in zip(*rows)]
        record['max'] = [max(c) for c in zip(*rows)]
    accessors.append(record)
    return len(accessors) - 1

pos = accessor([[-.5, y, 0] if x == 0 else [.5, y, 0] for y in range(3) for x in range(2)], 'VEC3', bounds=True)
normal = accessor([[0, 0, 1]] * 6, 'VEC3')
uv = accessor([[x, y / 2] for y in range(3) for x in range(2)], 'VEC2')
joints = accessor([[0, 1, 0, 0]] * 6, 'VEC4', 5121)
weights = accessor([[1-y/2, y/2, 0, 0] for y in range(3) for _ in range(2)], 'VEC4')
indices = accessor([0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5], 'SCALAR', 5123)
morph = accessor([[0, 0, 0], [0, 0, 0], [-.2, 0, .15], [.2, 0, .15], [-.5, 0, .3], [.5, 0, .3]], 'VEC3', bounds=True)
identity = [1 if i % 5 == 0 else 0 for i in range(16)]
inverse = identity.copy(); inverse[13] = -1
ibm = accessor([identity, inverse], 'MAT4')
times = accessor([0, 1, 2], 'SCALAR', bounds=True)
rotation = accessor([[0, 0, math.sin(a/2), math.cos(a/2)] for a in [0, .65, 0]], 'VEC4')
animated_weights = accessor([0, 1, 0], 'SCALAR')
translation = accessor([[0, 0, 0], [.5, 0, 0], [0, 0, 0]], 'VEC3')
gltf = dict(asset=dict(version='2.0', generator='Mjolnir deterministic fixture generator'), scene=0,
    scenes=[dict(name='Showcase', nodes=[0, 1])],
    nodes=[dict(name='Flag', mesh=0, skin=0), dict(name='Hip', children=[2]), dict(name='Tip', translation=[0, 1, 0])],
    meshes=[dict(name='FlagGeometry', weights=[0], primitives=[dict(attributes=dict(POSITION=pos, NORMAL=normal, TEXCOORD_0=uv, JOINTS_0=joints, WEIGHTS_0=weights), indices=indices, material=0, targets=[dict(POSITION=morph)])])],
    skins=[dict(name='FlagRig', joints=[1, 2], inverseBindMatrices=ibm, skeleton=1)],
    materials=[dict(name='CheckerMaterial', doubleSided=True, pbrMetallicRoughness=dict(baseColorTexture=dict(index=0), metallicFactor=0, roughnessFactor=1))],
    images=[dict(name='CheckerImage', uri='checker.png')], textures=[dict(source=0, sampler=0)],
    samplers=[dict(magFilter=9728, minFilter=9984, wrapS=10497, wrapT=10497)],
    animations=[dict(name='SwayAndMorph', samplers=[dict(input=times, output=rotation, interpolation='LINEAR'), dict(input=times, output=animated_weights, interpolation='LINEAR')], channels=[dict(sampler=0, target=dict(node=2, path='rotation')), dict(sampler=1, target=dict(node=0, path='weights'))]),
                dict(name='StepTranslate', samplers=[dict(input=times, output=translation, interpolation='STEP')], channels=[dict(sampler=0, target=dict(node=0, path='translation'))])],
    buffers=[dict(uri='flag.bin', byteLength=len(binary))], bufferViews=views, accessors=accessors)
(OUT / 'flag.bin').write_bytes(binary)
(OUT / 'flag.gltf').write_text(json.dumps(gltf, indent=2) + '\n', encoding='utf-8')
embedded = copy.deepcopy(gltf)
embedded['images'] = [dict(name='CheckerImage', bufferView=len(embedded['bufferViews']), mimeType='image/png')]
binary.extend(b'\0' * ((-len(binary)) % 4))
embedded['bufferViews'].append(dict(buffer=0, byteOffset=len(binary), byteLength=len(png)))
binary.extend(png)
embedded['buffers'] = [dict(byteLength=len(binary))]
text = json.dumps(embedded, separators=(',', ':')).encode(); text += b' ' * ((-len(text)) % 4)
binary.extend(b'\0' * ((-len(binary)) % 4))
glb = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)) + struct.pack('<II', len(text), 0x4E4F534A) + text + struct.pack('<II', len(binary), 0x004E4942) + binary
(OUT / 'flag.glb').write_bytes(glb)

def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')

write_json(GAME / 'game.json', dict(name='GltfShowcase', title='Mjolnir - glTF animation and morph', assetsRoot='assets', sceneDir='scenes/default'))
write_json(GAME / 'project_settings.json', dict(actions=[], rendering=dict(rayTracing=False)))
write_json(GAME / 'scenes/default/scene.json', dict(name='glTF showcase', entities=[1, 2, 3], nextId=4))
def entity(i, position, extra):
    write_json(GAME / f'scenes/default/entities/{i}.json', dict(id=i, parent=0, components=[dict(type='Transform', fields=dict(position=position, rotation=[0, 0, 0, 1], scale=[1, 1, 1])), dict(type='WorldTransform', fields=dict(matrix=identity)), extra]))
entity(1, [-1, 0, 0], dict(type='ModelPlayer', fields=dict(modelName='models/flag.glb', clip=0, preview=True, speed=1)))
entity(2, [1, 0, 0], dict(type='ModelPlayer', fields=dict(modelName='models/flag.gltf', clip=0, preview=True, time=1, speed=.6)))
entity(3, [0, 1, 6], dict(type='Camera', fields=dict(fovYDegrees=45, nearZ=.1, farZ=100)))
print('Generated glTF/GLB fixtures and GltfShowcase game.')
