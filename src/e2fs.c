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
    // check if there exists a reserved inode with number inode_num

    if (inode_num == 0) return false;
    int byte_idx = (inode_num - 1) / 8;
    int bit_idx = (inode_num - 1) % 8;
    return (inode_bitmap[byte_idx] >> bit_idx) & 1;
}

int find_free_inode() {
    int inode_num;
    for (int i = 0; i < sb->s_inodes_count; i++) {
        // check if the ith inode is in use
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!((inode_bitmap[byte_idx] >> bit_idx) & 1)) {
            // inode is available
            inode_num = i + 1;
            if (inode_num == EXT2_ROOT_INO || inode_num > EXT2_GOOD_OLD_FIRST_INO) {

                inode_bitmap[byte_idx] |= (1 << bit_idx);
                gd->bg_free_inodes_count--;
                sb->s_free_inodes_count--;

                return inode_num;
            }
        }
    }
    return -1;
}

int initialize_new_inode(int mode) {
    // allocate new inode of i_mode mode
    int new_inode_num = find_free_inode();
    if (new_inode_num == -1) {
        return -1; // no space left
    }
    

    struct ext2_inode* inode = &inode_table[new_inode_num - 1];
    if (mode == INODE_MODE_FILE) {
        inode->i_mode = EXT2_S_IFREG | 0644; 
        inode->i_size = 0;
        inode->i_blocks = 0;
    } 
    else if (mode == INODE_MODE_DIR) {
        inode->i_mode = EXT2_S_IFDIR | 0755; // allow owners to read, write, exec while others can only read and exec
        inode->i_size = EXT2_BLOCK_SIZE;
        inode->i_blocks = EXT2_BLOCK_SIZE / 512; // i_blocks reflect actual disk sectors
    } 
    else {
        // mode == INODE_MODE_LINK at this point
        inode->i_mode = EXT2_S_IFLNK | 0777;
        inode->i_size = 0;
        inode->i_blocks = 0;
    }
    
    
    // initalize all block pointers to 0
    memset(inode->i_block, 0, 15 * sizeof(unsigned int));
    return new_inode_num;
}

