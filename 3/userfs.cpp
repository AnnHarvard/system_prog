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

    for (size_t i = 0; i < file_descriptors.size(); ++i) {
        if (!file_descriptors[i]) {
            file_descriptors[i] = fd_ptr;
            return i + 1; 
        }
    }

    file_descriptors.push_back(fd_ptr);
	ufs_error_code = UFS_ERR_NO_ERR;
    return file_descriptors.size(); 
}

static block* allocate_block() {
    block* b = new block();
	std::memset(b->memory, 0, BLOCK_SIZE);
    return b;
}

static block* get_block_by_index(file* f, int index, bool create)
{
    if (index < 0)
        return nullptr;

    if (index < f->blocks_count) {
        if (index < f->blocks_count / 2) {
            int i = 0;
            rlist* it = f->blocks.next;
            while (it != &f->blocks) {
                if (i == index)
                    return rlist_entry(it, block, in_block_list);
                i++;
                it = it->next;
            }
        } else {
            int i = f->blocks_count - 1;
            rlist* it = f->blocks.prev;
            while (it != &f->blocks) {
                if (i == index)
                    return rlist_entry(it, block, in_block_list);
                i--;
                it = it->prev;
            }
        }
    }

    if (!create)
        return nullptr;

    while (f->blocks_count <= index) {
        block* new_block = allocate_block();
        if (!new_block) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return nullptr;
        }

        rlist_add_tail(&f->blocks, &new_block->in_block_list);
        f->last = new_block;
        f->blocks_count++;
    }

    if (index == f->blocks_count - 1)
        return f->last;

    int i = 0;
    rlist* it = f->blocks.next;
    while (it != &f->blocks) {
        if (i == index)
            return rlist_entry(it, block, in_block_list);
        i++;
        it = it->next;
    }

    return nullptr;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
    if (fd <= 0 || (size_t)(fd - 1) >= file_descriptors.size()) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (size == 0) {
        ufs_error_code = UFS_ERR_NO_ERR;
        return 0;
    }

    filedesc* desc = file_descriptors[fd - 1];
    if (!desc) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    file* f = desc->atfile;

    if (size > MAX_FILE_SIZE || desc->pos > MAX_FILE_SIZE - size) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    size_t written = 0;

    while (written < size) {
        size_t pos = desc->pos;

        int block_index = pos / BLOCK_SIZE;
        int offset = pos % BLOCK_SIZE;

        block* b = get_block_by_index(f, block_index, true);
        if (!b) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }

        size_t space = BLOCK_SIZE - offset;
        size_t remaining = size - written;
        size_t chunk = space < remaining ? space : remaining;

        memcpy(b->memory + offset, buf + written, chunk);

        desc->pos += chunk;
        written += chunk;
    }

    if (desc->pos > f->size) {
        f->size = desc->pos;
    }

    ufs_error_code = UFS_ERR_NO_ERR;
    return written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
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

    if (desc->pos >= f->size)
        return 0;

    size_t to_read = size;

    if (desc->pos + to_read > f->size)
        to_read = f->size - desc->pos;

    size_t read_bytes = 0;

    while (read_bytes < to_read) {

        size_t pos = desc->pos;

        int block_index = pos / BLOCK_SIZE;
        int offset = pos % BLOCK_SIZE;

        block* b = get_block_by_index(f, block_index, false);

        if (!b)
            break;

        size_t space = BLOCK_SIZE - offset;
        size_t remaining = to_read - read_bytes;

        size_t chunk = space < remaining ? space : remaining;

        memcpy(buf + read_bytes, b->memory + offset, chunk);

        desc->pos += chunk;
        read_bytes += chunk;
    }

	ufs_error_code = UFS_ERR_NO_ERR;
    return read_bytes;
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
	/* IMPLEMENT THIS FUNCTION */
	(void)fd;
	(void)new_size;
	ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
	return -1;
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

