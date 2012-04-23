#ifndef BTREE_NODE_HPP_
#define BTREE_NODE_HPP_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "btree/keys.hpp"
#include "buffer_cache/types.hpp"
#include "config/args.hpp"
#include "utils.hpp"

template <class Value>
class value_sizer_t;


// Class to hold common use case.
template <>
class value_sizer_t<void> {
public:
    value_sizer_t() { }
    virtual ~value_sizer_t() { }

    virtual int size(const void *value) const = 0;
    virtual bool fits(const void *value, int length_available) const = 0;
    virtual bool deep_fsck(block_getter_t *getter, const void *value, int length_available, std::string *msg_out) const = 0;
    virtual int max_possible_size() const = 0;
    virtual block_magic_t btree_leaf_magic() const = 0;
    virtual block_size_t block_size() const = 0;

private:
    DISABLE_COPYING(value_sizer_t);
};


struct btree_superblock_t {
    block_magic_t magic;
    block_id_t root_block;

    // We are unnecessarily generous with the amount of space
    // allocated here, but there's nothing else to push out of the
    // way.
    static const int METAINFO_BLOB_MAXREFLEN = 1500;

    char metainfo_blob[METAINFO_BLOB_MAXREFLEN];

    static const block_magic_t expected_magic;
};



//Note: This struct is stored directly on disk.  Changing it invalidates old data.
struct internal_node_t {
    block_magic_t magic;
    uint16_t npairs;
    uint16_t frontmost_offset;
    uint16_t pair_offsets[0];

    static const block_magic_t expected_magic;
};


// Note: Changing this struct changes the format of the data stored on disk.
// If you change this struct, previous stored data will be misinterpreted.
struct btree_key_t {
    uint8_t size;
    char contents[];
    uint16_t full_size() const {
        return size + offsetof(btree_key_t, contents);
    }
    bool fits(int space) const {
        return space > 0 && space > size;
    }
    void print() const {
        debugf("%*.*s\n", size, size, contents);
    }
};

/* A btree_key_t can't safely be allocated because it has a zero-length 'contents' buffer. This is
to represent the fact that its size may vary on disk. A btree_key_buffer_t is a much easier-to-work-
with type. */
struct btree_key_buffer_t {
    btree_key_buffer_t() { }
    explicit btree_key_buffer_t(const btree_key_t *k) {
        assign(k);
    }
    explicit btree_key_buffer_t(const store_key_t &store_key) {
        btree_key_t *k = key();
        k->size = store_key.size;
        memcpy(k->contents, store_key.contents, store_key.size);
    }

    explicit btree_key_buffer_t(std::string &key_string) {
        btree_key_t *k = key();
        k->size = key_string.size();
        memcpy(k->contents, &key_string.at(0), k->size);
    }

    template <class iterator_type>
    btree_key_buffer_t(iterator_type beg, iterator_type end) {
        assign<iterator_type>(beg, end);
    }
    void assign(const btree_key_t *new_key) {
        btree_key_t *k = key();
        k->size = new_key->size;
        memcpy(k->contents, new_key->contents, new_key->size);
    }
    template <class iterator_type>
    void assign(iterator_type beg, iterator_type end) {
        btree_key_t *k = key();
        rassert(end - beg <= MAX_KEY_SIZE);
        k->size = end - beg;
        std::copy(beg, end, &k->contents[0]);
    }

    inline btree_key_t *key() { return reinterpret_cast<btree_key_t*>(&buffer[0]); }
private:
    char buffer[sizeof(btree_key_t) + MAX_KEY_SIZE];
};

inline std::string key_to_str(const btree_key_t* key) {
    return std::string(key->contents, key->size);
}

// A node_t is either a btree_internal_node or a btree_leaf_node.
struct node_t {
    block_magic_t magic;
};

namespace node {

inline bool is_internal(const node_t *node) {
    if (node->magic == internal_node_t::expected_magic) {
        return true;
    }
    return false;
}

inline bool is_leaf(const node_t *node) {
    // We assume that a node is a leaf whenever it's not internal.
    // Unfortunately we cannot check the magic directly, because it differs
    // for different value types.
    return !is_internal(node);
}

void print(const node_t *node);

bool is_mergable(value_sizer_t<void> *sizer, const node_t *node, const node_t *sibling, const internal_node_t *parent);

bool is_underfull(value_sizer_t<void> *sizer, const node_t *node);

void split(value_sizer_t<void> *sizer, buf_lock_t *node_buf, node_t *rnode, btree_key_t *median);

void merge(value_sizer_t<void> *sizer, node_t *node, buf_lock_t *rnode_buf, const internal_node_t *parent);

bool level(value_sizer_t<void> *sizer, int nodecmp_node_with_sib, buf_lock_t *node_buf, buf_lock_t *rnode_buf, btree_key_t *replacement_key, const internal_node_t *parent);

void validate(value_sizer_t<void> *sizer, const node_t *node);

}  // namespace node

inline void keycpy(btree_key_t *dest, const btree_key_t *src) {
    memcpy(dest, src, sizeof(btree_key_t) + src->size);
}

#endif // BTREE_NODE_HPP_