int find_free_block() {
    for (uint32_t i = 0; i < sb->s_blocks_count; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!((block_bitmap[byte_idx] >> bit_idx) & 1)) {
            // block is available
            // mark block is in use
            block_bitmap[byte_idx] |= (1 << bit_idx);
            gd->bg_free_blocks_count--;
            sb->s_free_blocks_count--;

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

void clear_inode_data_blocks(int inode_num) {
    // clear all data blocks of the inode with number inode_num

    struct ext2_inode* inode = &inode_table[inode_num - 1];

    if (is_inode_to_dir(inode_num)) {
        // clear 15 direct pointers
        for (int i = 0; i < 15; i++) {
            if (inode->i_block[i] > 0) {
                // free this block
                memset(disk + inode->i_block[i] * EXT2_BLOCK_SIZE, 0, EXT2_BLOCK_SIZE);
                release_block(inode->i_block[i]);
                inode->i_block[i] = 0;
            }
        }
        // reset metadata after clearing all blocks
        // for directory, there are always 2 entries "." and ".."
        inode->i_size = EXT2_BLOCK_SIZE;
        inode->i_blocks = EXT2_BLOCK_SIZE / 512;
    }
    else if (is_inode_to_file(inode_num)) {
        // clear 14 direct pointers
        for (int i = 0; i < 14; i++) {
            if (inode->i_block[i] > 0) {
                // free this block
                memset(disk + inode->i_block[i] * EXT2_BLOCK_SIZE, 0, EXT2_BLOCK_SIZE);
                release_block(inode->i_block[i]);
                inode->i_block[i] = 0;
            }
        }

        // clear indirect pointer if valid
        if (inode->i_block[14] != 0) {
            // free indirect table
            int indirect_block_num = inode->i_block[14];
            uint32_t* indirect_table = disk + indirect_block_num * EXT2_BLOCK_SIZE;
            int max_indirect_blocks = EXT2_BLOCK_SIZE / sizeof(uint32_t);
            for (int i = 0; i < max_indirect_blocks; i++) {
                int block_num = indirect_table[i];
                if (block_num > 0) {
                    memset(disk + block_num * EXT2_BLOCK_SIZE, 0, EXT2_BLOCK_SIZE);
                    release_block(block_num);
                }
                
            }
            memset(disk + indirect_block_num * EXT2_BLOCK_SIZE, 0, EXT2_BLOCK_SIZE);
            release_block(indirect_block_num);
            inode->i_block[14] = 0;
        }
        // reset metadata after clearing all blocks
        inode->i_size = 0;
        inode->i_blocks = 0;
    }
    
}

// path validation
int validate_path_exists(const char* path) {
    /*
    Return value interpretation:
    0: path exists
    -1: last name of path does not exist
    -2: intermediate folder does not exist or exists as a file 
    */
    // create a copy of path since strtok modifies the string
    size_t path_len = strlen(path);
    char path_copy[path_len + 1];
    strncpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';
    const char* delimiter = "/";
    char* saveptr;
    char* token = strtok_r(path_copy, delimiter, &saveptr);
    int current_inode_num = EXT2_ROOT_INO;
    char* next_token;
    while (token != NULL) {
        next_token = strtok_r(NULL, delimiter, &saveptr);
        // check if this token is a valid entry of current parent dir
        int child_inode = get_child_inode_num(current_inode_num, token);

        // there exists a dir entry with same name as token
        if (next_token != NULL) {
            if (child_inode == -1){
                return -2;
            }
            // check if inode is pointing to a directory
            if (!is_inode_to_dir(child_inode)) {
                // this means token is not the last name of the path
                // but token is not a directory 
                return -2; // path is invalid
            }
        }
        else {
            if (child_inode == -1) {
                return -1;
            }
        }
        
        token = next_token; // move on to the next token
        // update current_inode_num
        current_inode_num = child_inode;
    }
    // path passes all the invalidation check
    return 0;
}

void traverse_path(const char* path, int* parent_inode, int* child_inode) {
    /*
    This function assumes path is a valid path.
    parent_inode points to the inode of the immediate parent directory
    child_inode points to the inode of the last name of the path
    */
    size_t path_len = strlen(path);
    char path_copy[path_len + 1];
    strncpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';
    const char* delimiter = "/";
    char* saveptr;
    char* token = strtok_r(path_copy, delimiter, &saveptr);
    char* next_token;
    int current_inode_num = EXT2_ROOT_INO;
    int parent_inode_num = current_inode_num;
    while (token != NULL) {
        next_token = strtok_r(NULL, delimiter, &saveptr);
        int child_inode_num = get_child_inode_num(current_inode_num, token);
        if (next_token == NULL) {
            // save immediate parent directory
            *parent_inode = parent_inode_num;
            // token now holds the last name of the path
            *child_inode = child_inode_num;
        }
        parent_inode_num = current_inode_num;
        token = next_token;
        current_inode_num = child_inode_num;
    }
}

char* get_path_to_parent(const char* path) {
    // return path to the parent directory
    // useful when the last name of path is not an existing file
    size_t path_len = strlen(path);
    char* path_copy = malloc(path_len + 1);
    if (path_copy == NULL) {
        return NULL;
    }
    strncpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';
    char* last_slash = strrchr(path_copy, '/'); // get last occurence of slash
    if (last_slash != NULL) {
        *last_slash = '\0'; // truncate at the last slash
    }
    return path_copy;
}

int get_child_inode_num(int parent_inode_num, const char* child_name) {
    /*
    Return value interpretation: 
    -1: entry with name child_name not found in parent dir
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
                    return dir_entry->inode;
                }
            }
            offset += dir_entry->rec_len;
        }
    }
    // entry with name child_name is not found in parent dir at this point
    return -1;
}

bool is_inode_to_dir(int inode_num) {
    // check whether inode with number inode_num points to a directory
    if (inode_num <= 0) {
        return false;
    }

    struct ext2_inode* inode = &inode_table[inode_num - 1]; // inode starts from 1
    
    return inode->i_mode == EXT2_FT_DIR; 
}

bool is_inode_to_file(int inode_num) {
    // check whether inode with number inode_num points to a file
    if (inode_num <= 0) {
        return false;
    }

    struct ext2_inode* inode = &inode_table[inode_num - 1];

    return inode->i_mode == EXT2_FT_REG_FILE;
}

bool is_inode_to_symlink(int inode_num) {
    // check whether inode with number inode_num points to a symlink
    if (inode_num <= 0) {
        return false;
    }

    struct ext2_inode* inode = &inode_table[inode_num - 1];

    return inode->i_mode == EXT2_FT_SYMLINK;
}

bool has_space_in_parent_last_used_block(int parent_inode_num, const char* new_dir_name) {

    
    // calculate amount of space needed for a new dir entry with name = new_dir_name
    int metadata_bytes = 8; // 4 bytes for inode, 2 bytes for rec_len, 1 byte for name_len, 1 byte for file_type
    int name_len = (int) strlen(new_dir_name);
    int new_padding = ((name_len + metadata_bytes) % 4 == 0) ? 0 : (4 - ((name_len + metadata_bytes) % 4));
    int new_entry_size = name_len + metadata_bytes + new_padding;

    struct ext2_inode *parent_inode = &inode_table[parent_inode_num - 1];

    // find the last used block
    int last_block_idx = -1;
    for (int i = 0; i < 15; i++) {
        if (parent_inode->i_block[i] != 0) {
            last_block_idx = i;
        }
    }

    if (last_block_idx == -1) {
        // sanity check since a valid directory must have at least 1 block with . and .. entries
        return false;
    }

    uint32_t last_block_num = parent_inode->i_block[last_block_idx];
    char* block_data = (char*)(disk + last_block_num * EXT2_BLOCK_SIZE);

    // find amount of space used in last block
    int used_space = 0;
    struct ext2_dir_entry* entry = (struct ext2_dir_entry*) (block_data + used_space);
    while (used_space < EXT2_BLOCK_SIZE && entry->rec_len > 0) {
        
        if (is_inode_in_use(entry->inode)) {
            // valid entry
            // note that rec_len = actual entry size of a dir entry except the last dir entry
            // rec_len of the last dir entry is the amount of remaining space until end of block
            int padding = ((entry->name_len + metadata_bytes) % 4 == 0) ? 0 : 4 - ((entry->name_len + metadata_bytes) % 4);
            int actual_entry_size = entry->name_len + metadata_bytes + padding;
            if (used_space + entry->rec_len == EXT2_BLOCK_SIZE) {
                // entry is the last dir_entry
                used_space += actual_entry_size;
                break;
            } else {
                // entry is not the last dir so rec_len = actual_entry_size
                used_space += entry->rec_len;
            }
        } else {
            // invalid entry at this point
            break;
        }
        entry = (struct ext2_dir_entry*) (block_data + used_space);
    }

    return EXT2_BLOCK_SIZE - used_space >= new_entry_size;
}

int allocate_new_block_for_parent(int parent_inode_num) {
    struct ext2_inode *parent_inode = &inode_table[parent_inode_num - 1];
    int next_block_idx = -1;
    for (int i = 0; i < 15; i++) {
        if (parent_inode->i_block[i] == 0) {
            next_block_idx = i; // found an available direct block
            break;
        }
    }

    if (next_block_idx == -1) {
        // shouldn't happen because one of the simplifying assumptions states that directories only need direct pointers, not indirect
        return -1;
    }

    // allocate new available block
    int new_block = -1;
    for (uint32_t i = 0; i < sb->s_blocks_count; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!((block_bitmap[byte_idx] >> bit_idx) & 1)) {
            // block is available
            new_block = i;
            // update metadata
            // update block bitmap
            block_bitmap[new_block / 8] |=  1 << (new_block % 8);

            // update group descriptor
            gd->bg_free_blocks_count--;

            // update super block
            sb->s_free_blocks_count--;
        }
    }

    if (new_block == -1) {
        // no space left
        return -1;
    }
  
    // save block number to i_block[next_block_idx]
    parent_inode->i_block[next_block_idx] = new_block;

    // allocate a new block to the directory so the size of the directory is increased by EXT2_BLOCK_SIZE
    parent_inode->i_size += EXT2_BLOCK_SIZE;

    // logical block can be of different size compared to physical disk sector
    // the i_blocks attribute reflect the number physical disk sector required
    parent_inode->i_blocks += (EXT2_BLOCK_SIZE / 512);

    // zero out new block
    char* new_block_data = (char*) (disk + new_block * EXT2_BLOCK_SIZE);
    memset(new_block_data, 0, EXT2_BLOCK_SIZE);

    return new_block;
}

void add_dir_entry_to_new_block(int parent_inode_num, int new_inode_num, char* dir, int new_block, int file_type) {
    struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *) (disk + new_block * EXT2_BLOCK_SIZE);
    new_entry->inode = new_inode_num;
    new_entry->name_len = strlen(dir);
    strncpy(new_entry->name, dir, strlen(dir));
    new_entry->file_type = file_type;
    // since this is the first entry to a new block
    new_entry->rec_len = EXT2_BLOCK_SIZE;

}

void add_dir_entry_to_last_used_block(int parent_inode_num, int new_inode_num, char* dir, int file_type) {
    // find last used block first
    struct ext2_inode *parent_inode = &inode_table[parent_inode_num - 1];

    // find the next available block
    int last_block_idx = -1;
    for (int i = 0; i < 15; i++) {
        if (parent_inode->i_block[i] != 0) {
            last_block_idx = i;
            
        }
    }

    int block_num = parent_inode->i_block[last_block_idx];

    char* block_data = (struct ext2_dir_entry *) (disk + block_num * EXT2_BLOCK_SIZE);
    int used_space = 0;
    int metadata_bytes = 8; // 4 bytes for inode, 2 bytes for rec_len, 1 byte for name_len, 1 byte for file_type
    struct ext2_dir_entry* entry = (struct ext2_dir_entry*) (block_data + used_space);
    while (used_space < EXT2_BLOCK_SIZE && entry->rec_len > 0) {
        if (is_inode_in_use(entry->inode)) {
            // valid entry
            int padding = ((entry->name_len + metadata_bytes) % 4 == 0) ? 0 : 4 - ((entry->name_len + metadata_bytes) % 4);
            int actual_entry_size = entry->name_len + metadata_bytes + padding;
            if (used_space + entry->rec_len == EXT2_BLOCK_SIZE) {
                // entry is the last dir_entry
                entry->rec_len = actual_entry_size;
                used_space += actual_entry_size;
                entry = (struct ext2_dir_entry*) (block_data + used_space); 
                break;
            } else {
                // entry is not the last dir so rec_len = actual_entry_size
                used_space += entry->rec_len;
            }
        }
        entry = (struct ext2_dir_entry*) (block_data + used_space); 
    }

    // entry is the next available entry in the last used block at this point
    // used space is also the cumulative space used for all previous dir entries in the same block
    entry->inode = new_inode_num;
    entry->name_len = strlen(dir);
    strncpy(entry->name, dir, strlen(dir));
    entry->file_type = file_type;
    entry->rec_len = EXT2_BLOCK_SIZE - used_space;
}