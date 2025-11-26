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

int copy_file_to_parent_dir(int parent_inode_num, int new_inode_num, FILE* src_file) {
    // copy data from src file to data blocks

    // get size of src file first
    fseek(src_file, 0, SEEK_END);
    long src_size = ftell(src_file);
    fseek(src_file, 0, SEEK_SET);

    int blocks_needed = (src_size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE; // round up number of blocks

    // allocate data blocks and copy data
    struct ext2_inode* inode = &inode_table[new_inode_num - 1];
    size_t bytes_written = 0;

    for (int i = 0; i < blocks_needed && i < 14; i++) {
        // allocate a data block
        int block_num = find_free_block();
        if (block_num == -1) {
            // no free blocks left
            clear_inode_data_blocks(new_inode_num);
            release_inode(new_inode_num);
            fclose(src_file);
            return ENOSPC;
        }

        // set block pointer in inode
        inode->i_block[i] = block_num;

        // copy data to this block
        size_t bytes_to_copy = (src_size - bytes_written > EXT2_BLOCK_SIZE) ? EXT2_BLOCK_SIZE : (src_size - bytes_written);

        // get pointer to the data block
        unsigned char* block_ptr = disk + (block_num * EXT2_BLOCK_SIZE);

        // read bytes_to_copy bytes from src_file to block_ptr
        // fread() will automatically increment the file descriptor src_file
        size_t bytes_read = fread(block_ptr, 1, bytes_to_copy, src_file);

        if (bytes_read != bytes_to_copy) {
            // error reading from src file
            fclose(src_file);
            clear_inode_data_blocks(new_inode_num);
            release_inode(new_inode_num);
            
            return EIO; // I/O error
        }

        bytes_written += bytes_to_copy;   
    }

    if (bytes_written < src_size) {
        // direct pointers are not enough to copy the src file
        // use the indirect pointer

        int indirect_block_num = find_free_block();
        if (indirect_block_num == -1) {
            fclose(src_file);
            clear_inode_data_blocks(new_inode_num);
            release_inode(new_inode_num);
            return ENOSPC;
        }

        inode->i_block[14] = indirect_block_num;
        uint32_t* indirect_table = disk + indirect_block_num * EXT2_BLOCK_SIZE;

        size_t bytes_needed = src_size - bytes_written;
        int blocks_needed = (bytes_needed + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;
        int max_indirect_blocks = EXT2_BLOCK_SIZE / sizeof(uint32_t);

        for (int i = 0; i < blocks_needed && i < max_indirect_blocks; i++) {
            int block_num = find_free_block();
            if (block_num == -1) {
                // no space avaialble
                clear_inode_data_blocks(new_inode_num);
                release_inode(new_inode_num);
                fclose(src_file);
                return ENOSPC;
            }

            indirect_table[i] = block_num;

            // get pointer to block
            unsigned char* block_ptr = disk + (block_num * EXT2_BLOCK_SIZE);
            size_t bytes_to_copy = (src_size - bytes_written > EXT2_BLOCK_SIZE) ? EXT2_BLOCK_SIZE : (src_size - bytes_written);
            size_t bytes_read = fread(block_ptr, 1, bytes_to_copy, src_file);
            if (bytes_read != bytes_to_copy) {
                // I/O error
                clear_inode_data_blocks(new_inode_num);
                release_inode(new_inode_num);
                fclose(src_file);
                return EIO;
            }

            bytes_written += bytes_to_copy;
        }
    }
    inode->i_size = src_size;
    inode->i_blocks = ((src_size + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE) * (EXT2_BLOCK_SIZE / 512);
    return 0;
}

int add_file_as_parent_dir_entry(int parent_inode_num, int new_inode_num, FILE* src_file, char* filename){
    // add directory entry to parent directory
    if (!has_space_in_parent_last_used_block(parent_inode_num, filename)) {
        // allocate new block for parent directory
        int new_parent_block = allocate_new_block_for_parent(parent_inode_num);
        if (new_parent_block == -1) {
            // no space left
            clear_inode_data_blocks(new_inode_num);
            release_inode(new_inode_num);
            fclose(src_file);
            return ENOSPC;
        }
        add_dir_entry_to_new_block(parent_inode_num, new_inode_num, filename, new_parent_block, EXT2_FT_REG_FILE);
    } 
    else {
        // last used block of parent dir has enough space
        add_dir_entry_to_last_used_block(parent_inode_num, new_inode_num, filename, EXT2_FT_REG_FILE);
    }
    return 0;
}

int32_t ext2_fsal_cp(const char *src,
                     const char *dst)
{
    /**
     * TODO: implement the ext2_cp command here ...
     * Arguments src and dst are the cp command arguments described in the handout.
     */
    char* normalized_src_path = get_normalized_path(src);
    FILE* src_file = fopen(src, "rb"); // source file is a file on native OS
    if (src_file == NULL) {
        // source file doesn't exist or cannot be opened
        return ENOENT;
    }
    char* last_slash = strrchr(normalized_src_path, '/');
    char src_file_name[strlen(normalized_src_path) + 1];
    size_t src_file_name_len;
    if (last_slash != NULL) {
        src_file_name_len = strlen(last_slash + 1);
        strncpy(src_file_name, last_slash + 1, src_file_name_len);
        
    } else {
        src_file_name_len = strlen(normalized_src_path);
        strncpy(src_file_name, normalized_src_path, src_file_name_len);
    }
    src_file_name[src_file_name_len] = '\0';
    free(normalized_src_path);

    char* normalized_dst_path = get_normalized_path(dst);
    
    int path_validation_res = validate_path_exists(normalized_dst_path);
    
    if (path_validation_res == -2) {
        // an intermediate folder does not exist or exists as a file
        free(normalized_dst_path);
        fclose(src_file);
        return ENOENT;
    }


    if (path_validation_res == -1) {
        // last name of the path does not exist, 
        // so modify the path to the immediate parent directory
        char* path_to_traverse = get_path_to_parent(normalized_dst_path);
        int parent_inode_num;
        int child_inode_num;
        traverse_path(path_to_traverse, &parent_inode_num, &child_inode_num);
        free(path_to_traverse);
        
        // child_inode_num now stores the inode number of the immediate parent directory
        parent_inode_num = child_inode_num; // for semantic and consistency

        char* last_slash = strrchr(normalized_dst_path, '/');
        char* filename = (last_slash != NULL) ? (last_slash + 1) : normalized_dst_path;

        free(normalized_dst_path);

        if (strlen(filename) > EXT2_NAME_LEN) {
            fclose(src_file);
            return ENAMETOOLONG;
        }
        // need to allocate inode for the new file

        int new_inode_num = initialize_new_inode(INODE_MODE_FILE);
        if (new_inode_num == -1) {
            fclose(src_file);
            return ENOSPC;
        }

        int res = copy_file_to_parent_dir(parent_inode_num, new_inode_num, src_file);
        if (res != 0) {
            return res;
        }

        res = add_file_as_parent_dir_entry(parent_inode_num, new_inode_num, src_file, filename);
        if (res != 0) {
            return res;
        }

    }
    else {
        // path_validation_res = 0 at this point
        // this means there exists a file/folder/symlink with same name

        // traverse the path first
        int parent_inode_num;
        int child_inode_num;
        traverse_path(normalized_dst_path, &parent_inode_num, &child_inode_num);

        
        if (is_inode_to_dir(child_inode_num)) {
            // need to allocate inode for the new file

            free(normalized_dst_path);
            int new_inode_num = initialize_new_inode(INODE_MODE_FILE);
            if (new_inode_num == -1) {
                fclose(src_file);
                return ENOSPC;
            }

            int res = copy_file_to_parent_dir(child_inode_num, new_inode_num, src_file);
            if (res != 0) {
                return res;
            }

            res = add_file_as_parent_dir_entry(child_inode_num, new_inode_num, src_file, src_file_name);
            if (res != 0) {
                return res;
            }
        }
        else if (is_inode_to_file(child_inode_num)) {
            // overwrite content
            // delete existing content
            clear_inode_data_blocks(child_inode_num);
            char* last_slash = strrchr(normalized_dst_path, '/');
            char* filename = (last_slash != NULL) ? (last_slash + 1) : normalized_dst_path;

            free(normalized_dst_path);

            if (strlen(filename) > EXT2_NAME_LEN) {
                fclose(src_file);
                return ENAMETOOLONG;
            }
            int res = copy_file_to_parent_dir(parent_inode_num, child_inode_num, src_file);
            if (res != 0) {
                return res;
            }

        } 
        else if (is_inode_to_symlink(child_inode_num)) {
            // overwrite existing symlink
            clear_inode_data_blocks(child_inode_num);

            struct ext2_inode* symlink_inode = &inode_table[child_inode_num - 1];
            symlink_inode->i_mode = EXT2_S_IFREG | 0644;

            char* last_slash = strrch(normalized_dst_path, '/');
            char* filename = (last_slash != NULL) ? (last_slash + 1) : normalized_dst_path;

            free(normalized_dst_path);

            if (strlen(filename) > EXT2_NAME_LEN) {
                fclose(src_file);
                return ENAMETOOLONG;
            }

            int res = copy_file_to_parent_dir(parent_inode_num, child_inode_num, src_file);
            if (res != 0) {
                return res;
            }
        }
    }
      
    fclose(src_file);

    return 0;
}