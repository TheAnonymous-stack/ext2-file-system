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

int initialize_dir_entry(uint32_t new_inode_num, uint32_t parent_inode_num) {
    int new_dir_block = find_free_block();
    if (new_dir_block == -1) {
        return -1;
    }

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
void restore_parent_inode(int parent_inode_num, int new_block) {
    /*
    This function is called when the parent inode uses a new allocated data block 
    but either the inode or directory initialization failed so parent_inode must 
    mark the allocated block as not in use
    */
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

}

void handle_failed_initialize_new_inode(char* normalized_path, int parent_inode_num, int new_block) {
    // no free inode remaining
    // free normalized path
    free(normalized_path);
    
    restore_parent_inode(parent_inode_num, new_block);
    
    // release allocated block by updating bookkeeping structures
    release_block(new_block);
}

void handle_failed_initalize_dir_entry(char* normalized_path, int parent_inode_num, int new_inode_num, int new_block) {
    // no free block remaining
    free(normalized_path);

    restore_parent_inode(parent_inode_num, new_block);

    // release allocated block and inode
    release_block(new_block);
    release_inode(new_inode_num);

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

    int path_validation_res = validate_path_exists(normalized_path);
    if (path_validation_res == -2) {
        // an intermediate folder does not exist or exists as a file
        free(normalized_path);
        return ENOENT;
    }
    if (path_validation_res == 0) {
        // a directory or file with the same name already exists
        free(normalized_path);
        return EEXIST;
    }
    
    // traverse the path to get to the parent dir
    const char* delimiter = "/";
    size_t path_len = strlen(normalized_path);
    char path_copy[path_len + 1];
    strncpy(path_copy, normalized_path, path_len);
    path_copy[path_len] = '\0';
    char* saveptr;
    char* token = strtok_r(path_copy, delimiter, &saveptr);
    int current_inode_num = EXT2_ROOT_INO;
    int parent_inode_num = current_inode_num;
    char dir_name[EXT2_NAME_LEN + 1];
    while (token != NULL) {
        int child_inode_num = get_child_inode_num(current_inode_num, token);
        if (child_inode_num == -1) {
            // save name of directory to be created then break
            // verify valid name length first
            if (strlen(token) > EXT2_NAME_LEN) {
                free(normalized_path);
                return ENAMETOOLONG;
            }

            strncpy(dir_name, token, strlen(token));
            dir_name[strlen(token)] = '\0';
            break;
        }
        parent_inode_num = current_inode_num;
        current_inode_num = child_inode_num;
        token = strtok_r(NULL, delimiter, &saveptr);
    }

    // parent_inode_num now holds the inode number of immediate parent directory
    if (!has_space_in_parent_last_used_block(parent_inode_num, dir_name)) {
        int new_block = allocate_new_block_for_parent(parent_inode_num);
        if (new_block == -1) {
            free(normalized_path);
            return ENOSPC; // no space left
        }

        int new_inode_num = initialize_new_inode(INODE_MODE_DIR);
        if (new_inode_num == -1) {
            handle_failed_initialize_new_inode(normalized_path, parent_inode_num, new_block);
            return ENOSPC; 
        }

        int res = initialize_dir_entry(new_inode_num, parent_inode_num);
        if (res == -1) {
            handle_failed_initalize_dir_entry(normalized_path, parent_inode_num, new_inode_num, new_block);
            return ENOSPC; 
        }
        add_dir_entry_to_new_block(parent_inode_num, new_inode_num, dir_name, new_block, EXT2_FT_DIR);
    }
    else {
        // there is enough space in parent's last used block
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
        add_dir_entry_to_last_used_block(parent_inode_num, new_inode_num, dir_name, EXT2_FT_DIR);
    }

    free(normalized_path);
    
    return 0;
}