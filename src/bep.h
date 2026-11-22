/* BEP v1 messages (Syncthing proto/bep/bep.proto) - the subset this client
 * sends and reads. Field numbers follow bep.proto; unknown fields are
 * skipped, malformed ones make the whole message invalid. */
#ifndef ST_BEP_H
#define ST_BEP_H

#include "devid.h"
#include "pb.h"

#include <stddef.h>
#include <stdint.h>

#define BEP_HELLO_MAGIC 0x2EA7D90Bu
#define BEP_HELLO_MAX 32767

enum bep_type {
    BEP_CLUSTER_CONFIG = 0,
    BEP_INDEX = 1,
    BEP_INDEX_UPDATE = 2,
    BEP_REQUEST = 3,
    BEP_RESPONSE = 4,
    BEP_DOWNLOAD_PROGRESS = 5,
    BEP_PING = 6,
    BEP_CLOSE = 7
};

enum bep_compression { BEP_COMPRESSION_NONE = 0, BEP_COMPRESSION_LZ4 = 1 };

enum bep_folder_type { BEP_FOLDER_SEND_RECEIVE = 0, BEP_FOLDER_SEND_ONLY = 1,
                       BEP_FOLDER_RECEIVE_ONLY = 2, BEP_FOLDER_RECEIVE_ENCRYPTED = 3 };

enum bep_file_type { BEP_FILE = 0, BEP_DIRECTORY = 1, BEP_SYMLINK_FILE = 2,
                     BEP_SYMLINK_DIRECTORY = 3, BEP_SYMLINK = 4 };

/* Strings below point into the decoded buffer: valid while it is. */
typedef struct {
    pb_slice device_name, client_name, client_version;
    int64_t num_connections, timestamp;
} bep_hello;

typedef struct {
    uint32_t type;
    uint32_t compression;
} bep_header;

typedef struct {
    unsigned char id[DEVID_BYTES];
    int id_valid; /* the id field was exactly 32 bytes */
    pb_slice name;
    int64_t max_sequence;
    uint64_t index_id;
    int introducer;
    size_t encryption_token_len;
} bep_cc_device;

typedef struct {
    pb_slice id, label;
    uint32_t type, stop_reason;
} bep_cc_folder;

typedef struct {
    pb_slice raw;                 /* the whole encoded FileInfo */
    pb_slice name, symlink_target;
    uint32_t type, permissions;
    int64_t size, modified_s, sequence;
    int32_t modified_ns, block_size;
    int deleted, invalid, no_permissions;
    size_t blocks, version_counters;
    uint64_t modified_by;
} bep_file;

/* Callbacks return 0 to continue, nonzero to abort decoding with that value. */
typedef int (*bep_folder_cb)(void *ctx, const bep_cc_folder *f);
typedef int (*bep_device_cb)(void *ctx, const bep_cc_folder *f, const bep_cc_device *d);
typedef int (*bep_file_cb)(void *ctx, const bep_file *f);

/* Decoders: 0 on success, -1 on malformed input, or a callback's value. */
int bep_decode_hello(const void *buf, size_t len, bep_hello *out);
int bep_decode_header(const void *buf, size_t len, bep_header *out);
int bep_decode_cluster_config(const void *buf, size_t len, void *ctx,
                              bep_folder_cb on_folder, bep_device_cb on_device);
/* Index (prev_sequence absent) and IndexUpdate share the layout. */
int bep_decode_index(const void *buf, size_t len, pb_slice *folder, int64_t *last_sequence,
                     int64_t *prev_sequence, void *ctx, bep_file_cb on_file);
int bep_decode_close(const void *buf, size_t len, pb_slice *reason);

/* Decodes one encoded FileInfo (bep_file.raw). 0 or -1. */
int bep_decode_fileinfo(pb_slice raw, bep_file *out);

/* Calls cb for each BlockInfo of an encoded FileInfo (bep_file.raw), in
 * wire order. hash is the raw hash bytes (32 for SHA-256). */
typedef int (*bep_block_cb)(void *ctx, int64_t offset, int32_t size, pb_slice hash);
int bep_file_blocks(pb_slice raw, void *ctx, bep_block_cb cb);

typedef struct {
    int32_t id, size, block_no;
    pb_slice folder, name, hash;
    int64_t offset;
    int from_temporary;
} bep_request;
int bep_decode_request_full(const void *buf, size_t len, bep_request *q);

int bep_decode_response(const void *buf, size_t len, int32_t *id, pb_slice *data, uint32_t *code);
int bep_decode_request(const void *buf, size_t len, int32_t *id, pb_slice *folder, pb_slice *name);

/* Encoders write the protobuf body only; framing is frame.c's job. */
void bep_encode_hello(pb_writer *w, const char *device_name, const char *client_name,
                      const char *client_version, int64_t timestamp);
void bep_encode_header(pb_writer *w, uint32_t type, uint32_t compression);

typedef struct {
    const unsigned char *id;
    const char *name;
    int64_t max_sequence;
    uint64_t index_id;
} bep_cc_device_out;

void bep_encode_cluster_config(pb_writer *w, const char *folder_id, const char *folder_label,
                               uint32_t folder_type, const bep_cc_device_out *devices, size_t n);
/* An Index with no files: what a device that has nothing announces. */
void bep_encode_empty_index(pb_writer *w, const char *folder_id);
void bep_encode_close(pb_writer *w, const char *reason);
void bep_encode_response_error(pb_writer *w, int32_t id, uint32_t code);
/* A Response carrying data (code NO_ERROR, which proto3 leaves out). */
void bep_encode_response_data(pb_writer *w, int32_t id, const void *data, size_t len);
void bep_encode_request(pb_writer *w, int32_t id, const char *folder, const char *name,
                        int64_t offset, int32_t size, const unsigned char *hash, size_t hash_len,
                        int32_t block_no);

/* IndexUpdate announcing files we now have: start with the header fields,
 * then add each file. A file is announced as a copy of the peer's FileInfo
 * (version vector, blocks, metadata) with our own sequence number, so the
 * peer sees exactly the version it offered as present here. */
void bep_encode_index_update_start(pb_writer *w, const char *folder, int64_t last_sequence,
                                   int64_t prev_sequence);
int bep_put_announced_file(pb_writer *w, pb_slice raw, int64_t sequence);

#endif
