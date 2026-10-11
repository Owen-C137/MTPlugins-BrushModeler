"""Merge two BO3 XMODEL_EXPORT v6 files by offsetting their indexed sections."""
import math
import re
import sys
from pathlib import Path


def read(path):
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        return f.read().splitlines()


def sections(lines, allow_empty=False):
    d = {"bones": [], "bone_data": [], "verts": [], "faces": [], "objects": [], "materials": []}
    version = 6
    i = 0
    while i < len(lines):
        s = lines[i].strip()
        t = s.split()
        if len(t) == 2 and t[0] == "VERSION": version = int(t[1])
        if len(t) == 2 and t[0] == "NUMBONES":
            n = int(t[1]); i += 1
            while i < len(lines) and len(d["bones"]) < n:
                if re.match(r'^BONE\s+\d+\s+-?\d+\s+', lines[i].strip()): d["bones"].append(lines[i].strip())
                i += 1
            while i < len(lines) and len(d["bone_data"]) < n:
                if re.match(r'^BONE\s+\d+\s*$', lines[i].strip()):
                    chunk = [lines[i].strip()]; i += 1
                    while i < len(lines) and lines[i].strip() and not re.match(r'^BONE\s+\d+\s*$', lines[i].strip()):
                        chunk.append(lines[i].strip()); i += 1
                    d["bone_data"].append(chunk)
                    continue
                i += 1
            continue
        if len(t) == 2 and t[0] in ("NUMVERTS", "NUMVERTS32"):
            n = int(t[1]); i += 1
            while i < len(lines) and len(d["verts"]) < n:
                if re.match(r'^VERT(?:32)?\s+\d+\s*$', lines[i].strip()):
                    chunk = [lines[i].strip()]; i += 1
                    while i < len(lines) and lines[i].strip() and not re.match(r'^VERT(?:32)?\s+\d+\s*$', lines[i].strip()) and not lines[i].strip().startswith(("NUMFACES", "NUMOBJECTS", "NUMMATERIALS")):
                        chunk.append(lines[i].strip()); i += 1
                    d["verts"].append(chunk); continue
                i += 1
            continue
        if len(t) == 2 and t[0] == "NUMFACES":
            n = int(t[1]); i += 1
            while i < len(lines) and len(d["faces"]) < n:
                if lines[i].strip().startswith("TRI "):
                    chunk = [lines[i].strip()]; i += 1
                    while i < len(lines) and not lines[i].strip().startswith(("TRI ", "NUMOBJECTS", "NUMMATERIALS")):
                        chunk.append(lines[i].strip()); i += 1
                    d["faces"].append(chunk); continue
                i += 1
            continue
        if len(t) == 2 and t[0] == "NUMOBJECTS":
            n = int(t[1]); i += 1
            while i < len(lines) and len(d["objects"]) < n:
                if lines[i].strip().startswith("OBJECT "): d["objects"].append(lines[i].strip())
                i += 1
            continue
        if len(t) == 2 and t[0] == "NUMMATERIALS":
            n = int(t[1]); i += 1
            while i < len(lines) and len(d["materials"]) < n:
                if lines[i].strip().startswith("MATERIAL "):
                    chunk = [lines[i].strip()]; i += 1
                    while i < len(lines) and lines[i].strip() and not lines[i].strip().startswith("MATERIAL "):
                        chunk.append(lines[i].strip()); i += 1
                    d["materials"].append(chunk); continue
                i += 1
            continue
        i += 1
    if not d["bones"] or (not allow_empty and (not d["verts"] or not d["faces"] or not d["objects"] or not d["materials"])):
        raise ValueError("unsupported or incomplete XMODEL_EXPORT sections")
    return d, version


