/*
 *------------
 * This code is provided solely for the personal and private use of
 * students taking the CSC369H5 course at the University of Toronto.
 * Copying for purposes other than this use is expressly prohibited.
 * All forms of distribution of this code, whether as given or with
 * any changes, are expressly prohibited.
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2025 MCS @ UTM
 * -------------
 */

#include "ext2fsal.h"
#include "e2fs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

extern unsigned char *disk;
extern struct ext2_super_block *sb;
extern struct ext2_group_desc *gd;
extern unsigned char *block_bitmap;
extern unsigned char* inode_bitmap;
extern struct ext2_inode *inode_table;
extern pthread_mutex_t inode_bitmap_lock;
extern pthread_mutex_t datablock_bitmap_lock;
extern pthread_mutex_t superblock_lock;
extern pthread_mutex_t group_desc_lock;


int32_t ext2_fsal_ln_sl(const char *src,
                        const char *dst)
{
    /**
     * TODO: implement the ext2_ln_sl command here ...
     * src and dst are the ln command arguments described in the handout.
     */


    char* normalized_dst_path = get_normalized_path(dst);
    int dst_validation_res = validate_path_exists(normalized_dst_path);
    if (dst_validation_res == 0) {
        int dst_parent_inode_num;
        int dst_child_inode_num;
        traverse_path(normalized_dst_path, &dst_parent_inode_num, &dst_child_inode_num);
        if (is_inode_to_dir(dst_child_inode_num)) {
            free(normalized_dst_path);
            return EISDIR;
        }
        if (is_inode_to_file(dst_child_inode_num) || is_inode_to_symlink(dst_child_inode_num)) {
            free(normalized_dst_path);
            return EEXIST;
        }
    }
    else if (dst_validation_res == -2) {
        // an intermediate folder along the dest path is missing
        free(normalized_dst_path);
        return ENOENT;
    }

    // only the last name of the dst path is not existing at this point
    char* dst_path_to_parent = get_path_to_parent(normalized_dst_path);
    int dst_parent_inode_num;
    int dst_child_inode_num;
    traverse_path(dst_path_to_parent, &dst_parent_inode_num, &dst_child_inode_num);
    dst_parent_inode_num = dst_child_inode_num; // for semantic and consistency
    free(dst_path_to_parent);
    

    // extract symlink name
    char* last_slash = strrchr(normalized_dst_path, '/');
    char* link_name = (last_slash != NULL) ? (last_slash + 1) : normalized_dst_path;

    free(normalized_dst_path);

    // validate link name's length
    if (strlen(link_name) > EXT2_NAME_LEN) {
        return ENAMETOOLONG;
    }

    int symlink_inode_num = initialize_new_inode(INODE_MODE_LINK);
    if (symlink_inode_num == -1) {
        // no space left for inode
        return ENOSPC;
    }

    int block_num = find_free_block();
    if (block_num == -1) {
        release_inode(symlink_inode_num);
        return ENOSPC;
    }

    // get symlink inode and set block pointer to block_num
    struct ext2_inode* symlink_inode = &inode_table[symlink_inode_num - 1];
    symlink_inode->i_block[0] = block_num;

    // write source path to block
    unsigned char* block_ptr = (unsigned char*) (disk + block_num * EXT2_BLOCK_SIZE);
    size_t src_len = strlen(src);
    memcpy(block_ptr, src, src_len);

    // update inode metadata
    symlink_inode->i_size = src_len;
    symlink_inode->i_blocks = EXT2_BLOCK_SIZE / 512;

    // add symlink to parent directory
    if (!has_space_in_parent_last_used_block(dst_parent_inode_num, link_name)) {
        // find new block
        int new_parent_block = allocate_new_block_for_parent(dst_parent_inode_num);
        if (new_parent_block == -1) {
            // no space left available
            // undo all changes
            memset(block_ptr, 0, src_len);
            release_block(block_num);
            release_inode(symlink_inode_num);
            return ENOSPC;
        }

        add_dir_entry_to_new_block(dst_parent_inode_num, symlink_inode_num, link_name, new_parent_block, EXT2_FT_SYMLINK);
    }
    else {
        // parent's last used block has enough space
        add_dir_entry_to_last_used_block(dst_parent_inode_num, symlink_inode_num, link_name, EXT2_FT_SYMLINK);
    }

    return 0;
}
