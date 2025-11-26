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

#ifndef CSC369_E2FS_H
#define CSC369_E2FS_H

#include "ext2.h"
#include <string.h>
#include <stdbool.h>
/**
 * TODO: add in here prototypes for any helpers you might need.
 * Implement the helpers in e2fs.c
 */
char* get_normalized_path(const char* path);
bool is_inode_in_use(int inode_num);
int find_free_inode();
int initialize_new_inode(int mode);
int find_free_block();
void release_block(int block_num); 
void release_inode(int inode_num);
void clear_inode_data_blocks(int inode_num);
int validate_path_exists(const char* path);
void traverse_path(const char* path, int* parent_inode, int* child_inode);
char* get_path_to_parent(const char* path);

int get_child_inode_num(int parent_inode_num, const char* child_name);
bool is_inode_to_dir(int inode_num);
bool is_inode_to_file(int inode_num);
bool is_inode_to_symlink(int inode_num);

bool has_space_in_parent_last_used_block(int parent_inode_num, const char* new_dir_name);
int allocate_new_block_for_parent(int parent_inode_num);
void add_dir_entry_to_new_block(int parent_inode_num, int new_inode_num, char* dir, int new_block, int file_type);
void add_dir_entry_to_last_used_block(int parent_inode_num, int new_inode_num, char* dir, int file_type);


#endif