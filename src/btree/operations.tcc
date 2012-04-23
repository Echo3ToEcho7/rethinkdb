#include "btree/internal_node.hpp"
#include "btree/leaf_node.hpp"
#include "btree/node.hpp"
#include "btree/slice.hpp"
#include "btree/buf_patches.hpp"
#include "buffer_cache/buffer_cache.hpp"

// TODO: consider B#/B* trees to improve space efficiency

// TODO: perhaps allow memory reclamation due to oversplitting? We can
// be smart and only use a limited amount of ram for incomplete nodes
// (doing this efficiently very tricky for high insert
// workloads). Also, if the serializer is log-structured, we can write
// only a small part of each node.

// TODO: change rwi_write to rwi_intent followed by rwi_upgrade where
// relevant.





template <class Value>
void find_keyvalue_location_for_write(transaction_t *txn, got_superblock_t *got_superblock, btree_key_t *key, keyvalue_location_t<Value> *keyvalue_location_out, eviction_priority_t *root_eviction_priority) {
    value_sizer_t<Value> sizer(txn->get_cache()->get_block_size());
    keyvalue_location_out->sb.swap(got_superblock->sb);

    buf_lock_t last_buf;
    buf_lock_t buf;
    get_root(&sizer, txn, keyvalue_location_out->sb.get(), &buf, *root_eviction_priority);

    // Walk down the tree to the leaf.
    while (node::is_internal(reinterpret_cast<const node_t *>(buf.get_data_read()))) {
        // Check if the node is overfull and proactively split it if it is (since this is an internal node).
        check_and_handle_split(&sizer, txn, buf, last_buf, keyvalue_location_out->sb.get(), key, reinterpret_cast<Value *>(NULL), root_eviction_priority);

        // Check if the node is underfull, and merge/level if it is.
        check_and_handle_underfull(&sizer, txn, buf, last_buf, keyvalue_location_out->sb.get(), key);

        // Release the superblock, if we've gone past the root (and haven't
        // already released it). If we're still at the root or at one of
        // its direct children, we might still want to replace the root, so
        // we can't release the superblock yet.
        if (last_buf.is_acquired()) {
            keyvalue_location_out->sb->release();
        }

        // Release the old previous node (unless we're at the root), and set
        // the next previous node (which is the current node).

        // Look up and acquire the next node.
        block_id_t node_id = internal_node::lookup(reinterpret_cast<const internal_node_t *>(buf.get_data_read()), key);
        rassert(node_id != NULL_BLOCK_ID && node_id != SUPERBLOCK_ID);

        buf_lock_t tmp(txn, node_id, rwi_write);
        tmp.set_eviction_priority(incr_priority(buf.get_eviction_priority()));
        last_buf.swap(tmp);
        buf.swap(last_buf);
    }

    {
        scoped_malloc<Value> tmp(sizer.max_possible_size());

        // We've gone down the tree and gotten to a leaf. Now look up the key.
        bool key_found = leaf::lookup(&sizer, reinterpret_cast<const leaf_node_t *>(buf.get_data_read()), key, tmp.get());

        if (key_found) {
            keyvalue_location_out->there_originally_was_value = true;
            keyvalue_location_out->value.swap(tmp);
        }
    }

    keyvalue_location_out->last_buf.swap(last_buf);
    keyvalue_location_out->buf.swap(buf);
}

