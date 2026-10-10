"""Generate editable, flat printable concept parts. Units: mm.

Dependencies: trimesh, shapely, mapbox-earcut. No CAD application needed.
"""
import json
from pathlib import Path
import trimesh
from shapely.geometry import Point, box, LineString
from shapely.ops import unary_union

ROOT = Path(__file__).resolve().parent
p = json.loads((ROOT / 'params.json').read_text(encoding='utf-8'))
OUT = ROOT / 'stl-draft'
OUT.mkdir(exist_ok=True)
W = p['panel_width'] - 2*p['frame_inset']
H = p['panel_height'] - 2*p['frame_inset']
r = p['rail_width']
assert W > 2*r and H > 2*r and r >= 20
assert p['frame_thickness'] > 0 and p['panel_gap'] >= 0

def circle(x, y, diameter):
    return Point(x, y).buffer(diameter/2, quad_segs=24)

def slot(x, y, diameter, travel):
    return LineString([(x-travel/2, y), (x+travel/2, y)]).buffer(diameter/2, quad_segs=24)

report = {'units': 'mm', 'dimensions_verified': p['dimensions_verified'], 'parts': {}}
def save(name, shape, height):
    assert shape.is_valid and shape.geom_type == 'Polygon', name
    mesh = trimesh.creation.extrude_polygon(shape, height, engine='earcut')
    assert mesh.is_watertight and mesh.is_winding_consistent and mesh.volume > 0, name
    mesh.apply_translation([-mesh.bounds[0][0], -mesh.bounds[0][1], 0])
    path = OUT / (name + '.stl')
    mesh.export(path)
    loaded = trimesh.load_mesh(path)
    assert loaded.is_watertight and loaded.is_winding_consistent, name
    report['parts'][name] = {'size_mm': mesh.extents.round(3).tolist(), 'watertight': True,
                             'volume_mm3': round(mesh.volume, 1), 'triangles': len(mesh.faces)}
    return shape

# Open frame with a central rail; two halves fit a smaller print bed.
frame = box(-W/2, -H/2, W/2, H/2).difference(box(-W/2+r, -H/2+r, W/2-r, H/2-r))
frame = frame.union(box(-r/2, -H/2, r/2, H/2))
# Board platform connected to right rail and central rail. Hole pitches are placeholders.
bx = W/4
board_width = max(76, p['board_hole_pitch_x'] + p['board_slot_travel'] + 14)
board_height = max(68, p['board_hole_pitch_y'] + 14)
assert bx-board_width/2 >= r/2 and bx+board_width/2 <= W/2-r
assert board_height < H-2*r
frame = unary_union([frame, box(r/2, -6, W/2, 6),
                     box(bx-board_width/2, -board_height/2, bx+board_width/2, board_height/2)])
holes = []
for x in [-p['panel_hole_pitch_x']/2, p['panel_hole_pitch_x']/2]:
    for y in [-p['panel_hole_pitch_y']/2, p['panel_hole_pitch_y']/2]:
        hole = circle(x, y, p['panel_clearance_diameter'])
        assert frame.contains(hole.buffer(2)), 'Panel hole is outside supported rail; change frame/rail parameters'
        holes.append(hole)
# Outer joining points and inner split-splice points.
outer_x = W/2-r/2
outer_y = H/2-r/2
for x in [-outer_x, outer_x]:
    for y in [-H/3, H/3]:
        holes.append(circle(x, y, p['joining_clearance_diameter']))
for y in [-outer_y, outer_y]:
    for x in [-W/3, W/3, -22, 22]:
        holes.append(circle(x, y, p['joining_clearance_diameter']))
for x in [-p['board_hole_pitch_x']/2, p['board_hole_pitch_x']/2]:
    for y in [-p['board_hole_pitch_y']/2, p['board_hole_pitch_y']/2]:
        holes.append(slot(bx+x, y, p['board_clearance_diameter'], p['board_slot_travel']))
# Cable tie slots on the board platform, away from PCB mounting holes.
for x in [bx-18, bx+18]:
    holes.append(box(x-3, -2, x+3, 2))
frame = frame.difference(unary_union(holes))
left = save('carrier-left-DRAFT', frame.intersection(box(-W, -H, 0, H)), p['frame_thickness'])
right = save('carrier-right-DRAFT', frame.intersection(box(0, -H, W, H)), p['frame_thickness'])

def strap(name, pitch):
    # Round locating hole at one end, adjustment slot at the other.
    shape = LineString([(0, 0), (pitch, 0)]).buffer(10, quad_segs=24)
    shape = shape.difference(circle(0, 0, p['joining_clearance_diameter']))
    shape = shape.difference(slot(pitch, 0, p['joining_clearance_diameter'], 4))
    return save(name, shape, 6)

strap('split-splice-DRAFT', 44)
strap('panel-link-DRAFT', p['panel_width']-2*outer_x+p['panel_gap'])
assert abs((p['panel_width']-2*outer_x)-(p['panel_height']-2*outer_y)) < 0.01
save('board-spacer-DRAFT', circle(0, 0, 8).difference(circle(0, 0, p['board_clearance_diameter'])), p['board_spacer_height'])
save('panel-spacer-DRAFT', circle(0, 0, 10).difference(circle(0, 0, p['panel_clearance_diameter'])), p['panel_spacer_height'])

# Dimension-matched 2D preview showing the actual model's cutouts, not generated concept art.
def svg_path(poly):
    rings = [poly.exterior, *poly.interiors]
    return ' '.join('M '+' L '.join(f'{x:.3f},{-y:.3f}' for x,y in ring.coords)+' Z' for ring in rings)
svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="{-W/2-10} {-H/2-25} {W+20} {H+40}">
<rect x="{-W/2-10}" y="{-H/2-25}" width="{W+20}" height="{H+40}" fill="white"/>
<text x="{-W/2}" y="{-H/2-10}" font-size="7">DRAFT / mm / hole positions unverified</text>
<path d="{svg_path(left)}" fill="#c6dbea" stroke="#245779" stroke-width="0.5" fill-rule="evenodd"/>
<path d="{svg_path(right)}" fill="#dfd4b8" stroke="#78643c" stroke-width="0.5" fill-rule="evenodd"/>
</svg>'''
(ROOT/'preview.svg').write_text(svg, encoding='utf-8')
(OUT/'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
