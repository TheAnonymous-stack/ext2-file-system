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


bool has_space_in_parent_last_used_block(uint32_t parent_inode_num, const char* new_dir_name) {

    
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

uint32_t allocate_new_block_for_parent(uint32_t parent_inode_num) {
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

    // update metadata
    // update block bitmap
    block_bitmap[new_block / 8] |=  1 << (new_block % 8);

    // update group descriptor
    gd->bg_free_blocks_count--;

    // update super block
    sb->s_free_blocks_count--;


    return new_block;
}

int initialize_inode() {
    // create a new dir with name = dir
    uint32_t new_inode_num = find_free_inode();
    if (new_inode_num == -1) {
        return -1; // no space left
    }
    // update metadata
    // update inode bitmap
    inode_bitmap[(new_inode_num - 1) / 8] |= 1 << ((new_inode_num - 1) % 8);

    // update group descriptor
    gd->bg_free_inodes_count--;

    // update superblock
    sb->s_free_inodes_count--;
    struct ext2_inode* inode = &inode_table[new_inode_num - 1];
    inode->i_mode = EXT2_S_IFDIR | 0755; // owner can read write exec, non-owner can read and execute
    inode->i_size = EXT2_BLOCK_SIZE;
    inode->i_blocks = EXT2_BLOCK_SIZE / 512; // i_blocks reflect actual disk sectors
    
    // initalize all block pointers to 0
    memset(inode->i_block, 0, 15 * sizeof(unsigned int));
    return new_inode_num;
}
     
void add_dir_entry_to_new_block(uint32_t parent_inode_num, uint32_t new_inode_num, char* dir, uint32_t new_block) {
    struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *) (disk + new_block * EXT2_BLOCK_SIZE);
    new_entry->inode = new_inode_num;
    new_entry->name_len = strlen(dir);
    strncpy(new_entry->name, dir, strlen(dir));
    new_entry->file_type = EXT2_FT_DIR;
    // since this is the first entry to a new block
    new_entry->rec_len = EXT2_BLOCK_SIZE;

}

void add_dir_entry_to_last_used_block(uint32_t parent_inode_num, uint32_t new_inode_num, char* dir) {
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
    entry->file_type = EXT2_FT_DIR;
    entry->rec_len = EXT2_BLOCK_SIZE - used_space;
}

int initialize_dir_entry(uint32_t new_inode_num, uint32_t parent_inode_num) {
    int new_dir_block = find_free_block();
    if (new_dir_block == -1) {
        return -1;
    }
    // update bookkeeping structures to reserve this block
    block_bitmap[new_dir_block / 8] |= 1 << (new_dir_block % 8);
    gd->bg_free_blocks_count--;
    sb->s_free_blocks_count--;

    struct ext2_inode* inode = &inode_table[new_inode_num - 1];
    inode->i_block[0] = new_dir_block;
    // zero out block
    memset(disk + new_dir_block, 0, EXT2_BLOCK_SIZE);

    // create "." and ".." entries
    struct ext2_dir_entry* dot_entry = (struct ext2_dir_entry*) (disk + new_dir_block * EXT2_BLOCK_SIZE);
    dot_entry->inode = new_inode_num;
    dot_entry->name_len = 1;
    dot_entry->name[0] = '.';
    dot_entry->file_type = EXT2_FT_DIR;
    dot_entry->rec_len = 12; // 8 bytes for metadata (inode, rec_len, file_type, name_len) and 1 byte for name => 3 bytes padding

    struct ext2_dir_entry* dot_dot_entry = (struct ext2_dir_entry*) (disk + new_dir_block * EXT2_BLOCK_SIZE + dot_entry->rec_len);
    dot_dot_entry->inode = parent_inode_num;
    dot_dot_entry->name_len = 2;
    dot_dot_entry->name[0] = '.';
    dot_dot_entry->name[1] = '.';
    dot_dot_entry->file_type = EXT2_FT_DIR;
    dot_dot_entry->rec_len = EXT2_BLOCK_SIZE - 12;
    return 0;
}

