#ifndef _CONSTANTS_H
#define _CONSTANTS_H


/*
	* This file contains the all the constants required for dedupe file system
	* This code is submitted by Denil Vira (dvvira), Nitin Tak (ntak), Pooja Routray (proutra), Sumeet Hemant Bhatia (sbhatia3) as a part of CSC 568 - 001 Spring 2015
*/
#define MAGIC 								0x47726f757031 		/* Hex Representation of the text Group1 */
#define MAX_PATH_LEN						246
#define TWENTY								20
#define INODE_CACHE_START					1
#define ROOT_INODE_NUMBER 					1
#define START_DATA_BLOCK_NUMBER 			40
#define MAX_DIR_RECORD_PER_BLOCK	 		15
#define MAX_INODES_SUPPORTED				180
#define SUPERBLOCK_BLOCK_NUMBER 			0
#define MAX_BLOCKS							4096
#define INODE_CACHE_SIZE					10
#define MAX_INODES_PER_BLOCK				18 
#define MAX_DEDUPE_RECORD_PER_BLOCK			100
#define DEDUPE_CACHE_START					11
#define DEDUPE_CACHE_SIZE					29
#define DEDUPEFS_BLOCK_SIZE					4096
#define HASH_LENGTH							21

#endif