def shifted(doc, bone_at, vert_at, face_at, object_at, material_at, transform=None):
    bones = []
    for line in doc["bones"]:
        m = re.match(r'BONE\s+(\d+)\s+(-?\d+)\s+(.*)', line)
        idx, parent = int(m[1]) + bone_at, int(m[2])
        # XMODEL_EXPORT supports one hierarchy root. Attach each appended model's
        # former root to the combined tag_origin instead of emitting another -1 parent.
        merged_parent = parent + bone_at if parent >= 0 else (0 if bone_at > 0 else -1)
        bones.append(f"BONE {idx} {merged_parent} {m[3]}")
    bone_data = []
    for chunk in doc["bone_data"]:
        out = []
        for line in chunk:
            if line.startswith("BONE "):
                out.append(f"BONE {int(line.split()[1]) + bone_at}")
            elif line.startswith(("OFFSET ", "X ", "Y ", "Z ")) and transform:
                # ExportX stores each joint's bind-pose transform in model space.
                # Transform every joint, not just tag_origin, or a rigged mesh's
                # weighted vertices remain around its unplaced skeleton.
                # ExportX writes vector components comma-separated ("x, y, z"), while
                # BrushModeler writes whitespace-separated values. Accept both formats.
                key, *values = line.split(); x, y, z = map(lambda v: float(v.strip(",")), values[:3]); xx, yy, zz = transform(x, y, z, key == "OFFSET")
                out.append(f"{key} {xx:.6f} {yy:.6f} {zz:.6f}")
            else: out.append(line)
        bone_data.append(out)
    verts = []
    for chunk in doc["verts"]:
        out = []
        for line in chunk:
            if line.startswith("VERT"):
                tok, idx = line.split()[:2]; out.append(f"{tok} {int(idx) + vert_at}")
            elif line.startswith("BONE "):
                tok, idx, *rest = line.split(); out.append(f"BONE {int(idx) + bone_at} {' '.join(rest)}")
            else: out.append(line)
        verts.append(out)
    faces = []
    for chunk in doc["faces"]:
        out = []
        for line in chunk:
            if line.startswith("TRI "):
                t = line.split(); out.append(f"TRI {int(t[1])+object_at} {int(t[2])+material_at} {' '.join(t[3:])}")
            elif line.startswith("VERT"):
                t = line.split(); out.append(f"{t[0]} {int(t[1])+vert_at}")
            else: out.append(line)
        faces.append(out)
    objects = []
    for line in doc["objects"]:
        t = line.split(maxsplit=2); objects.append(f"OBJECT {int(t[1])+object_at} {t[2]}")
    materials = []
    for chunk in doc["materials"]:
        out = list(chunk); t = out[0].split(maxsplit=2); out[0] = f"MATERIAL {int(t[1])+material_at} {t[2]}"; materials.append(out)
    return {"bones":bones,"bone_data":bone_data,"verts":verts,"faces":faces,"objects":objects,"materials":materials}


def _vertex_key(chunk, fallback_index):
    """Build a conservative exact key; face-corner UVs/normals stay on faces."""
    position = None
    weights = []
    extra = []
    bone_count = None
    for line in chunk[1:]:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "OFFSET" and len(parts) >= 4:
            position = tuple(float(value.strip(",")) for value in parts[1:4])
        elif parts[0] == "BONES" and len(parts) >= 2:
            bone_count = int(parts[1])
        elif parts[0] == "BONE" and len(parts) >= 3:
            weights.append((int(parts[1]), tuple(float(value.strip(",")) for value in parts[2:])))
        else:
            # Unknown per-vertex attributes must match exactly before welding.
            extra.append(tuple(parts))
    if position is None:
        # A vertex without a recognized position cannot safely be merged.
        return ("unique", fallback_index)
    return (position, bone_count, tuple(sorted(weights)), tuple(extra))


