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
int find_free_block();
void release_block(int block_num); 
void release_inode(int inode_num);

int get_child_dir_inode_num(int parent_inode_num, const char* child_name);
#endif