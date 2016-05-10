#ifndef _DATA_DEFINITION_H
#define _DATA_DEFINITION_H

/*
	* This file contains the implementation of data structures required for dedupe file system
	* This code is submitted by Denil Vira (dvvira), Nitin Tak (ntak), Pooja Routray (proutra), Sumeet Hemant Bhatia (sbhatia3) as a part of CSC 568 - 001 Spring 2015
*/

#include <linux/types.h>
#include "constants.h"

struct dedupe_super_block
{
	uint64_t magic_no;
	uint64_t number_of_valid_inodes; 		/* It will only be in the range of 0 to 180. Because 10 blocks are allocated for inode_cache and each inode is 224 bytes (Theoretical Max). If inode_cache size is increased more # of inodes can then be created */
	uint64_t all_data_blocks[64]; 			/* To keep availability information of 4096 data blocks */
	uint64_t last_inode_number;				/* This field will be incremented by one and assigned as the inde# for a new incoming inode */
	uint64_t all_inodes_on_disk;			/* Count of all valid and invalid inodes on disk. It is required since a assigned inode is never deleted. */
	uint64_t number_of_valid_dedupe_records; /* Total valid records in dedupe store. It will be incremented / decremented on insertion and / or deletion */
	uint64_t all_dedupe_records_on_disk;	 /* Total number of dedupe FS records on disk. It will only be incremented. Never decremented. */
};

/* Inode structure for each inode will be saved inode_cache on disk. * Data block 0: - Free, 1: - Used */
struct dedupe_inode
{
	uint64_t inode_number; 					/* Unique for each inode*/
	uint64_t file_size;						/* Actual file size in bytes. Valid only for regular files */		
	uint64_t valid_file_count;				/* Number of files and directories in this inode. Valid only for directories */
	uint64_t all_file_count;				/* NUmber of valid and invalid file counts. It is required since dir_records are never deleted */
	
	uint64_t associated_data_blocks[TWENTY]; 	/* Maximum 20 blocks per file. This will restrict file size to 80 kB. For a directory there will be 15 records per block*/
	uint64_t parent_inode_number;				/* This is required for delete flows */

	unsigned short number_blocks;				/* Help identify last element in the above array */
	unsigned short is_valid;
	mode_t mode;
};

/* This structure will be stored in the data block associated with directories */
struct dir_record
{
	char fully_qualified_name[MAX_PATH_LEN];
	unsigned short is_valid;				/* This field is used to indicate the validity of this record when inodes are deleted */
	uint64_t inode_number;
};

/* This structure will be stored on the blocks 11 - 40 */
struct dedupe_info
{
	char hash[HASH_LENGTH];					/* 160 bits hash given by SHA-1 which maps to 20 bytes. One additional byte for NULL character */
	unsigned short reference_count;			/* # of data blocks that have the same hash for their data */
	unsigned short is_valid;				/* This field is used to indicate the validity of this record when inodes are deleted */
	uint64_t data_block_number;				/* Data block # associated with this hash */	
};

#endif