def optimize_mesh(d):
    """Weld exact duplicate vertices and discard only degenerate/exact duplicate faces."""
    old_vertex_count = len(d["verts"])
    old_face_count = len(d["faces"])
    remap = {}
    unique = []
    by_key = {}
    for chunk in d["verts"]:
        header = chunk[0].split()
        old_index = int(header[1])
        key = _vertex_key(chunk, old_index)
        new_index = by_key.get(key)
        if new_index is None:
            new_index = len(unique)
            by_key[key] = new_index
            token = "VERT32" if header[0] == "VERT32" else "VERT"
            unique.append([f"{token} {new_index}", *chunk[1:]])
        remap[old_index] = new_index

    kept_faces = []
    face_keys = set()
    degenerate = 0
    duplicate = 0
    for chunk in d["faces"]:
        out = []
        corner_indices = []
        for line in chunk:
            if line.startswith(("VERT ", "VERT32 ")):
                parts = line.split()
                old_index = int(parts[1])
                new_index = remap.get(old_index)
                if new_index is None:
                    raise ValueError(f"face references missing vertex {old_index}")
                out.append(f"{parts[0]} {new_index}")
                corner_indices.append(new_index)
            else:
                out.append(line)
        if len(corner_indices) >= 3 and len(set(corner_indices[:3])) < 3:
            degenerate += 1
            continue
        # Preserve winding, material, normals, colors and UVs; only exact repeated
        # face records are removed. No geometric simplification is performed.
        key = tuple(out)
        if key in face_keys:
            duplicate += 1
            continue
        face_keys.add(key)
        kept_faces.append(out)

    d["verts"] = unique
    d["faces"] = kept_faces
    return {"vertices_before": old_vertex_count, "vertices_after": len(unique),
            "faces_before": old_face_count, "faces_after": len(kept_faces),
            "degenerate_faces_removed": degenerate, "duplicate_faces_removed": duplicate}


def write(path, a, b):
    d = {k: a[k] + b[k] for k in a}
    for expected, line in enumerate(d["bones"]):
        match = re.match(r"BONE\s+(\d+)\s+(-?\d+)\s+", line)
        if not match or int(match[1]) != expected:
            raise ValueError(f"invalid bone index at slot {expected}: {line}")
        parent = int(match[2])
        if (expected == 0 and parent != -1) or (expected > 0 and not 0 <= parent < expected):
            raise ValueError(f"invalid bone parent at bone {expected}: parent={parent}")
    cleanup = optimize_mesh(d)
    wide = any(chunk[0].startswith("VERT32 ") for chunk in d["verts"])
    vertices = sum(d["verts"], [])
    faces = sum(d["faces"], [])
    if wide:
        vertices = [re.sub(r"^VERT ", "VERT32 ", line) for line in vertices]
        faces = [re.sub(r"^VERT ", "VERT32 ", line) for line in faces]
    vert_count = "NUMVERTS32" if wide else "NUMVERTS"
    lines = ["MODEL", "VERSION 6", "", f"NUMBONES {len(d['bones'])}", *d["bones"], "", *sum(d["bone_data"], []), "", f"{vert_count} {len(d['verts'])}", "", *vertices, "", f"NUMFACES {len(d['faces'])}", "", *faces, "", f"NUMOBJECTS {len(d['objects'])}", *d["objects"], "", f"NUMMATERIALS {len(d['materials'])}"]
    for m in d["materials"]: lines.extend(m)
    with open(path, "w", encoding="utf-8", newline="\n") as f: f.write("\n".join(lines) + "\n")
    return cleanup


def single_root(doc):
    """Return true when a model's vertices all use its one root bone."""
    if len(doc["bones"]) != 1 or len(doc["bone_data"]) != 1:
        return False
    refs = []
    for chunk in doc["verts"]:
        refs.extend(int(line.split()[1]) for line in chunk if line.startswith("BONE "))
    if not refs or any(index != 0 for index in refs):
        return False
    return True


