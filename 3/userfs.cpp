#include "userfs.h"

#include "rlist.h"

#include <stddef.h>
#include <string>
#include <vector>
#include <cstring>
#include <unordered_set>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

static ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	char memory[BLOCK_SIZE];
	rlist in_block_list = RLIST_LINK_INITIALIZER;

};

struct file {
	rlist blocks = RLIST_HEAD_INITIALIZER(blocks);
	int refs = 0;
	std::string name;
	rlist in_file_list = RLIST_LINK_INITIALIZER;


	size_t size = 0;
	block* last = nullptr;
	int blocks_count = 0;
	bool deleted = false;
};

static rlist file_list = RLIST_HEAD_INITIALIZER(file_list);

struct filedesc {
	file *atfile = nullptr;

	size_t pos = 0;
    block* cur_block = nullptr;
    int cur_block_index = 0;

#if NEED_OPEN_FLAGS
    int flags = UFS_READ_WRITE;;
#endif
};

static std::vector<filedesc*> file_descriptors;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

static file* find_file_by_name(const char* name) {
    rlist* it;
    rlist_foreach(it, &file_list) {
        file* f = rlist_entry(it, file, in_file_list);
        if (!f->deleted && f->name == name)
            return f;
    }
    return nullptr;
}

int
ufs_open(const char *filename, int flags)
{
	if (!filename) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

#if NEED_OPEN_FLAGS
    if (!(flags & (UFS_READ_ONLY | UFS_WRITE_ONLY | UFS_READ_WRITE))) {
        flags |= UFS_READ_WRITE;
    }
#endif

    file* f = find_file_by_name(filename);

    if (!f) {
        if (flags & UFS_CREATE) {
            f = new file();
            f->name = filename;
            f->refs = 1;
            f->size = 0;
            f->last = nullptr;
			f->blocks_count = 0;
            f->deleted = false;

            rlist_add_tail(&file_list, &f->in_file_list);
        } else {
            ufs_error_code = UFS_ERR_NO_FILE;
            return -1;
        }
    } else {
        f->refs++;
		f->deleted = false;
    }

    filedesc* fd_ptr = new filedesc();
    fd_ptr->atfile = f;
    fd_ptr->pos = 0;
    fd_ptr->cur_block = nullptr;
    fd_ptr->cur_block_index = 0;
#if NEED_OPEN_FLAGS
    fd_ptr->flags = flags;
#endif

    for (size_t i = 0; i < file_descriptors.size(); ++i) {
        if (!file_descriptors[i]) {
            file_descriptors[i] = fd_ptr;
            ufs_error_code = UFS_ERR_NO_ERR;
            return i + 1; 
        }
    }

    file_descriptors.push_back(fd_ptr);
	ufs_error_code = UFS_ERR_NO_ERR;
    return file_descriptors.size(); 
}

static block* allocate_block()
{
    block* b = new block();
    std::memset(b->memory, 0, BLOCK_SIZE);
    return b;
}

static block* get_block_by_index(filedesc* desc, int index, bool create)
{
    file* f = desc->atfile;

    if (index < 0)
        return nullptr;

    if (desc->cur_block && desc->cur_block_index == index)
        return desc->cur_block;

    if (desc->cur_block && index == desc->cur_block_index + 1) {
        rlist* next = desc->cur_block->in_block_list.next;
        if (next != &f->blocks) {
            desc->cur_block = rlist_entry(next, block, in_block_list);
            desc->cur_block_index = index;
            return desc->cur_block;
        }
    }

    if (desc->cur_block && index == desc->cur_block_index - 1) {
        rlist* prev = desc->cur_block->in_block_list.prev;
        if (prev != &f->blocks) {
            desc->cur_block = rlist_entry(prev, block, in_block_list);
            desc->cur_block_index = index;
            return desc->cur_block;
        }
    }

    int i = 0;
    rlist* it = f->blocks.next;
    while (it != &f->blocks) {
        if (i == index) {
            desc->cur_block = rlist_entry(it, block, in_block_list);
            desc->cur_block_index = index;
            return desc->cur_block;
        }
        ++i;
        it = it->next;
    }

    if (!create)
        return nullptr;

    while (f->blocks_count <= index) {
        block* b = allocate_block();
        rlist_add_tail(&f->blocks, &b->in_block_list);
        f->last = b;
        ++f->blocks_count;
    }

    desc->cur_block = f->last;
    desc->cur_block_index = index;
    return f->last;
}

