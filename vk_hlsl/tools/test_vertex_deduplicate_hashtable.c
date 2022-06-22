/* 
 * Just a test program to see how long it would take to
 * deduplicate verts in C rather than Python (in which it's excruciatingly slow).
 * 
 * To make it (un)fair, we take in mesh data that is potentially already deduplicated,
 * then unpack the vertices and then merge them again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#define countof(x) (sizeof(x) / (sizeof(x[0])))

static char scratch_mem[128 * 1024 * 1024];
char *scratch_mem_top = scratch_mem;

static void *align_ptr(void *addr, uint64_t alignment)
{
    return (void *)(((uint64_t)addr + alignment) & ~(alignment - 1));
}

static void *push_memory(size_t size)
{
#if 1
    void *ptr = align_ptr(scratch_mem_top, 32);
    assert((uint64_t)ptr - (uint64_t)scratch_mem < sizeof(scratch_mem));

    scratch_mem_top = ptr + size;

    memset(ptr, 0, size);
    
    //printf("Pushed %d size, [%p]\n", size, ptr);

    return ptr;
#else
    return calloc(size, 1);
#endif
}

struct PosMatches {
    uint32_t indices[32]; 
    int collision_count;
    struct PosMatches *next;
};

struct VertexHashTable {
    struct PosMatches entries[256 * 1024];
    const float *verts;
};

static uint64_t hash_pos(const float *vert)
{
    uint32_t a = *(uint32_t *)&vert[0];
    uint32_t b = *(uint32_t *)&vert[1];
    uint32_t c = *(uint32_t *)&vert[2];
    
    uint32_t lower = a | (b >> 4) ^ c;
    uint32_t upper = b ^ (a >> 5) ^ c;
    
    uint64_t hash_value = (upper << 31) | lower;

    return hash_value;
}

static bool vert_compare(const float *a, const float *b)
{
    return (
        a[0] == b[0] &&
        a[1] == b[1] &&
        a[2] == b[2] &&
        a[3] == b[3] &&
        a[4] == b[4] &&
        a[5] == b[5] &&
        a[6] == b[6] &&
        a[7] == b[7]
    );
}

// Add to hash table assuming that it isn't already there
static void verthash_add_assume_unique(struct VertexHashTable *table, uint32_t index)
{
    _Static_assert(countof(table->entries) % 2 == 0, "");

    const float *vert = &table->verts[index * 8];
    uint64_t hash = hash_pos(vert);
    struct PosMatches *matches = &table->entries[hash & (countof(table->entries) - 1)];
    
    while(matches->next) {
        matches = matches->next;
    }

    if(matches->collision_count == countof(matches->indices)) {
        matches->next = push_memory(sizeof(*matches));
        matches = matches->next;
    }
    
    matches->indices[matches->collision_count++] = index;
    assert(matches->collision_count <= 32);
}

static bool verthash_lookup(struct VertexHashTable *table, const float *vert, uint32_t *out_index)
{
    uint64_t hash = hash_pos(vert);
    const struct PosMatches *matches = &table->entries[hash & (countof(table->entries) - 1)];

    const struct PosMatches *stack[256];
    volatile int depth = 0;

    do {
        assert(matches->collision_count <= 32);
        for(int i = 0; i < matches->collision_count; ++i) {
            uint32_t match_idx = matches->indices[i];
            
            if(vert_compare(&table->verts[match_idx * 8], vert)) {
                *out_index = match_idx;
                return true;
            }
        }
        
        stack[depth++] = matches;
        matches = matches->next;
    } while(matches);

    return false;
}

int main(int argc, char **argv)
{  
    /* Load in existing mesh data */
    printf("Loading in data...\n");
    FILE *fp = fopen(argv[1], "rb");

    uint32_t vert_count;
    uint32_t index_count;

    fread(&vert_count, sizeof(vert_count), 1, fp);
    fread(&index_count, sizeof(index_count), 1, fp);

    uint32_t vert_elem_count = 8;
    uint32_t vert_size_bytes = vert_elem_count * sizeof(float);
    
    uint32_t vert_buffer_size = vert_count * vert_size_bytes;
    uint32_t index_buffer_size = index_count * sizeof(uint16_t);

    float *vert_buffer = push_memory(vert_buffer_size);
    uint16_t *index_buffer = push_memory(index_buffer_size);
    
    fread(vert_buffer, vert_buffer_size, 1, fp);
    fread(index_buffer, index_buffer_size, 1, fp);

    fclose(fp);
    
    /* Unpack vertices */
    printf("Unpacking vertices...\n");
    uint32_t max_vert_buffer_size = index_count * vert_size_bytes;
    float *vert_buffer_unpacked = push_memory(max_vert_buffer_size);
    
    uint32_t unpacked_vert_count = 0;
    
    for(uint32_t i = 0; i < index_count; ++i) {
        uint16_t index = index_buffer[i];

        memcpy(&vert_buffer_unpacked[vert_elem_count * unpacked_vert_count++],
               &vert_buffer[index * vert_elem_count],
               vert_size_bytes);
    }
    
    assert(unpacked_vert_count == index_count);

    /* Re-deduplicate vertices */
    printf("Deduplicating vertices...\n");
    float *new_vertex_buffer = push_memory(max_vert_buffer_size);
    uint16_t *new_index_buffer = push_memory(index_buffer_size);

    struct VertexHashTable *hash_table = push_memory(sizeof(*hash_table));
    *hash_table = (struct VertexHashTable) {
        .verts = new_vertex_buffer
    };

    uint32_t new_index_buffer_count = 0;
    uint32_t new_vertex_buffer_count = 0;

    for(uint32_t i = 0; i < unpacked_vert_count; ++i) {
        uint32_t found_index;
        bool found = verthash_lookup(hash_table, &vert_buffer_unpacked[i * 8], &found_index);

        if(found) {
            new_index_buffer[new_index_buffer_count++] = found_index;
        }
        else {
            uint32_t vert_index = new_vertex_buffer_count++;
            new_index_buffer[new_index_buffer_count++] = vert_index;
            
            memcpy(&new_vertex_buffer[vert_elem_count * vert_index],
                   &vert_buffer_unpacked[vert_elem_count * i],
                   vert_size_bytes);
            
            verthash_add_assume_unique(hash_table, vert_index);
        }
    }

    printf("Writing out data...\n");
    fp = fopen("test.bin", "wb");
    fwrite(&new_vertex_buffer_count, sizeof(new_vertex_buffer_count), 1, fp);
    fwrite(&new_index_buffer_count, sizeof(new_index_buffer_count), 1, fp);
    fwrite(new_vertex_buffer, new_vertex_buffer_count * vert_size_bytes, 1, fp);
    fwrite(new_index_buffer, new_index_buffer_count * sizeof(uint16_t), 1, fp);
    fclose(fp);

    printf("Done. Stats:\n");
    printf("\tOriginal vert count: %d\n", vert_count);
    printf("\tOriginal index count: %d\n", index_count);
    printf("\tNew vert count: %d\n", new_vertex_buffer_count);
    printf("\tNew index count: %d\n", new_index_buffer_count);

    return 0;
}
