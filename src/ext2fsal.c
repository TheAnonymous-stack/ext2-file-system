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
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>

unsigned char *disk;
struct ext2_super_block *sb;
struct ext2_group_desc *gd;
unsigned char *block_bitmap;
unsigned char* inode_bitmap;
struct ext2_inode *inode_table;
pthread_mutex_t inode_bitmap_lock;
pthread_mutex_t datablock_bitmap_lock;
pthread_mutex_t superblock_lock;
pthread_mutex_t group_desc_lock;



void ext2_fsal_init(const char* image)
{
    /**
     * TODO: Initialization tasks, e.g., initialize synchronization primitives used,
     * or any other structures that may need to be initialized in your implementation,
     * open the disk image by mmap-ing it, etc.
     */
    int fd = open(image, O_RDWR);
    disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    sb = (struct ext2_super_block *)(disk + EXT2_BLOCK_SIZE);
    gd = (struct ext2_group_desc *) (disk + 2 * EXT2_BLOCK_SIZE);
    block_bitmap = (unsigned char*) (disk + gd->bg_block_bitmap * EXT2_BLOCK_SIZE);
    inode_bitmap = (unsigned char*) (disk + gd->bg_inode_bitmap * EXT2_BLOCK_SIZE);
    inode_table = (struct ext2_inode *) (disk + gd->bg_inode_table * EXT2_BLOCK_SIZE);

    // initialize sync locks
    pthread_mutex_init(&inode_bitmap_lock, NULL);
    pthread_mutex_init(&datablock_bitmap_lock, NULL);
    pthread_mutex_init(&superblock_lock, NULL);
    pthread_mutex_init(&group_desc_lock, NULL);

}

void ext2_fsal_destroy()
{
    /**
     * TODO: Cleanup tasks, e.g., destroy synchronization primitives, munmap the image, etc.
     */

    // clean up sync locks
    pthread_mutex_destroy(&inode_bitmap_lock);
    pthread_mutex_destroy(&datablock_bitmap_lock);
    pthread_mutex_destroy(&superblock_lock);
    pthread_mutex_destroy(&group_desc_lock);

    // munmap disk image
    munmap(disk, 128 * 1024);
}