ssize_t ufs_write(int fd, const char *buf, size_t size)
{
    if (fd <= 0 || (size_t)(fd - 1) >= file_descriptors.size())
        return (ufs_error_code = UFS_ERR_NO_FILE, -1);

    filedesc* desc = file_descriptors[fd - 1];
    if (!desc)
        return (ufs_error_code = UFS_ERR_NO_FILE, -1);

#if NEED_OPEN_FLAGS
    if (!(desc->flags & UFS_WRITE_ONLY))
        return (ufs_error_code = UFS_ERR_NO_PERMISSION, -1);
#endif

    file* f = desc->atfile;

    if (size == 0) {
        ufs_error_code = UFS_ERR_NO_ERR;
        return 0;
    }

    if (size > MAX_FILE_SIZE || desc->pos > MAX_FILE_SIZE - size) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    size_t written = 0;
    size_t block_index = desc->pos / BLOCK_SIZE;
    size_t offset = desc->pos % BLOCK_SIZE;

    block* b = get_block_by_index(desc, (int)block_index, true);
    if (!b) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    while (written < size) {
        size_t chunk = std::min((size_t)BLOCK_SIZE - offset, size - written);

        memcpy(b->memory + offset, buf + written, chunk);

        written += chunk;
        desc->pos += chunk;

        if (written == size)
            break;

        rlist* next = b->in_block_list.next;

        if (next == &f->blocks) {
            block* new_block = allocate_block();
            rlist_add_tail(&f->blocks, &new_block->in_block_list);
            f->last = new_block;
            ++f->blocks_count;
            b = new_block;
        } else {
            b = rlist_entry(next, block, in_block_list);
        }

        ++block_index;
        offset = 0;
        desc->cur_block = b;
        desc->cur_block_index = (int)block_index;
    }

    if (desc->pos > f->size)
        f->size = desc->pos;

    desc->cur_block = b;
    desc->cur_block_index = (int)block_index;

    ufs_error_code = UFS_ERR_NO_ERR;
    return (ssize_t)written;
}

ssize_t ufs_read(int fd, char *buf, size_t size)
{
    if (fd <= 0 || (size_t)(fd - 1) >= file_descriptors.size())
        return (ufs_error_code = UFS_ERR_NO_FILE, -1);

    filedesc* desc = file_descriptors[fd - 1];
    if (!desc)
        return (ufs_error_code = UFS_ERR_NO_FILE, -1);

#if NEED_OPEN_FLAGS
    if (!(desc->flags & UFS_READ_ONLY))
        return (ufs_error_code = UFS_ERR_NO_PERMISSION, -1);
#endif

    file* f = desc->atfile;

    if (desc->pos >= f->size) {
        ufs_error_code = UFS_ERR_NO_ERR;
        return 0;
    }

    size_t to_read = std::min(size, f->size - desc->pos);
    size_t read_bytes = 0;
    size_t block_index = desc->pos / BLOCK_SIZE;
    size_t offset = desc->pos % BLOCK_SIZE;

    block* b = get_block_by_index(desc, (int)block_index, false);
    if (!b) {
        ufs_error_code = UFS_ERR_NO_ERR;
        return 0;
    }

    while (read_bytes < to_read) {
        size_t chunk = std::min((size_t)BLOCK_SIZE - offset, to_read - read_bytes);

        memcpy(buf + read_bytes, b->memory + offset, chunk);

        read_bytes += chunk;
        desc->pos += chunk;

        if (read_bytes == to_read)
            break;

        rlist* next = b->in_block_list.next;
        if (next == &f->blocks)
            break;

        b = rlist_entry(next, block, in_block_list);
        ++block_index;
        offset = 0;
        desc->cur_block = b;
        desc->cur_block_index = (int)block_index;
    }

    desc->cur_block = b;
    desc->cur_block_index = (int)block_index;

    ufs_error_code = UFS_ERR_NO_ERR;
    return (ssize_t)read_bytes;
}

static void free_file_blocks(file* f) {
    rlist* it = f->blocks.next;
    while (it != &f->blocks) {
        rlist* next = it->next;
        block* b = rlist_entry(it, block, in_block_list);
        rlist_del(&b->in_block_list);
        delete b;
        it = next;
    }

    f->last = nullptr;
	f->blocks_count = 0;
        for (auto fd : file_descriptors) {
        if (fd && fd->atfile == f) {
            fd->cur_block = nullptr;
            fd->cur_block_index = -1;
        }
    }
}

