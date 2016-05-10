/*
	* This file initially writes the required data structures on the disk block.
	* This code is submitted by Denil Vira (dvvira), Nitin Tak (ntak), Pooja Routray (proutra), Sumeet Hemant Bhatia (sbhatia3) as a part of CSC 568 - 001 Spring 2015
*/

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>

#include "data_definition.h"
#include "constants.h"

/*
	* This function writes the filesystem superblock on the disk block
	* It takes the file descriptor to which the superblock is to be written and user's input of whether dedupe would be supported or not as input parameters
	* Data block 0: - Free, 1: - Used
*/
int superblock_write(int fd)
{
	ssize_t ret;
	int remBytes;
	uint64_t one = 1;
	int i;
	struct dedupe_super_block sb = {
		.magic_no = MAGIC,
		.number_of_valid_inodes = ROOT_INODE_NUMBER,
		.last_inode_number = ROOT_INODE_NUMBER,
		.all_inodes_on_disk = ROOT_INODE_NUMBER,
		.all_data_blocks[0] = one,
	}; 

	for(i = 1; i < 64; i++)
	{
		sb.all_data_blocks[i] = 0;
	}
	ret = write(fd, &sb, sizeof(sb));
	if (ret != sizeof(sb))
	{
		perror("Writing super block failed");
		return -1;
	}

	printf("Super block written succesfully\n");
	
	/* Padding the rest of the bytes to allocate one data block to the superblock */
	remBytes = 4096 - sizeof(sb);
	ret = lseek(fd, remBytes, SEEK_CUR);

	if (ret == (off_t)-1)
	{
		perror("Padding failed");
		return -1;
	}

	printf("Padding succesful\n");

	return 0;
}

/*
	* This function writes the file system root inode to the disk block
	* It takes the file descriptor to which the root inode is to be written as input parameter
	* Parent Inode# of root directory will be 0
*/
int rootinode_write(int fd)
{
	ssize_t ret;
	struct dedupe_inode root_inode = {
		.mode = S_IFDIR,
		.inode_number = ROOT_INODE_NUMBER,
		.number_blocks = 0,
		.is_valid = 1,
		.valid_file_count = 0,
		.all_file_count = 0,
		.parent_inode_number = 0,
	};

	ret = write(fd, &root_inode, sizeof(root_inode));

	if (ret != sizeof(root_inode))
	{
		perror("Writing root inode failed");
		return -1;
	}

	printf("root inode written succesfully\n");
	return 0;
}

int main(int argc, char *argv[])
{
	int fd;

	if (argc != 2) 
	{
		printf("Usage: mkfs-simplefs <device>\n");
		return -1;
	}

	fd = open(argv[1], O_RDWR);
	if (fd == -1) 
	{
		perror("Opening device failed");
		return -1;
	}

	superblock_write(fd);
	rootinode_write(fd);

	close(fd);
	return 0;
}