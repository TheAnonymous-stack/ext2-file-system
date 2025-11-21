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

/**
 * TODO: Make sure to add all necessary includes here
 */

#include "e2fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

 /**
  * TODO: Add any helper implementations here
  */
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
char* get_normalized_path(const char* path) {
    size_t original_len = strlen(path);
    char* normalized_path = malloc(original_len + 1);
    char prev, curr;
    prev = path[0];
    normalized_path[0] = prev;
    size_t normalized_len = 1;

    for (int i = 1; i < original_len; i++) {
        curr = path[i];
        // ignore duplicate slashes
        if (curr == prev && curr == '/') {
            continue;
        } 
        // ignore trailing slash
        else if (i == original_len - 1 && curr == '/') {
            continue;
        } 
        // valid char at this point
        else {
            normalized_path[normalized_len] = curr;
            prev = curr;
            normalized_len++;
        }
    }
    normalized_path[normalized_len] = '\0';
    return normalized_path;

}

bool is_inode_in_use(int inode_num) {
    if (inode_num == 0) return false;
    int byte_idx = (inode_num - 1) / 8;
    int bit_idx = (inode_num - 1) % 8;
    return (inode_bitmap[byte_idx] >> bit_idx) & 1;
}

int find_free_inode() {
    uint32_t inode_num;
    for (uint32_t i = 0; i < sb->s_inodes_count; i++) {
        // check if the ith inode is in use
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!((inode_bitmap[byte_idx] >> bit_idx) & 1)) {
            // inode is available
            inode_num = i + 1;
            if (inode_num == EXT2_ROOT_INO || inode_num > EXT2_GOOD_OLD_FIRST_INO) {
                return inode_num;
            }
        }
    }
    return -1;
}

int find_free_block() {
    for (uint32_t i = 0; i < sb->s_blocks_count; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!((block_bitmap[byte_idx] >> bit_idx) & 1)) {
            // block is available
            return i;
        }
    }
    return -1;
}

void release_block(int block_num) {
    block_bitmap[block_num / 8] &= ~(1 << (block_num % 8));
    gd->bg_free_blocks_count++;
    sb->s_free_blocks_count++;
}

void release_inode(int inode_num) {
    inode_bitmap[(inode_num - 1) / 8] &= ~(1 << ((inode_num - 1) % 8));
    gd->bg_free_inodes_count++;
    sb->s_free_inodes_count++;
}

int get_child_dir_inode_num(int parent_inode_num, const char* child_name) {
    /*
    Return value interpretation: 
    -1: entry with name child_name is a regular file
    -2: entry with name child_name not found in parent dir
    other values: inode number (1-based index) of entry with name child_name
    */
 
    // get inode_table first
    struct ext2_inode *inode_table = (struct ext2_inode*) (disk + gd->bg_inode_table * EXT2_BLOCK_SIZE);

    // inode number starts at 1 so need to convert to 0-based index
    struct ext2_inode *parent_inode = &inode_table[parent_inode_num - 1];

    // iterate through data blocks of parent dir
    for (int block = 0; block < 15; block++) {

        // beginning of block
        unsigned char* dir_block = disk + parent_inode->i_block[block] * EXT2_BLOCK_SIZE;
        unsigned int offset = 0;

        // read dir entries
        while (offset < EXT2_BLOCK_SIZE) {
            struct ext2_dir_entry *dir_entry = (struct ext2_dir_entry*) (dir_block + offset);
            
            if (is_inode_in_use(dir_entry->inode)) {
                // only proceed with valid dir entries

                // buffer to store name of dir entry
                char name[dir_entry->name_len + 1];
                strncpy(name, dir_entry->name, dir_entry->name_len);
                name[dir_entry->name_len] = '\0';

                if (strcmp(name, child_name) == 0) {
                    if (dir_entry->file_type == EXT2_FT_DIR) {
                        return dir_entry->inode;
                    } else if (dir_entry->file_type == EXT2_FT_REG_FILE){
                        // dir entry with same name exists but entry is a file
                        // return -1 so ext2_mkdir knows to return ENOENT
                        return -1;
                    }
                }
            }
            offset += dir_entry->rec_len;
        }
    }
    // entry with name child_name is not found in parent dir at this point
    return -2;
}