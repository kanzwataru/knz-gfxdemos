"""
Super bare-bones mesh exporter script.
Exports into a minimal data format ready to upload to the GPU.
Vertices are deduplicated, and using index buffer.
Mesh is triangulated. Hard edges are preserved.

--- Data Format ---
VERTEX_COUNT:  u32
INDEX_COUNT:   u32
VERTEX_BUFFER: float[VERTEX_COUNT][8]
INDEX_BUFFER:  u16[INDEX_COUNT]

--- Vertex Buffer Format ---
pos.x pos.y pos.z  norm.x norm.y norm.z  uv.x uv.y

--- Usage ---
You can run the script directly with a mesh selected and it will
export a file in the same directory as the .blend file except with .bin as the extension.

Alternatively you can manually call extract_vert_index_buffers and write_data inside your own script.
"""
import os

import bpy
import bmesh
import struct

def extract_vert_index_buffers(mesh_ob):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    scene = bpy.context.scene

    # NOTE: We get the evaluated mesh from the depsgraph, so the below triangulation does not overwrite the mesh in the scene
    # TODO: Check if we need to free anything at the end.
    ob = mesh_ob.evaluated_get(depsgraph)

    mesh = ob.to_mesh()
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()

    mesh.calc_normals_split()

    uv_layer = mesh.uv_layers.active.data

    vertex_buffer = []
    index_buffer = []

    for poly in mesh.polygons:
        for idx in poly.loop_indices:
            pos = mesh.vertices[mesh.loops[idx].vertex_index].co
            normal = mesh.loops[idx].normal
            uv = uv_layer[idx].uv

            found_vertex_idx = None
            for i, vert in enumerate(vertex_buffer):
                # TODO: Fuzzy matching for working around floating point inaccuracy maybe?
                other_pos, other_normal, other_uv = vert
                if other_pos == pos and other_normal == normal and other_uv == uv:
                    found_vertex_idx = i
            
            if found_vertex_idx != None:
                index_buffer.append(found_vertex_idx)
            else:
                index_buffer.append(len(vertex_buffer))
                vertex_buffer.append((pos, normal, uv))

    return vertex_buffer, index_buffer


def print_buffers(vertex_buffer, index_buffer):
    for i, vert in enumerate(vertex_buffer):
        pos, normal, uv = vert
        print('[{}] p: {} n: {} uv: {}'.format(i, pos, normal, uv))

    for i in range(0, len(index_buffer), 3):
        print('({} {} {})'.format(index_buffer[i + 0], index_buffer[i + 1], index_buffer[i + 2]))


def write_data(path, vertex_buffer, index_buffer):
    f = open(path, 'wb')

    # NOTE: The index buffer is 16-bit
    assert(len(vertex_buffer) < 65536)
    
    # Header
    f.write(struct.pack('II', len(vertex_buffer), len(index_buffer)))
    
    # Vertex buffer
    for vert in vertex_buffer:
        pos, normal, uv = vert
        f.write(struct.pack('fff fff ff', pos.x, pos.y, pos.z, normal.x, normal.y, normal.z, uv.x, uv.y))
    
    # Index buffer
    for idx in index_buffer:
        f.write(struct.pack('H', idx))
    
    f.close()


def export_selected(path):
    selected_object = bpy.context.active_object
    
    vertex_buffer, index_buffer = extract_vert_index_buffers(selected_object)
    #print_buffers(vertex_buffer, index_buffer)
    write_data(path, vertex_buffer, index_buffer)


if __name__ == '__main__':
    path = bpy.data.filepath.replace('.blend', '.bin')
    export_selected(path)