template <class Value>
void find_keyvalue_location_for_read(transaction_t *txn, got_superblock_t *got_superblock, btree_key_t *key, keyvalue_location_t<Value> *keyvalue_location_out, eviction_priority_t root_eviction_priority) {
    value_sizer_t<Value> sizer(txn->get_cache()->get_block_size());

    block_id_t node_id = got_superblock->sb->get_root_block_id();
    rassert(node_id != SUPERBLOCK_ID);

    buf_lock_t buf;
    got_superblock->sb->swap_buf(buf);

    if (node_id == NULL_BLOCK_ID) {
        // There is no root, so the tree is empty.
        return;
    }

    {
        buf_lock_t tmp(txn, node_id, rwi_read);
        tmp.set_eviction_priority(root_eviction_priority);
        buf.swap(tmp);
    }

#ifndef NDEBUG
    node::validate(&sizer, reinterpret_cast<const node_t *>(buf.get_data_read()));
#endif  // NDEBUG

    while (node::is_internal(reinterpret_cast<const node_t *>(buf.get_data_read()))) {
        node_id = internal_node::lookup(reinterpret_cast<const internal_node_t *>(buf.get_data_read()), key);
        rassert(node_id != NULL_BLOCK_ID && node_id != SUPERBLOCK_ID);

        {
            buf_lock_t tmp(txn, node_id, rwi_read);
            tmp.set_eviction_priority(incr_priority(buf.get_eviction_priority()));
            buf.swap(tmp);
        }

#ifndef NDEBUG
        node::validate(&sizer, reinterpret_cast<const node_t *>(buf.get_data_read()));
#endif  // NDEBUG
    }

    // Got down to the leaf, now probe it.
    const leaf_node_t *leaf = reinterpret_cast<const leaf_node_t *>(buf.get_data_read());
    scoped_malloc<Value> value(sizer.max_possible_size());
    if (leaf::lookup(&sizer, leaf, key, value.get())) {
        keyvalue_location_out->buf.swap(buf);
        keyvalue_location_out->there_originally_was_value = true;
        keyvalue_location_out->value.swap(value);
    }
}

template <class Value>
void apply_keyvalue_change(transaction_t *txn, keyvalue_location_t<Value> *kv_loc, btree_key_t *key, repli_timestamp_t tstamp, bool expired, key_modification_callback_t<Value> *km_callback, eviction_priority_t *root_eviction_priority) {
    value_sizer_t<Value> sizer(txn->get_cache()->get_block_size());

    key_modification_proof_t km_proof = km_callback->value_modification(txn, kv_loc, key);

    if (kv_loc->value) {
        // We have a value to insert.

        // Split the node if necessary, to make sure that we have room
        // for the value.  Not necessary when deleting, because the
        // node won't grow.

        check_and_handle_split(&sizer, txn, kv_loc->buf, kv_loc->last_buf, kv_loc->sb.get(), key, kv_loc->value.get(), root_eviction_priority);

        rassert(!leaf::is_full(&sizer, reinterpret_cast<const leaf_node_t *>(kv_loc->buf.get_data_read()),
                key, kv_loc->value.get()));

        leaf_patched_insert(&sizer, &kv_loc->buf, key, kv_loc->value.get(), tstamp, km_proof);
    } else {
        // Delete the value if it's there.
        if (kv_loc->there_originally_was_value) {
            if (!expired) {
                rassert(tstamp != repli_timestamp_t::invalid, "Deletes need a valid timestamp now.");
                leaf_patched_remove(&kv_loc->buf, key, tstamp, km_proof);
            } else {
                // Expirations do an erase, not a delete.
                leaf_patched_erase_presence(&kv_loc->buf, key, km_proof);
            }
        }
    }

    // Check to see if the leaf is underfull (following a change in
    // size or a deletion, and merge/level if it is.
    check_and_handle_underfull(&sizer, txn, kv_loc->buf, kv_loc->last_buf, kv_loc->sb.get(), key);
}

template <class Value>
void apply_keyvalue_change(transaction_t *txn, keyvalue_location_t<Value> *kv_loc, btree_key_t *key, repli_timestamp_t tstamp, key_modification_callback_t<Value> *km_callback, eviction_priority_t *root_eviction_priority) {
    apply_keyvalue_change(txn, kv_loc, key, tstamp, false, km_callback, root_eviction_priority);
}

template <class Value>
void get_value_read(btree_slice_t *slice, btree_key_t *key, order_token_t token, keyvalue_location_t<Value> *kv_location_out, boost::scoped_ptr<transaction_t>& txn_out) {
    got_superblock_t got_superblock;
    get_btree_superblock_for_reading(slice, rwi_read, token, false, &got_superblock, txn_out);

    find_keyvalue_location_for_read(txn_out.get(), &got_superblock, key, kv_location_out, slice->root_eviction_priority);
}