def root_vectors(doc):
    values = {}
    for line in doc["bone_data"][0]:
        parts = line.split()
        if parts and parts[0] in ("OFFSET", "SCALE", "X", "Y", "Z"):
            values[parts[0]] = tuple(float(v.strip(",")) for v in parts[1:4])
    return {"OFFSET": values.get("OFFSET", (0, 0, 0)), "SCALE": values.get("SCALE", (1, 1, 1)),
            "X": values.get("X", (1, 0, 0)), "Y": values.get("Y", (0, 1, 0)), "Z": values.get("Z", (0, 0, 1))}


def shifted_baked_single_root(doc, vert_at, face_at, object_at, material_at, transform):
    """Bake a static one-root model's skeleton and placement into vertices and reuse bone 0."""
    out = shifted(doc, 0, vert_at, face_at, object_at, material_at)
    out["bones"] = []
    out["bone_data"] = []
    root = root_vectors(doc)
    offset, scale = root["OFFSET"], root["SCALE"]
    axes = (root["X"], root["Y"], root["Z"])

    def bake_position(x, y, z):
        scaled = (x * scale[0], y * scale[1], z * scale[2])
        local = tuple(offset[i] + sum(axes[j][i] * scaled[j] for j in range(3)) for i in range(3))
        return transform(*local, True)

    def bake_normal(x, y, z):
        scaled = (x / scale[0] if scale[0] else 0.0, y / scale[1] if scale[1] else 0.0, z / scale[2] if scale[2] else 0.0)
        local = tuple(sum(axes[j][i] * scaled[j] for j in range(3)) for i in range(3))
        rotated = transform(*local, False)
        length = math.sqrt(sum(v * v for v in rotated))
        return tuple(v / length for v in rotated) if length else rotated

    # A negative determinant mirrors the root coordinate system. Reverse each
    # triangle's corner records in that case so its winding remains consistent
    # with the inverse-transpose normal transform.
    x_axis, y_axis, z_axis = axes
    determinant = (x_axis[0] * (y_axis[1] * z_axis[2] - y_axis[2] * z_axis[1])
                   - y_axis[0] * (x_axis[1] * z_axis[2] - x_axis[2] * z_axis[1])
                   + z_axis[0] * (x_axis[1] * y_axis[2] - x_axis[2] * y_axis[1]))
    determinant *= scale[0] * scale[1] * scale[2]

    for chunk in out["verts"]:
        baked = []
        for line in chunk:
            if line.startswith("OFFSET "):
                _, *values = line.split()
                x, y, z = map(lambda v: float(v.strip(",")), values[:3])
                x, y, z = bake_position(x, y, z)
                baked.append(f"OFFSET {x:.6f} {y:.6f} {z:.6f}")
            elif line.startswith("BONE "):
                parts = line.split(maxsplit=2)
                baked.append(f"BONE 0 {parts[2] if len(parts) > 2 else '1'}")
            else:
                baked.append(line)
        chunk[:] = baked
    for chunk in out["faces"]:
        for index, line in enumerate(chunk):
            if line.startswith("NORMAL "):
                _, *values = line.split()
                x, y, z = map(lambda v: float(v.strip(",")), values[:3])
                x, y, z = bake_normal(x, y, z)
                chunk[index] = f"NORMAL {x:.6f} {y:.6f} {z:.6f}"
        if determinant < 0.0:
            # Keep TRI metadata intact while swapping the second and third full
            # corner records (VERT/NORMAL/COLOR/UV) together.
            prefix = []
            corners = []
            current = None
            for line in chunk:
                if line.startswith(("VERT ", "VERT32 ")):
                    if current is not None:
                        corners.append(current)
                    current = [line]
                elif current is None:
                    prefix.append(line)
                else:
                    current.append(line)
            if current is not None:
                corners.append(current)
            if len(corners) == 3:
                chunk[:] = prefix + corners[0] + corners[2] + corners[1]
    return out


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--optimize-only":
        path = sys.argv[2]
        doc, _ = sections(read(path))
        cleanup = write(path, doc, {key: [] for key in doc})
        with Path(__file__).with_name("BrushModeler.log").open("a", encoding="utf-8") as log:
            log.write(f"mesh cleanup: vertices={cleanup['vertices_before']}->{cleanup['vertices_after']} faces={cleanup['faces_before']}->{cleanup['faces_after']} degenerate_removed={cleanup['degenerate_faces_removed']} exact_duplicates_removed={cleanup['duplicate_faces_removed']} mode=brush_patch_only\n")
        return
    allow_empty = len(sys.argv) == 13 and sys.argv[12] == "--allow-empty-base"
    if len(sys.argv) not in (12, 13) or (len(sys.argv) == 13 and not allow_empty):
        raise SystemExit("usage: merge_xmodels.py base_export model_export ox oy oz cx cy cz pitch yaw roll [--allow-empty-base]")
    a, _ = sections(read(sys.argv[1]), allow_empty=allow_empty); b, _ = sections(read(sys.argv[2]))
    # The first export is the selected brush mesh. Keep its origin bone at index zero,
    # then append all source model bones and remap every indexed section.
    aa = shifted(a, 0, 0, 0, 0, 0)
    ox, oy, oz, cx, cy, cz, pitch, yaw, roll = map(float, sys.argv[3:12])
    p, y, r = map(math.radians, (pitch, yaw, roll))
    cp, sp, cyaw, syaw, cr, sr = math.cos(p), math.sin(p), math.cos(y), math.sin(y), math.cos(r), math.sin(r)
    rot = ((cyaw*cp, cyaw*sp*sr-syaw*cr, cyaw*sp*cr+syaw*sr),
           (syaw*cp, syaw*sp*sr+cyaw*cr, syaw*sp*cr-cyaw*sr),
           (-sp, cp*sr, cp*cr))
    def transform(x, y, z, is_offset):
        q = (x, y, z); out = tuple(sum(rot[i][j]*q[j] for j in range(3)) for i in range(3))
        if is_offset: out = (out[0]+ox-cx, out[1]+oy-cy, out[2]+oz-cz)
        return out
    if single_root(b):
        # A second tag_origin bone's offset is not applied consistently by Radiant
        # after export2bin. Bake the entity-relative position into the static model
        # vertices and bind them to the brush model's existing root instead.
        bb = shifted_baked_single_root(b, len(a["verts"]), len(a["faces"]), len(a["objects"]), len(a["materials"]), transform)
        strategy = "baked_to_shared_root"
    else:
        bb = shifted(b, len(a["bones"]), len(a["verts"]), len(a["faces"]), len(a["objects"]), len(a["materials"]), transform)
        strategy = "attached_source_skeleton"
    cleanup = write(sys.argv[1], aa, bb)
    with Path(__file__).with_name("BrushModeler.log").open("a", encoding="utf-8") as log:
        log.write(f"mesh cleanup: vertices={cleanup['vertices_before']}->{cleanup['vertices_after']} faces={cleanup['faces_before']}->{cleanup['faces_after']} degenerate_removed={cleanup['degenerate_faces_removed']} exact_duplicates_removed={cleanup['duplicate_faces_removed']}\n")
        log.write(f"merge complete: strategy={strategy} source_bones={len(b['bones'])} source_vertices={len(b['verts'])} origin=({ox:.3f},{oy:.3f},{oz:.3f}) output_origin=({cx:.3f},{cy:.3f},{cz:.3f}) offset=({ox-cx:.3f},{oy-cy:.3f},{oz-cz:.3f}) angles=({pitch:.3f},{yaw:.3f},{roll:.3f})\n")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        # The plugin launches Python without a console. Put the actionable exception in
        # the same log the user already collects for support.
        log_path = Path(__file__).with_name("BrushModeler.log")
        try:
            with log_path.open("a", encoding="utf-8") as log:
                log.write(f"merge script failed: {type(exc).__name__}: {exc}\n")
        except OSError:
            pass
        raise