int32_t ext2_fsal_mkdir(const char *path)
{
    /**
     * TODO: implement the ext2_mkdir command here ...
     * the argument path is the path to the directory that is to be created.
     */

    char* normalized_path = get_normalized_path(path);
    
    if (strcmp(normalized_path, "/") == 0) {
        // user attempts to create another root directory => illegal
        free(normalized_path);
        return EEXIST;
    }

    const char* delimiter = "/";
    // tokenize to extract individual directory leading up to directory to be created
    char* dir = strtok(normalized_path, delimiter);
    char* next_dir;
    int current_inode_num = EXT2_ROOT_INO;
    int parent_inode_num;
    while (dir != NULL) {
        next_dir = strtok(NULL, delimiter);
        int child_inode_num = get_child_dir_inode_num(current_inode_num, dir);

        if (child_inode_num == -1) {
            // dir is a regular file
            free(normalized_path);
            return ENOENT;
        }
        if (next_dir == NULL) {
            // dir is directory user wants to create
            // check if dir already exists first
            if (child_inode_num != -2) {
                // this means dir already exists
                free(normalized_path);
                return EEXIST;
            } else {
                // update parent_inode_num
                parent_inode_num = current_inode_num;

                if (!has_space_in_parent_last_used_block(parent_inode_num, dir)) {
                    int new_block = allocate_new_block_for_parent(parent_inode_num);
                    if (new_block == -1) {
                        free(normalized_path);
                        return ENOSPC; // no space left
                    }
                    int new_inode_num = initialize_inode();
                    if (new_inode_num == -1) {
                        // no free inode remaining
                        // free normalized path
                        free(normalized_path);

                        // since parent_inode is updated already, need to restore previous state
                        struct ext2_inode* parent_inode = &inode_table[parent_inode_num - 1];
                        bool found = false;
                        int i = 0;
                        while (!found) {
                            if (parent_inode->i_block[i] == new_block) {
                                parent_inode->i_block[i] = 0;
                                found = true;
                            }
                            i++;
                        }
                        parent_inode->i_size -= EXT2_BLOCK_SIZE;
                        parent_inode->i_blocks -= (EXT2_BLOCK_SIZE / 512);

                        // release allocated block by updating bookkeeping structures
                        release_block(new_block);
                        return ENOSPC; 
                    }
                    int res = initialize_dir_entry(new_inode_num, parent_inode_num);
                    if (res == -1) {
                        // no free block remaining
                        free(normalized_path);

                        // since parent_inode is updated already, need to restore previous state
                        struct ext2_inode* parent_inode = &inode_table[parent_inode_num - 1];
                        bool found = false;
                        int i = 0;
                        while (!found) {
                            if (parent_inode->i_block[i] == new_block) {
                                parent_inode->i_block[i] = 0;
                                found = true;
                            }
                            i++;
                        }
                        parent_inode->i_size -= EXT2_BLOCK_SIZE;
                        parent_inode->i_blocks -= (EXT2_BLOCK_SIZE / 512);

                        // release allocated block and inode
                        release_block(new_block);
                        release_inode(new_inode_num);

                        return ENOSPC; 
                    }
                    add_dir_entry_to_new_block(parent_inode_num, new_inode_num, dir, new_block);

                } else {
                    int new_inode_num = initialize_inode();
                    if (new_inode_num == -1) {
                        free(normalized_path);
                        return ENOSPC; // no free inode remaining
                    }
                    int res = initialize_dir_entry(new_inode_num, parent_inode_num);
                    if (res == -1) {
                        // no free block remaining
                        free(normalized_path);

                        // release allocated inode
                        release_inode(new_inode_num);

                        return ENOSPC; 
                    }
                    // there is space in the last used block
                    add_dir_entry_to_last_used_block(parent_inode_num, new_inode_num, dir);
                }
            }

            
        } else {
            // dir is a directory along the path leading up to the directory to be created
            // check if dir exists
            if (child_inode_num == -2) {
                // this meas a directory along the path does not exist
                free(normalized_path);
                return ENOENT;
            }
            else {
                // save the parent node num before updating current_inode_num
                parent_inode_num = current_inode_num;
                // update current_inode to continue traversing the path
                current_inode_num = child_inode_num;
            }

        }
        dir = next_dir;
    }
    free(normalized_path);
    
    return 0;
}