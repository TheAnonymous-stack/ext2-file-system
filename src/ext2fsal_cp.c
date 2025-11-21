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


int32_t ext2_fsal_cp(const char *src,
                     const char *dst)
{
    /**
     * TODO: implement the ext2_cp command here ...
     * Arguments src and dst are the cp command arguments described in the handout.
     */
    char* normalized_src_path = get_normalized_path(src);
    

    // check if src path is valid or not first
    const char* delimiter = "/";
    char* dir = strtok(normalized_src_path, delimiter);
    char* next_dir;
    int current_inode_num = EXT2_ROOT_INO;
    int parent_inode_num;
    int src_inode_num = -1;
    while (dir != NULL) {
        next_dir = strtok(NULL, delimiter);
        int child_inode_num = get_child_dir_inode_num(current_inode_num, dir);

        if (next_dir == NULL) {
            if (child_inode_num == -2) {
                // dir is not in parent_inode_num
                free(normalized_src_path);
                return ENOENT;
            } else if (child_inode_num == -1) {
                // dir is a regular file at this point => valid
                src_inode_num = child_inode_num;
            }
        }
    }



    char* normalized_dst_path = get_normalized_path(dst);
    free(normalized_src_path);
    free(normalized_dst_path);
    

    return 0;
}