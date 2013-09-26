#include "buffer_cache/alt/page.hpp"

#include "arch/runtime/coroutines.hpp"
#include "concurrency/auto_drainer.hpp"
#include "serializer/serializer.hpp"

namespace alt {

page_cache_t::page_cache_t(serializer_t *serializer)
    : serializer_(serializer),
      free_list_(serializer),
      drainer_(new auto_drainer_t) {
    {
        // RSI: Don't you hate this?
        on_thread_t thread_switcher(serializer->home_thread());
        // RSI: These priority values were configurable in the old cache.  Did
        // anything configure them?
        reads_io_account.init(serializer->make_io_account(CACHE_READS_IO_PRIORITY));
        writes_io_account.init(serializer->make_io_account(CACHE_WRITES_IO_PRIORITY));
    }
}

page_cache_t::~page_cache_t() {
    drainer_.reset();
    for (auto it = current_pages_.begin(); it != current_pages_.end(); ++it) {
        delete *it;
    }

    {
        /* IO accounts must be destroyed on the thread they were created on */
        on_thread_t thread_switcher(serializer_->home_thread());
        reads_io_account.reset();
        writes_io_account.reset();
    }
}

current_page_t *page_cache_t::page_for_block_id(block_id_t block_id) {
    if (current_pages_.size() <= block_id) {
        current_pages_.resize(block_id + 1, NULL);
    }

    if (current_pages_[block_id] == NULL) {
        current_pages_[block_id] = new current_page_t(block_id, this);
   }

    return current_pages_[block_id];
}

current_page_t *page_cache_t::page_for_new_block_id(block_id_t *block_id_out) {
    block_id_t block_id = free_list_.acquire_block_id();
    // RSI: Have a user-specifiable block size.
    current_page_t *ret = new current_page_t(serializer_->get_block_size(),
                                             serializer_->malloc(),
                                             this);
    *block_id_out = block_id;
    return ret;
}

current_page_acq_t::current_page_acq_t(current_page_t *current_page,
                                       alt_access_t access)
    : access_(access),
      current_page_(current_page),
      snapshotted_page_(NULL) {
    current_page_->add_acquirer(this);
}

current_page_acq_t::~current_page_acq_t() {
    if (current_page_ != NULL) {
        current_page_->remove_acquirer(this);
    }
    if (snapshotted_page_ != NULL) {
        snapshotted_page_->remove_snapshotter();
    }
}

void current_page_acq_t::declare_snapshotted() {
    rassert(access_ == alt_access_t::read);
    // Allow redeclaration of snapshottedness.
    if (!declared_snapshotted_) {
        declared_snapshotted_ = true;
        rassert(current_page_ != NULL);
        current_page_->pulse_pulsables(this);
    }
}

signal_t *current_page_acq_t::read_acq_signal() {
    return &read_cond_;
}

signal_t *current_page_acq_t::write_acq_signal() {
    rassert(access_ == alt_access_t::write);
    return &write_cond_;
}

page_t *current_page_acq_t::page_for_read() {
    rassert(snapshotted_page_ != NULL || current_page_ != NULL);
    read_cond_.wait();
    if (snapshotted_page_ != NULL) {
        return snapshotted_page_;
    }
    rassert(current_page_ != NULL);
    return current_page_->the_page_for_read();
}

page_t *current_page_acq_t::page_for_write() {
    rassert(access_ == alt_access_t::write);
    rassert(current_page_ != NULL);
    write_cond_.wait();
    rassert(current_page_ != NULL);
    return current_page_->the_page_for_write();
}

current_page_t::current_page_t(block_id_t block_id, page_cache_t *page_cache)
    : block_id_(block_id),
      page_cache_(page_cache),
      page_(NULL) {
}

current_page_t::current_page_t(block_size_t block_size,
                               scoped_malloc_t<ser_buffer_t> buf,
                               page_cache_t *page_cache)
    : block_id_(NULL_BLOCK_ID),
      page_cache_(page_cache),
      page_(new page_t(block_size, std::move(buf))) {
}

current_page_t::~current_page_t() {
    rassert(acquirers_.empty());
}

void current_page_t::add_acquirer(current_page_acq_t *acq) {
    acquirers_.push_back(acq);
    pulse_pulsables(acq);
}

void current_page_t::remove_acquirer(current_page_acq_t *acq) {
    current_page_acq_t *next = acquirers_.next(acq);
    acquirers_.remove(acq);
    if (next != NULL) {
        pulse_pulsables(next);
    }
}

void current_page_t::pulse_pulsables(current_page_acq_t *const acq) {
    // First, avoid pulsing when there's nothing to pulse.
    {
        current_page_acq_t *prev = acquirers_.prev(acq);
        if (!(prev == NULL || (prev->access_ == alt_access_t::read
                               && prev->read_cond_.is_pulsed()))) {
            return;
        }
    }

    // Second, avoid re-pulsing already-pulsed chains.
    if (acq->access_ == alt_access_t::read && acq->read_cond_.is_pulsed()) {
        return;
    }

    // It's time to pulse the pulsables.
    current_page_acq_t *cur = acq;
    while (cur != NULL) {
        // We know that the previous node has read access and has been pulsed as
        // readable, so we pulse the current node as readable.
        cur->read_cond_.pulse_if_not_already_pulsed();

        if (cur->access_ == alt_access_t::read) {
            current_page_acq_t *next = acquirers_.next(cur);
            if (cur->declared_snapshotted_) {
                // Snapshotters get kicked out of the queue, to make way for
                // write-acquirers.
                cur->snapshotted_page_ = the_page_for_read();
                cur->current_page_ = NULL;
                cur->snapshotted_page_->add_snapshotter();
                acquirers_.remove(cur);
            }
            cur = next;
        } else {
            // Even the first write-acquirer gets read access (there's no need for an
            // "intent" mode).  But subsequent acquirers need to wait, because the
            // write-acquirer might modify the value.
            if (acquirers_.prev(cur) == NULL) {
                // (It gets exclusive write access if there's no preceding reader.)
                cur->write_cond_.pulse_if_not_already_pulsed();
            }
            break;
        }
    }
}

void current_page_t::convert_from_serializer_if_necessary() {
    if (page_ == NULL) {
        page_ = new page_t(block_id_, page_cache_);
        block_id_ = NULL_BLOCK_ID;
    }
}

page_t *current_page_t::the_page_for_read() {
    convert_from_serializer_if_necessary();
    return page_;
}

page_t *current_page_t::the_page_for_write() {
    convert_from_serializer_if_necessary();
    if (page_->has_snapshot_references()) {
        page_ = page_->make_copy(page_cache_);
    }
    return page_;
}

page_t::page_t(block_id_t block_id, page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      buf_size_(block_size_t::undefined()),
      snapshot_refcount_(0) {
    coro_t::spawn_now_dangerously(std::bind(&page_t::load_with_block_id,
                                            this,
                                            block_id,
                                            page_cache));
}

page_t::page_t(block_size_t block_size, scoped_malloc_t<ser_buffer_t> buf)
    : destroy_ptr_(NULL),
      buf_size_(block_size),
      buf_(std::move(buf)),
      snapshot_refcount_(0) {
    rassert(buf_.has());
}

page_t::page_t(page_t *copyee, page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      buf_size_(block_size_t::undefined()),
      snapshot_refcount_(0) {
    coro_t::spawn_now_dangerously(std::bind(&page_t::load_from_copyee,
                                            this,
                                            copyee,
                                            page_cache));
}

page_t::~page_t() {
    if (destroy_ptr_ != NULL) {
        *destroy_ptr_ = true;
    }
}

void page_t::load_from_copyee(page_t *page, page_t *copyee,
                              page_cache_t *page_cache) {
    // This is called using spawn_now_dangerously.  We need to atomically set
    // destroy_ptr_ (and add a snapshotter, why not).
    copyee->add_snapshotter();
    bool page_destroyed = false;
    rassert(page->destroy_ptr_ == NULL);
    page->destroy_ptr_ = &page_destroyed;

    // Okay, it's safe to block.
    auto_drainer_t::lock_t lock(page_cache->drainer_.get());

    {
        page_acq_t acq;
        acq.init(copyee);
        acq.buf_ready_signal()->wait();

        ASSERT_FINITE_CORO_WAITING;
        if (!page_destroyed) {
            // RSP: If somehow there are no snapshotters of copyee now (besides ourself),
            // maybe we could avoid copying this memory.  We need to carefully track
            // snapshotters anyway, once we're comfortable with that, we could do it.

            block_size_t buf_size = copyee->buf_size_;
            rassert(copyee->buf_.has());
            // RSI: Support mallocking the specific buf size.
            scoped_malloc_t<ser_buffer_t> buf = page_cache->serializer_->malloc();

            // RSI: Are we sure we want to copy the whole ser buffer, and not the cache's
            // part?  Is it _necessary_?  If unnecessary, let's not do it.
            memcpy(buf.get(), copyee->buf_.get(), buf_size.ser_value());

            page->buf_size_ = buf_size;
            page->buf_ = std::move(buf);
            page->destroy_ptr_ = NULL;

            page->pulse_waiters();
        }
    }

    // RSI: Shouldn't there be some better way to acquire and release a snapshot
    // reference?  Maybe not.
    copyee->remove_snapshotter();
}


void page_t::load_with_block_id(page_t *page, block_id_t block_id,
                                page_cache_t *page_cache) {
    // This is called using spawn_now_dangerously.  We need to atomically set
    // destroy_ptr_.
    bool page_destroyed = false;
    rassert(page->destroy_ptr_ == NULL);
    page->destroy_ptr_ = &page_destroyed;

    // Okay, now it's safe to block.  We do so by going to another thread.  RSI: Can
    // the lock constructor yield?  (It's okay if it can, but I'd be curious if we're
    // worried about on_thread_t not yielding.)
    auto_drainer_t::lock_t lock(page_cache->drainer_.get());

    scoped_malloc_t<ser_buffer_t> buf;
    counted_t<standard_block_token_t> block_token;
    {
        serializer_t *serializer = page_cache->serializer_;
        // RSI: It would be nice if we _always_ yielded here (not just when the
        // thread's different.  spawn_now_dangerously is dangerous, after all.
        on_thread_t th(serializer->home_thread());
        block_token = serializer->index_read(block_id);
        // RSI: Figure out if block_token can be empty (if a page is deleted?).
        rassert(block_token.has());
        // RSI: Support variable block size.
        buf = serializer->malloc();
        serializer->block_read(block_token,
                               buf.get(),
                               NULL  /* RSI: file account */);
    }

    ASSERT_FINITE_CORO_WAITING;
    if (page_destroyed) {
        return;
    }

    rassert(!page->block_token_.has());
    rassert(!page->buf_.has());
    rassert(block_token.has());
    page->buf_size_ = block_token->block_size();
    page->buf_ = std::move(buf);
    page->block_token_ = std::move(block_token);

    page->pulse_waiters();
}

void page_t::add_snapshotter() {
    // This may not block, because it's called at the beginning of
    // page_t::load_from_copyee.
    ASSERT_NO_CORO_WAITING;
    ++snapshot_refcount_;
}

void page_t::remove_snapshotter() {
    rassert(snapshot_refcount_ > 0);
    --snapshot_refcount_;
    // RSI: We might need to delete the page here.
}

bool page_t::has_snapshot_references() {
    return snapshot_refcount_ > 0;
}

page_t *page_t::make_copy(page_cache_t *page_cache) {
    return new page_t(this, page_cache);
}

void page_t::pulse_waiters() {
    for (page_acq_t *p = waiters_.head(); p != NULL; p = waiters_.next(p)) {
        // The waiter's not already going to have been pulsed.
        p->buf_ready_signal_.pulse();
    }
}

void page_t::add_waiter(page_acq_t *acq) {
    waiters_.push_back(acq);
    if (buf_.has()) {
        acq->buf_ready_signal_.pulse();
    }
}

void *page_t::get_buf() {
    // RSI: definitely make private, used through page_acq_t.
    rassert(buf_.has());
    return buf_->cache_data;
}

uint32_t page_t::get_buf_size() {
    // RSI: definitely make private, used through page_acq_t.
    rassert(buf_.has());
    return buf_size_.value();
}

void page_t::remove_waiter(page_acq_t *acq) {
    waiters_.remove(acq);
    if (waiters_.empty()) {
        // RSI: We might need to delete the page here.
    }
}

page_acq_t::page_acq_t() : page_(NULL) {
}

void page_acq_t::init(page_t *page) {
    rassert(page_ == NULL);
    rassert(!buf_ready_signal_.is_pulsed());
    page_ = page;
    page_->add_waiter(this);
}

bool page_acq_t::has() const {
    return page_ != NULL;
}

signal_t *page_acq_t::buf_ready_signal() {
    return &buf_ready_signal_;
}

page_acq_t::~page_acq_t() {
    if (page_ != NULL) {
        page_->remove_waiter(this);
    }
}

free_list_t::free_list_t(serializer_t *serializer) {
    // RSI: Maybe this should be specifically constructed when we visit the
    // serializer's home thread (and setting the file accounts).
    on_thread_t th(serializer->home_thread());

    next_new_block_id_ = serializer->max_block_id();
    for (block_id_t i = 0; i < next_new_block_id_; ++i) {
        if (serializer->get_delete_bit(i)) {
            free_ids_.push_back(i);
        }
    }
}

free_list_t::~free_list_t() { }

block_id_t free_list_t::acquire_block_id() {
    if (free_ids_.empty()) {
        block_id_t ret = next_new_block_id_;
        ++next_new_block_id_;
        return ret;
    } else {
        block_id_t ret = free_ids_.back();
        free_ids_.pop_back();
        return ret;
    }
}

void free_list_t::release_block_id(block_id_t block_id) {
    free_ids_.push_back(block_id);
}


}  // namespace alt

