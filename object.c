// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    char type_str[10];

    if (type == OBJ_BLOB) strcpy(type_str, "blob");
    else if (type == OBJ_TREE) strcpy(type_str, "tree");
    else if (type == OBJ_COMMIT) strcpy(type_str, "commit");
    else return -1;

    // Build header
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    size_t total_len = header_len + len;
    char *full = malloc(total_len);
    if (!full) return -1;

    memcpy(full, header, header_len);
    memcpy(full + header_len, data, len);

    // Compute hash
    compute_hash(full, total_len, id_out);

    // Deduplication
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    // Build path
    char path[512];
    object_path(id_out, path, sizeof(path));

    // Extract directory safely
    char dir[512];
    strncpy(dir, path, sizeof(dir));
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) {
        free(full);
        return -1;
    }
    *slash = '\0';

    // Create directories
    mkdir(OBJECTS_DIR, 0755);
    mkdir(dir, 0755);

    // Temp file
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full);
        return -1;
    }

    if (write(fd, full, total_len) != (ssize_t)total_len) {
        close(fd);
        free(full);
        return -1;
    }

    fsync(fd);
    close(fd);

    // Atomic rename
    if (rename(tmp_path, path) != 0) {
        free(full);
        return -1;
    }

    free(full);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);

    char *buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    if (fread(buffer, 1, size, f) != size) {
        free(buffer);
        fclose(f);
        return -1;
    }
    fclose(f);

    // Verify hash
    ObjectID check;
    compute_hash(buffer, size, &check);
    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    // Parse header
    char *null_pos = memchr(buffer, '\0', size);
    if (!null_pos) {
        free(buffer);
        return -1;
    }

    size_t header_len = null_pos - buffer;
    char header[64];
    memcpy(header, buffer, header_len);
    header[header_len] = '\0';

    char type_str[10];
    size_t data_len;

    if (sscanf(header, "%s %zu", type_str, &data_len) != 2) {
        free(buffer);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    *data_out = malloc(data_len);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    memcpy(*data_out, null_pos + 1, data_len);
    *len_out = data_len;

    free(buffer);
    return 0;
}