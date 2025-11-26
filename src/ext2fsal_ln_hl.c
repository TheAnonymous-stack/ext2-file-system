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


int32_t ext2_fsal_ln_hl(const char *src,
                        const char *dst)
{
    /**
     * TODO: implement the ext2_ln_hl command here ...
     * src and dst are the ln command arguments described in the handout.
     */

    char* normalized_src_path = get_normalized_path(src);
    int src_validation_res = validate_path_exists(normalized_src_path);
    

    if (src_validation_res != 0) {
        free(normalized_src_path);
        return ENOENT;
    }
    int src_parent_inode_num;
    int src_child_inode_num;
    traverse_path(normalized_src_path, &src_parent_inode_num, &src_child_inode_num);
    if (is_inode_to_dir(src_child_inode_num)) {
        free(normalized_src_path);
        return EISDIR;
    }
    free(normalized_src_path);

    char* normalized_dst_path = get_normalized_path(dst);
    int dst_validation_res = validate_path_exists(normalized_dst_path);
    if (dst_validation_res == -2) {
        free(normalized_dst_path);
        return ENOENT;
    }
    int dst_parent_inode_num;
    int dst_child_inode_num;
    if (dst_validation_res == 0) {
        
        traverse_path(normalized_dst_path, &dst_parent_inode_num, &dst_child_inode_num);
        if (is_inode_to_file(dst_child_inode_num) || is_inode_to_symlink(dst_child_inode_num)) {
            free(normalized_dst_path);
            return EEXIST;
        } 
        else if (is_inode_to_dir(dst_child_inode_num)) {
            free(normalized_dst_path);
            return EISDIR;
        }
    }

    char* dst_path_to_parent = get_path_to_parent(normalized_dst_path);
    traverse_path(dst_path_to_parent, &dst_parent_inode_num, &dst_child_inode_num);
    dst_parent_inode_num = dst_child_inode_num; // for semantic and consistency
    free(dst_path_to_parent);
    
    // validate link name length
    char* last_slash = strrchr(normalized_dst_path, '/');
    char* link_name = (last_slash != NULL) ? (last_slash + 1) : normalized_dst_path;
    
    free(normalized_dst_path);

    if (strlen(link_name) > EXT2_NAME_LEN) {
        return ENAMETOOLONG;
    }


    // increment link count of source file
    struct ext2_inode* src_inode = &inode_table[src_child_inode_num - 1];
    src_inode->i_links_count++;

    // add a directory enty in dst_parent_inode_num to point to existing inode
    if (!has_space_in_parent_last_used_block(dst_parent_inode_num, link_name)) {
        int new_parent_block = allocate_new_block_for_parent(dst_parent_inode_num);
        if (new_parent_block == -1) {
            // no space available
            // undo all changes
            src_inode->i_links_count--;
            return ENOSPC;
        }
        add_dir_entry_to_new_block(dst_parent_inode_num, src_child_inode_num, link_name, new_parent_block, EXT2_FT_REG_FILE);
    } 
    else {
        add_dir_entry_to_last_used_block(dst_parent_inode_num, src_child_inode_num, link_name, EXT2_FT_REG_FILE);
    }

    return 0;
}