int
ufs_close(int fd)
{
	if (fd <= 0 || (size_t)(fd - 1) >= file_descriptors.size()) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    filedesc* desc = file_descriptors[fd - 1];

    if (!desc) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    file* f = desc->atfile;

    delete desc;
    file_descriptors[fd - 1] = nullptr;

    f->refs--;

    if (f->refs == 0 && f->deleted) {
        free_file_blocks(f);
        delete f;
    }

	ufs_error_code = UFS_ERR_NO_ERR;
    return 0;
}

int
ufs_delete(const char *filename)
{
	file* f = find_file_by_name(filename);

    if (!f) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    rlist_del(&f->in_file_list);

    f->deleted = true;

    for (auto fd : file_descriptors) {
        if (fd && fd->atfile == f) {
            fd->cur_block = nullptr;
            fd->cur_block_index = -1;
        }
    }

    if (f->refs == 0) {
        free_file_blocks(f);
        delete f;
    }

	ufs_error_code = UFS_ERR_NO_ERR;
    return 0;
}

#if NEED_RESIZE
int
ufs_resize(int fd, size_t new_size)
{
    if (fd <= 0 || (size_t)(fd - 1) >= file_descriptors.size()) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    filedesc* desc = file_descriptors[fd - 1];
    if (!desc) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

#if NEED_OPEN_FLAGS
    if (!(desc->flags & UFS_WRITE_ONLY)) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }
#endif

    file* f = desc->atfile;

    if (new_size > MAX_FILE_SIZE) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    size_t old_size = f->size;
    size_t old_blocks = (old_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    size_t new_blocks = (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    if (new_size > old_size) {
        while (f->blocks_count < (int)new_blocks) {
            block* b = allocate_block();
            rlist_add_tail(&f->blocks, &b->in_block_list);
            f->last = b;
            ++f->blocks_count;
        }

        if (old_size % BLOCK_SIZE != 0 && old_size / BLOCK_SIZE < new_blocks) {
            block* last_used = rlist_entry(f->blocks.prev, block, in_block_list);
            size_t from = old_size % BLOCK_SIZE;
            size_t to = std::min(new_size, old_blocks * (size_t)BLOCK_SIZE);
            if (to > old_size) {
                memset(last_used->memory + from, 0, to - old_size);
            }
        }
    } else if (new_size < old_size) {
        while (f->blocks_count > (int)new_blocks) {
            block* last_block = rlist_entry(f->blocks.prev, block, in_block_list);
            rlist_del(&last_block->in_block_list);
            delete last_block;
            --f->blocks_count;
        }

        if (new_size % BLOCK_SIZE != 0 && f->blocks_count > 0) {
            block* last_kept = rlist_entry(f->blocks.prev, block, in_block_list);
            memset(last_kept->memory + (new_size % BLOCK_SIZE), 0,
                   BLOCK_SIZE - (new_size % BLOCK_SIZE));
        }
    }

    f->size = new_size;

    for (auto fd_ptr : file_descriptors) {
        if (fd_ptr && fd_ptr->atfile == f && fd_ptr->pos > new_size) {
            fd_ptr->pos = new_size;
            fd_ptr->cur_block = nullptr;
            fd_ptr->cur_block_index = -1;
        }
    }

    ufs_error_code = UFS_ERR_NO_ERR;
    return 0;
}
#endif

void
ufs_destroy(void)
{
    std::unordered_set<file*> files_to_delete;

    for (size_t i = 0; i < file_descriptors.size(); ++i) {
        filedesc* desc = file_descriptors[i];
        if (desc) {
            if (desc->atfile)
                files_to_delete.insert(desc->atfile);
            delete desc;
            file_descriptors[i] = nullptr;
        }
    }

    std::vector<filedesc*>().swap(file_descriptors);

    rlist* it = file_list.next;
    while (it != &file_list) {
        rlist* next = it->next;
        file* f = rlist_entry(it, file, in_file_list);
        rlist_del(&f->in_file_list);
        files_to_delete.insert(f);
        it = next;
    }

    for (file* f : files_to_delete) {
        free_file_blocks(f);
        delete f;
    }

    ufs_error_code = UFS_ERR_NO_ERR;
}

