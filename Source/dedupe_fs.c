/*
	* This file contains the implementation of file system related operations
	* This code is submitted by Denil Vira (dvvira), Nitin Tak (ntak), Pooja Routray (proutra), Sumeet Hemant Bhatia (sbhatia3) as a part of CSC 568 - 001 Spring 2015

	* Following is the list of generic references
	* 1. http://www.tldp.org/LDP/lki/lki-3.html
	* 2. http://stackoverflow.com/questions/14450133/slab-memory-management
	* 3. http://en.wikipedia.org/wiki/Slab_allocation
	* 4. http://stackoverflow.com/questions/24858424/unable-to-find-new-object-create-with-kmem-cache-create-in-proc-slabinfo
	* 5. http://stackoverflow.com/questions/22370102/difference-between-kmalloc-and-kmem-cache-alloc
	* 6. https://www.cs.princeton.edu/courses/archive/fall04/cos318/precepts/6.pdf (Influenced certain design choices)
	* 7. http://www.cis.syr.edu/~wedu/seed/Labs/Documentation/Minix3/Inode.pdf (Accessing data structures)

	* Following two links extensively helped in creating pluggable kernel module and making certain design decisions
	* 1. http://lxr.free-electrons.com/source/fs/ramfs/
	* 2. https://github.com/psankar/simplefs
*/

#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/statfs.h>
#include <linux/scatterlist.h>
#include <linux/crypto.h>

#include "data_definition.h"
#include "constants.h"
#include "kernel_structures.h"

static struct kmem_cache *dedupefs_slab_inode;	/* This inode is used as a cache for slab memory allocation */

static DEFINE_MUTEX(superblock_lock); /* This lock is used while working with superblock */
static DEFINE_MUTEX(inode_lock); /* This lock is used while working with inodes */
static DEFINE_MUTEX(children_update_lock); /* This lock is used while updating children in a directory */

/*
	*	Function signature refered from http://lxr.free-electrons.com/source/include/linux/fs.h#L1562
*/
int dedupefs_getattr(struct vfsmount *mnt_point, struct dentry *dentry_ptr, struct kstat *kstat_object)
{
	struct dedupe_inode *dedupe_inode;

	BUG_ON(!dentry_ptr);

	dedupe_inode = dentry_ptr->d_inode->i_private;

	if(unlikely(!dedupe_inode))
	{
		printk(KERN_ERR "unlikely code \n");
		return -ENOENT;
	}
		
	
	kstat_object->ino = dedupe_inode->inode_number;
	if(!S_ISDIR(dedupe_inode->mode)) /* indicates file */
	{
		kstat_object->size = dedupe_inode->file_size;
		kstat_object->nlink = 1;
		kstat_object->mode = dedupe_inode->mode | 0644;
	}
	else
	{
		kstat_object->nlink = 2;
		kstat_object->mode = dedupe_inode->mode | 0755;
	}
	kstat_object->atime = dentry_ptr->d_inode->i_atime;
	kstat_object->mtime = dentry_ptr->d_inode->i_mtime;
	kstat_object->ctime = dentry_ptr->d_inode->i_ctime;

	return 0;
}


/* 
	* This function will fecth dedupe FS inode from the superblock
	* The fetched inode will have inode# as specified in second parameter
	* The inode will be returned by the function
*/
struct dedupe_inode* fetch_dedupefs_inode(struct super_block *sb, uint64_t inode_no)
{
	struct dedupe_inode *inode_to_fetch = NULL;
	struct dedupe_super_block *dedupe_sb = (struct dedupe_super_block *)sb->s_fs_info;
	struct buffer_head *buffer = NULL;
	struct dedupe_inode *loop_inode;

	unsigned short i, loop_bound, difference, inode_cache_counter = INODE_CACHE_START;
	uint64_t visited_inodes = 0;
	
	while(visited_inodes < dedupe_sb->all_inodes_on_disk)
	{
		buffer = sb_bread(sb, inode_cache_counter);
		BUG_ON(!buffer);

		loop_inode = (struct dedupe_inode *)buffer->b_data;
		BUG_ON(!loop_inode);

		difference = dedupe_sb->all_inodes_on_disk - visited_inodes;
		loop_bound = difference > MAX_INODES_PER_BLOCK ? MAX_INODES_PER_BLOCK : difference;

		for(i = 0; i < loop_bound; ++i )
		{
			if(loop_inode->is_valid == 1 && loop_inode->inode_number == inode_no )
			{				
				inode_to_fetch = kmem_cache_alloc(dedupefs_slab_inode, GFP_KERNEL);
				memcpy(inode_to_fetch, loop_inode, sizeof(struct dedupe_inode));
				break;								
			}
			++loop_inode;
			++visited_inodes;
		}

		++inode_cache_counter;
		brelse(buffer);	

		if(inode_to_fetch != NULL)
			break;							
	}

	return inode_to_fetch;
}

/*
	* Function signature is referred from: http://lxr.free-electrons.com/source/include/linux/fs.h#L1542
*/
struct dentry *dedupefs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags)
{
	struct dedupe_inode *parent_dedupe = parent_inode->i_private; 
	struct dedupe_inode *child_dedupe;
	struct inode *child_inode;
	struct super_block *sb = parent_inode->i_sb;
	struct buffer_head *buffer;
	struct dir_record *dir_contents;
	int loop_inner, loop_outer, child_count = 0;

	for(loop_outer = 0 ; loop_outer < parent_dedupe->number_blocks; loop_outer++)
	{
		buffer = sb_bread(sb, parent_dedupe->associated_data_blocks[loop_outer]);
		BUG_ON(!buffer);
		dir_contents = (struct dir_record *)buffer->b_data;
		BUG_ON(!dir_contents);
		for(loop_inner = 0; (loop_inner < MAX_DIR_RECORD_PER_BLOCK) && (child_count != parent_dedupe->all_file_count); loop_inner++)
		{
			if((dir_contents->is_valid) && (!strcmp(dir_contents->fully_qualified_name, child_dentry->d_name.name)))
			{
				child_dedupe = (struct dedupe_inode *)fetch_dedupefs_inode(sb, dir_contents->inode_number);
				child_inode = new_inode(sb);
				child_inode->i_ino = dir_contents->inode_number;
				child_inode->i_sb = sb;
				child_inode->i_op = &dedupefs_inode_operations;
				if (S_ISDIR(child_dedupe->mode))
					child_inode->i_fop = &dedupefs_directory_operations;
				else if (S_ISREG(child_dedupe->mode))
					child_inode->i_fop = &dedupefs_file_operations;
				else
					printk(KERN_ERR "Invalid mode");
				child_inode->i_atime = child_inode->i_mtime = child_inode->i_ctime = current_kernel_time();
				child_inode->i_private = child_dedupe;

				inode_init_owner(child_inode, parent_inode, child_dedupe->mode);	
				d_add(child_dentry, child_inode);
				return NULL;
			}
			dir_contents++;
			child_count++;
		}
	}

	return NULL;
}

/*
	* This function syncs the superblock to the disk
*/
void superblock_sync(struct super_block *sb)
{
	struct buffer_head *buffer;
	struct dedupe_super_block *dsb = sb->s_fs_info;

	buffer = sb_bread(sb, SUPERBLOCK_BLOCK_NUMBER);
	BUG_ON(!buffer);

	buffer->b_data = (char *)dsb;
	mark_buffer_dirty(buffer);
	sync_dirty_buffer(buffer);
	brelse(buffer);
}

/*
	* This functions fetches the block number of the next available free block associated with the passed superblock
	* The block number is returned in the second parameter 
*/
int fetch_free_block(struct super_block *sb, uint64_t *out)
{
	struct dedupe_super_block *dsb = sb->s_fs_info;
	int loop_inner, loop_outer, block_multiplier = 0;
	int outer_break_flag = 0; 														/* Used to break out of the outer for loop */
	uint64_t zero = 0, one = 1;

	if (mutex_lock_interruptible(&superblock_lock)) 
	{
		printk(KERN_ERR "Acquiring superblock lock failed\n");
		return -EINTR;
	}

	if((~(~(~zero << START_DATA_BLOCK_NUMBER) | dsb->all_data_blocks[0])) == 0) 	/* Returns true if the first 41-64 blocks are in use */
	{
		
		block_multiplier += 64; 													/* Move to the next 64 blocks */
		for(loop_outer = 1 ; loop_outer < 64 ; loop_outer++)
		{
			if((~(dsb->all_data_blocks[loop_outer])) != 0) 							/* Returns true if the current block is free */
			{
				for (loop_inner = 0; loop_inner < 64; loop_inner++)
				{
					if (!(dsb->all_data_blocks[loop_outer] & (one << loop_inner)))  	/* Returns true on finding a free block */
					{
						dsb->all_data_blocks[loop_outer] |= (one << loop_inner);
						outer_break_flag = 1;
						break;
					}
				}
				if(outer_break_flag)
					break;
			}
			block_multiplier += 64; 												/* Move to the next 64 blocks */
		}
	}
	else
	{
		for (loop_inner = START_DATA_BLOCK_NUMBER; loop_inner < 64; loop_inner++)	
		{
			if (!(dsb->all_data_blocks[0] & (one << loop_inner))) 					/* Returns true on finding a free block */
			{
				dsb->all_data_blocks[0] |= (one << loop_inner);
				break;
			}
		}
	}

	*out = loop_inner + block_multiplier;
	
	superblock_sync(sb);

	mutex_unlock(&superblock_lock);
	return 0;
}

/*
	* This function will save the modified inode passed as second parameter on disk.
	* This is achieved by searching the modified inode on disk and then copying the contents of modified inode to disk
	* Return Values:
		* 0: Success
		* 1: Failure
*/
int save_inode(struct super_block *super_blk, struct dedupe_inode *dedupe_fs_modified_inode)
{
	struct dedupe_inode *current_dedupe_fs_inode, *required_inode_on_disk;
	struct dedupe_super_block *dedupe_sblk;
	struct buffer_head *buffer;
	uint64_t visited_inodes;
	unsigned short i, loop_bound, difference, inode_cache_counter;
	int ret;
	
	inode_cache_counter = INODE_CACHE_START;
	ret = 0;
	visited_inodes = 0;
	required_inode_on_disk = NULL;
	dedupe_sblk = (struct dedupe_super_block *)super_blk->s_fs_info;

	/* visited_inodes - indicates number of visited inodes */
	while(visited_inodes < dedupe_sblk->all_inodes_on_disk)
	{
		buffer = sb_bread(super_blk, inode_cache_counter);
		BUG_ON(!buffer);
		
		current_dedupe_fs_inode = (struct dedupe_inode *)buffer->b_data;
		BUG_ON(!current_dedupe_fs_inode);
		
		difference = dedupe_sblk->all_inodes_on_disk - visited_inodes;
		loop_bound = difference > MAX_INODES_PER_BLOCK ? MAX_INODES_PER_BLOCK : difference;

		for(i = 0; i < loop_bound; ++i )
		{
			if(current_dedupe_fs_inode->is_valid == 1 && current_dedupe_fs_inode->inode_number == dedupe_fs_modified_inode->inode_number)
			{				
				required_inode_on_disk = current_dedupe_fs_inode;
				break;									
			}
			++current_dedupe_fs_inode;	
			++visited_inodes;
		}

		if(required_inode_on_disk != NULL)
			break;

		++inode_cache_counter;
		brelse(buffer);		
	}

	if(required_inode_on_disk != NULL)
	{
		memcpy(required_inode_on_disk, dedupe_fs_modified_inode, sizeof(struct dedupe_inode));
		mark_buffer_dirty(buffer);
		sync_dirty_buffer(buffer);	
		brelse(buffer);	
	}
	else
	{		
		ret = 1;
	}	
	return ret;
}

/*
	* This function adds an inode to a specific superblock
	* The superblock and inode are passed as parameters
*/
void add_inode(struct super_block *sb, struct dedupe_inode *inode)
{
	struct dedupe_super_block *dsb = sb->s_fs_info;
	struct buffer_head *buffer;
	struct dedupe_inode *inode_counter;
	uint64_t all_inodes = dsb->all_inodes_on_disk;
	uint64_t block_to_search = (all_inodes / MAX_INODES_PER_BLOCK) + INODE_CACHE_START; 	/* Since 20 inodes are allowed per block */
	uint64_t last_inode_in_block = all_inodes % MAX_INODES_PER_BLOCK;
	int loop_outer,loop_inner;
	int outer_break_flag = 0; 														/* Used to break out of the outer for loop */
	
	if (mutex_lock_interruptible(&inode_lock))
	{
		printk(KERN_ERR "Acquiring inode lock failed\n");
		return;
	}
	
	if (mutex_lock_interruptible(&superblock_lock)) 
	{
		printk(KERN_ERR "Acquiring superblock lock failed\n");
		return;
	}

	if(dsb->number_of_valid_inodes == dsb->all_inodes_on_disk)						/* Returns true if there are no invalid inodes */
	{
		buffer = sb_bread(sb, block_to_search);
		BUG_ON(!buffer);
		inode_counter = (struct dedupe_inode *)buffer->b_data;
		BUG_ON(!inode_counter);
		inode_counter += last_inode_in_block;
		memcpy(inode_counter, inode, sizeof(struct dedupe_inode));
		
		dsb->all_inodes_on_disk++;

		mark_buffer_dirty(buffer);
		sync_dirty_buffer(buffer);
		brelse(buffer);
	}
	else
	{
		for(loop_outer = INODE_CACHE_START; loop_outer <= INODE_CACHE_SIZE; loop_outer++)
		{
			buffer = sb_bread(sb, loop_outer);
			BUG_ON(!buffer);
			inode_counter = (struct dedupe_inode *)buffer->b_data;
			BUG_ON(!inode_counter);
			for(loop_inner = 0; loop_inner < MAX_INODES_PER_BLOCK; loop_inner++) 				/* Since 20 inodes are allowed per block */
			{
				if(!(inode_counter->is_valid))
				{
					memcpy(inode_counter, inode, sizeof(struct dedupe_inode));
					mark_buffer_dirty(buffer);
					sync_dirty_buffer(buffer);
					outer_break_flag = 1;
					break;
				}
				inode_counter++;
			}
			brelse(buffer);
			if(outer_break_flag)
				break;
		}
	}
	
	mutex_unlock(&inode_lock);
	
	dsb->number_of_valid_inodes++;
	dsb->last_inode_number++;
		
	superblock_sync(sb);
	mutex_unlock(&superblock_lock);
}

/*
	* This is a common function that creates both files and directories
	* Both dedupefs_mkdir(struct inode *, struct dentry *, umode_t) and dedupefs_create(struct inode *, struct dentry *, umode_t, bool) invoke this function
	* 
*/
static int dedupefs_create_object(struct inode *parent_inode, struct dentry *dentry, umode_t mode)
{
	struct super_block *sb;
	struct inode *inode;
	struct buffer_head *buffer;

	struct dedupe_super_block *dsb;	
	struct dedupe_inode *d_inode;
	struct dedupe_inode *parent_d_inode;
	struct dir_record *dir_contents;
	
	uint64_t number_of_objects;
	uint64_t last_inode_num;

	int block_to_search, last_record_in_block,loop_outer, loop_inner, index_associated_block, outer_break_flag, ret, fetch_new_block;

	fetch_new_block = 0;
	outer_break_flag = 0;

	if (mutex_lock_interruptible(&children_update_lock))
	{
		printk(KERN_ERR "Acquiring children update lock failed\n");
		return -EINTR;
	}

	/* Validating mode */
	if (!S_ISDIR(mode) && !S_ISREG(mode)) 
	{
		mutex_unlock(&children_update_lock);
		return -EINVAL;
	}

	/* Fetching the number of objects associated with the superblock */
	sb = parent_inode->i_sb;
	dsb = (struct dedupe_super_block *)sb->s_fs_info;
	if (mutex_lock_interruptible(&inode_lock)) 
	{
		printk(KERN_ERR "Acquiring inode lock failed\n");
		return -EINTR;
	}
	
	last_inode_num = dsb->last_inode_number;
	number_of_objects = dsb->number_of_valid_inodes;
	mutex_unlock(&inode_lock);

	/* Checking if count has reached the maximum number of objects that are supported by the filesystem */
	if (unlikely(number_of_objects == MAX_INODES_SUPPORTED)) 
	{
		printk(KERN_ERR "No more objects are supported by the filesystem");
		mutex_unlock(&children_update_lock);
		return -ENOSPC;
	}

	/* Creating an inode and filling in required information */
	inode = new_inode(sb);
	if (!inode)
	{
		mutex_unlock(&children_update_lock);
		return -ENOMEM;
	}

	parent_d_inode = parent_inode->i_private;

	inode->i_sb = sb;
	inode->i_op = &dedupefs_inode_operations;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_kernel_time();
	inode->i_ino = ++last_inode_num;

	d_inode = kmem_cache_alloc(dedupefs_slab_inode, GFP_KERNEL);
	d_inode->inode_number = inode->i_ino;
	inode->i_private = d_inode;
	d_inode->mode = mode;
	d_inode->parent_inode_number = parent_d_inode->inode_number;

	inode->i_fop = S_ISDIR(mode) ? &dedupefs_directory_operations : &dedupefs_file_operations;

	d_inode->all_file_count = 0;
	d_inode->valid_file_count = 0;
	d_inode->file_size = 0;

	d_inode->is_valid = 1;

	/* Fetching free blocks */
	d_inode->number_blocks = 0;

	/* Checking whether new block need to be fetched or not */
	last_record_in_block = parent_d_inode->all_file_count % MAX_DIR_RECORD_PER_BLOCK ;
	index_associated_block = parent_d_inode->all_file_count / MAX_DIR_RECORD_PER_BLOCK;
	if(parent_d_inode->valid_file_count == parent_d_inode->all_file_count && (index_associated_block + 1) > parent_d_inode->number_blocks)
	{
		if(parent_d_inode->number_blocks == TWENTY)
			return -ENOSPC;
		fetch_new_block = 1;
	}

	/* Adding inode to the superblock */
	add_inode(sb, d_inode);

	/* Adding dir_record to the parent directory*/
	if(parent_d_inode->valid_file_count == parent_d_inode->all_file_count) /* Returns true is all files and directories are valid */
	{
		if(fetch_new_block)
		{
			ret = fetch_free_block(sb, &parent_d_inode->associated_data_blocks[index_associated_block]);
			if (ret < 0) 
			{
				printk(KERN_ERR "Fetching free blocks failed \n");
				mutex_unlock(&children_update_lock);
				return ret;
			}
			else
			{
				printk(KERN_INFO "Fetched free block # %lld \n", parent_d_inode->associated_data_blocks[index_associated_block]);
			}
			parent_d_inode->number_blocks++;
		}
		block_to_search = parent_d_inode->associated_data_blocks[index_associated_block];
		buffer = sb_bread(sb, block_to_search);
		BUG_ON(!buffer);
		dir_contents = (struct dir_record *)buffer->b_data;
		BUG_ON(!dir_contents);
		dir_contents += last_record_in_block;
		
		dir_contents->inode_number = d_inode->inode_number;
		strcpy(dir_contents->fully_qualified_name, dentry->d_name.name);
		dir_contents->is_valid = 1;	

		parent_d_inode->all_file_count++;
		
		mark_buffer_dirty(buffer);
		sync_dirty_buffer(buffer);
		brelse(buffer);
	}
	else
	{
		for(loop_outer = 0; loop_outer < parent_d_inode->number_blocks; loop_outer++)
		{
			buffer = sb_bread(sb, parent_d_inode->associated_data_blocks[loop_outer]);
			BUG_ON(!buffer);
			dir_contents = (struct dir_record *)buffer->b_data;
			BUG_ON(!dir_contents);
			for(loop_inner = 0; loop_inner < MAX_DIR_RECORD_PER_BLOCK; loop_inner++)
			{
				if(!(dir_contents->is_valid))
				{
					dir_contents->inode_number = d_inode->inode_number;
					strcpy(dir_contents->fully_qualified_name, dentry->d_name.name);	
					dir_contents->is_valid = 1;

					mark_buffer_dirty(buffer);
					sync_dirty_buffer(buffer);

					outer_break_flag = 1;
					break;
				}
				dir_contents++;
			}
			brelse(buffer);
			if(outer_break_flag)
				break;
		}
	}	

	if (mutex_lock_interruptible(&inode_lock)) 
	{
		mutex_unlock(&children_update_lock);
		printk(KERN_ERR "Acquiring inode lock failed\n");
		return -EINTR;
	}

	parent_d_inode->valid_file_count++;

	/* Syncing the parent inode to the disk */
	ret = save_inode(sb, parent_d_inode);

	if (ret) 
	{
		/* Undo actions not implemented */
		mutex_unlock(&inode_lock);
		mutex_unlock(&children_update_lock);

		return ret;
	}

	mutex_unlock(&inode_lock);
	mutex_unlock(&children_update_lock);

	inode_init_owner(inode, parent_inode, mode);
	d_add(dentry, inode);

	return 0;
}

/*
	* Function signature referred from: http://lxr.free-electrons.com/source/include/linux/fs.h#L1554
*/
int dedupefs_mkdir(struct inode *parent_inode, struct dentry *dentry, umode_t mode)
{
	return dedupefs_create_object(parent_inode, dentry, S_IFDIR | mode);
}

/*
	* Function signature referred from: http://lxr.free-electrons.com/source/include/linux/fs.h#L1550
*/
int dedupefs_create(struct inode *parent_inode, struct dentry *dentry, umode_t mode, bool excl)
{
	return dedupefs_create_object(parent_inode, dentry, mode);
}

/*
	* Clears directory record associated with the passed parent_inode that with the passed inode_no
*/
void clear_dir_record(struct super_block *sb, struct dedupe_inode *parent_inode, uint64_t inode_no)
{
	int loop_outer, loop_inner;
	int outer_break_flag;
	struct buffer_head *buffer;
	struct dir_record *dir_contents;
	outer_break_flag = 0;

	for(loop_outer = 0; loop_outer < parent_inode->number_blocks; loop_outer++)
	{
		buffer = sb_bread(sb, parent_inode->associated_data_blocks[loop_outer]);
		BUG_ON(!buffer);
		dir_contents = (struct dir_record *)buffer->b_data;
		BUG_ON(!dir_contents);
		for(loop_inner = 0; loop_inner < MAX_DIR_RECORD_PER_BLOCK; loop_inner++)
		{
			if(dir_contents->is_valid == 1 && dir_contents->inode_number == inode_no)
			{	
				memset(dir_contents->fully_qualified_name, 0, MAX_PATH_LEN);
				dir_contents->is_valid = 0;
				dir_contents->inode_number = 0;

				mark_buffer_dirty(buffer);
				sync_dirty_buffer(buffer);

				outer_break_flag = 1;
				break;
			}
			dir_contents++;
		}
		brelse(buffer);
		if(outer_break_flag)
			break;
	}
}

void delete_inode(struct inode *inode, struct inode *parent_inode)
{
	struct dedupe_inode *d_inode;
	struct dedupe_inode *parent_dedupe;
	struct super_block *sb;
	struct dedupe_super_block *dsb;

	if (mutex_lock_interruptible(&superblock_lock)) 
	{
		printk(KERN_ERR "Acquiring superblock lock failed\n");
	}

	if (mutex_lock_interruptible(&inode_lock)) 
	{
		printk(KERN_ERR "Acquiring inode lock failed\n");
	}

	d_inode = (struct dedupe_inode *)inode->i_private;
	sb = inode->i_sb;
	dsb = (struct dedupe_super_block *)sb->s_fs_info;

	parent_dedupe = (struct dedupe_inode *)parent_inode->i_private;//fetch_dedupefs_inode(sb, d_inode->parent_inode_number);
	parent_dedupe->valid_file_count--;
	clear_dir_record(sb, parent_dedupe, d_inode->inode_number);
	
	save_inode(sb, parent_dedupe);

	d_inode->is_valid = 0;
	d_inode->number_blocks = 0;
	d_inode->file_size = 0;
	d_inode->valid_file_count = 0;
	d_inode->all_file_count = 0;
	d_inode->parent_inode_number = 0;

	save_inode(sb, d_inode);

	dsb->number_of_valid_inodes--;

	superblock_sync(sb);

	mutex_unlock(&inode_lock);
	mutex_unlock(&superblock_lock);
}

/*
	* This function clears the data block referred to by the passed data_block_number and frees the block for future use
*/
void clear_free_datablock(struct super_block *sb, uint64_t data_block_number)
{
	struct buffer_head *buffer;
	void *block_content;
	int array_index, block_in_index;
	struct dedupe_super_block *dsb;
	uint64_t one;
	
	dsb = (struct dedupe_super_block *)sb->s_fs_info;
	one = 1;
	
	/* clearing data block */
	buffer = sb_bread(sb, data_block_number);
	BUG_ON(!buffer);
	block_content = buffer->b_data;
	BUG_ON(!block_content);
	
	memset(block_content, 0, DEDUPEFS_BLOCK_SIZE);
	
	mark_buffer_dirty(buffer);
	sync_dirty_buffer(buffer);
	brelse(buffer);

	/* freeing data block */
	array_index = data_block_number / 64;
	block_in_index = data_block_number % 64;

	dsb->all_data_blocks[array_index] &= ~(one << block_in_index);

	superblock_sync(sb);
}

/*
	* Function signature referred from: http://lxr.free-electrons.com/source/include/linux/fs.h#L1555
*/
int dedupefs_rmdir(struct inode *parent_inode, struct dentry *dentry)
{
	int loop_var;
	struct super_block *sb; 
	struct dedupe_inode *d_inode;

	sb = (struct super_block *)dentry->d_inode->i_sb;
	d_inode = (struct dedupe_inode *)dentry->d_inode->i_private;

	if (mutex_lock_interruptible(&superblock_lock)) 
	{
		printk(KERN_ERR "Acquiring superblock lock failed\n");
		return -EINTR;
	}

	if(d_inode->valid_file_count != 0)
	{
		printk(KERN_ERR "Directory not empty \n");
		return -ENOTEMPTY;
	}

	for(loop_var = 0; loop_var < d_inode->number_blocks; loop_var++)
	{
		clear_free_datablock(sb, d_inode->associated_data_blocks[loop_var]);
		d_inode->associated_data_blocks[loop_var] = 0;
	}

	mutex_unlock(&superblock_lock);
	//delete_inode(dentry->d_inode);
	delete_inode(dentry->d_inode, parent_inode);
	d_drop(dentry);
	return 0;
}

/*
	* The function decrements the reference count of the dedupe record corresponding to the passed data_block_no. If the reference count becomes 0, the dedupe record is cleaned
*/
void decrement_dedupe_ref(struct super_block *sb, uint64_t data_block_no)
{
	struct dedupe_info *dedupe_map;
	struct buffer_head *buffer;
	struct dedupe_super_block *dsb = sb->s_fs_info;
	int loop_outer, loop_inner;
	int outer_break_flag = 0;

	for(loop_outer = DEDUPE_CACHE_START; loop_outer < DEDUPE_CACHE_SIZE; loop_outer++)
	{
		buffer = sb_bread(sb, loop_outer);
		BUG_ON(!buffer);
		dedupe_map = (struct dedupe_info *)buffer->b_data;
		BUG_ON(!dedupe_map);

		for(loop_inner = 0; loop_inner < MAX_DEDUPE_RECORD_PER_BLOCK; loop_inner++)
		{
			if(dedupe_map->is_valid == 1 && dedupe_map->data_block_number == data_block_no)
			{
				dedupe_map->reference_count--;
				if(dedupe_map->reference_count == 0)
				{
					clear_free_datablock(sb, data_block_no);
					memset(dedupe_map->hash, 0, HASH_LENGTH);
					dedupe_map->is_valid = 0;
					dedupe_map->data_block_number = 0;
					mark_buffer_dirty(buffer);
					sync_dirty_buffer(buffer);
					brelse(buffer);

					dsb->number_of_valid_dedupe_records--;
					superblock_sync(sb);
				}
				else
				{
					mark_buffer_dirty(buffer);
					sync_dirty_buffer(buffer);
					brelse(buffer);
				}

				outer_break_flag = 1;
				break;
			}
			dedupe_map++;
		}
		if(outer_break_flag)
			break;
		brelse(buffer);
	}
}

/*
	* Function signature referred from: http://lxr.free-electrons.com/source/include/linux/fs.h#L1552
*/
int dedupefs_unlink(struct inode *parent_inode, struct dentry *dentry)
{
	struct inode *inode; 
	struct super_block *sb;
	struct dedupe_inode *d_inode;
	int loop_var;

	inode = (struct inode *)dentry->d_inode;
	sb = (struct super_block *)inode->i_sb;
	d_inode = (struct dedupe_inode *)inode->i_private;
	
	if (mutex_lock_interruptible(&superblock_lock)) 
	{
		printk(KERN_ERR "Acquiring superblock lock failed\n");
		return -EINTR;
	}

	for(loop_var = 0; loop_var < d_inode->number_blocks; loop_var++)
	{
		decrement_dedupe_ref(sb, d_inode->associated_data_blocks[loop_var]);
		d_inode->associated_data_blocks[loop_var] = 0;
	}

	mutex_unlock(&superblock_lock);
 	delete_inode(dentry->d_inode, parent_inode);
 	d_drop(dentry);
	return 0;
}

/*
* This function is used to read the content on disk and pass it on to user
* Function defintion and signature refered from : http://lxr.free-electrons.com/source/include/linux/fs.h#L1507
*/
ssize_t dedupefs_read(struct file *filp, char __user *user_buffer, size_t read_size, loff_t *poff)
{
	struct dedupe_inode *dedupe_fs_inode;
	struct buffer_head *buffer;
	char *saved_content;	
	int bytes_to_copy, loop_var;

	buffer = NULL;
	dedupe_fs_inode = (struct dedupe_inode *)filp->f_inode->i_private;
	saved_content = (char *)kmalloc(dedupe_fs_inode->file_size, GFP_KERNEL);
	memset(saved_content, 0 , dedupe_fs_inode->file_size);

	if(*poff >= dedupe_fs_inode->file_size)
	{
		return 0;
	}

	for(loop_var = 0; loop_var < dedupe_fs_inode->number_blocks; loop_var++)
	{
		buffer = sb_bread(filp->f_inode->i_sb, dedupe_fs_inode->associated_data_blocks[loop_var]);

		BUG_ON(!buffer);
		strcat(saved_content,(char *)buffer->b_data);

		brelse(buffer);
	}

	saved_content += *poff;

	bytes_to_copy = min((size_t)dedupe_fs_inode->file_size, read_size);

	if(copy_to_user(user_buffer, saved_content,bytes_to_copy))
	{
		printk(KERN_ERR "Failed to copy the contents to user space buffer \n");
		return -EFAULT;
	}
	
	saved_content -= *poff;
	kfree(saved_content);

	*poff += bytes_to_copy;
	return bytes_to_copy;
}

/*
	* This function takes a super block and checks if the second parameter equals to any hash value in the dedupe store
	* If a matching hash is found it increments the reference count and updates the corresponding disk contents
	* Return Value
	* 1: Block exists in dedupe store
	* 0: Block does not exist in dedupe store
*/
int hash_check_and_update(struct super_block *sb, char *hash_value, int to_increment)
{
	uint64_t visited_dedupe_records, block_to_read;
	struct buffer_head *buffer;
	struct dedupe_info *dedupe_store;
	int loop_counter, loop_bound, difference, break_loop;
	struct dedupe_super_block *dedup_sb;

	visited_dedupe_records = 0;
	block_to_read = DEDUPE_CACHE_START;
	buffer = NULL;
	dedupe_store = NULL;
	loop_counter = loop_bound = difference = break_loop = 0;
	dedup_sb = (struct dedupe_super_block *)sb->s_fs_info;

	while(visited_dedupe_records < dedup_sb->all_dedupe_records_on_disk)
	{
		buffer = sb_bread(sb, block_to_read);
		BUG_ON(!buffer);

		dedupe_store = (struct dedupe_info *)buffer->b_data;
		BUG_ON(!dedupe_store);

		difference = dedup_sb->all_dedupe_records_on_disk - visited_dedupe_records;
		loop_bound = difference > MAX_DEDUPE_RECORD_PER_BLOCK ? MAX_DEDUPE_RECORD_PER_BLOCK : difference;
		for(loop_counter = 0; loop_counter < loop_bound; ++loop_counter)
		{
			if(dedupe_store->is_valid == 1 && strncmp(dedupe_store->hash, hash_value, TWENTY) == 0) /*TODO: Check if strncmp is compiled as a kernel module */
			{
				break_loop = dedupe_store->data_block_number;				
				break;
			}
			++dedupe_store;
			++visited_dedupe_records;
		}
		
		if(break_loop != 0)
			goto update;
		
		brelse(buffer);
		++block_to_read;		
	}

	return break_loop;

update:
	dedupe_store->reference_count = (to_increment == 1) ? dedupe_store->reference_count + 1 : dedupe_store->reference_count - 1;
	if(dedupe_store->reference_count == 0)
	{				
		dedupe_store->is_valid = 0;
		memset(dedupe_store->hash, 0, HASH_LENGTH);				
		
		/* So that no lock occurs due to two sb_bread */
		mark_buffer_dirty(buffer);
		sync_dirty_buffer(buffer);
		brelse(buffer);

		dedup_sb->number_of_valid_dedupe_records--;
		superblock_sync(sb);
	}
	else
	{
		mark_buffer_dirty(buffer);
		sync_dirty_buffer(buffer);
		brelse(buffer);	
	}

	return break_loop;
}

void compute_hash(char *user_content, unsigned char *hash, size_t len)
{
	struct scatterlist slist;
	struct hash_desc desc;

	sg_init_one(&slist, user_content, len);
	desc.tfm = crypto_alloc_hash("sha1", 0, CRYPTO_ALG_ASYNC);

	crypto_hash_init(&desc);
	crypto_hash_update(&desc, &slist, len);
	crypto_hash_final(&desc, hash);

	crypto_free_hash(desc.tfm);	
}

void add_record_dedupe_store(struct super_block *super_blk, char *hash, uint64_t data_blk)
{
	struct buffer_head *buffer;
	struct dedupe_info *dedupe;
	
	int store_id, offset, loop_counter, for_loop_count, for_loop_bound, difference, read_buffer_no, break_loop;
	struct dedupe_super_block *sblk;

	buffer = NULL;
	dedupe = NULL;
	break_loop = difference = for_loop_bound = loop_counter =  for_loop_count = 0;
	read_buffer_no = DEDUPE_CACHE_START;
	
	sblk = (struct dedupe_super_block *)super_blk->s_fs_info;

	if(sblk->number_of_valid_dedupe_records == sblk->all_dedupe_records_on_disk) /* indicates new block needs to be added */
	{
		offset 		= sblk->number_of_valid_dedupe_records % MAX_DEDUPE_RECORD_PER_BLOCK;
		store_id 	= sblk->number_of_valid_dedupe_records / MAX_DEDUPE_RECORD_PER_BLOCK;

		buffer = sb_bread(super_blk, DEDUPE_CACHE_START + store_id);
		BUG_ON(!buffer);

		dedupe = (struct dedupe_info *)buffer->b_data;
		BUG_ON(!dedupe);

		dedupe = dedupe + offset;
		
		strncpy(dedupe->hash, hash, HASH_LENGTH );
		dedupe->reference_count 	= 1;
		dedupe->is_valid			= 1;
		dedupe->data_block_number 	= data_blk;

		mark_buffer_dirty(buffer);
		sync_dirty_buffer(buffer);
		brelse(buffer);	

		sblk->all_dedupe_records_on_disk++;		
	}
	else /* search for an invalid inode and */
	{
		while(loop_counter < sblk->all_dedupe_records_on_disk)
		{
			buffer = sb_bread(super_blk, read_buffer_no);
			BUG_ON(!buffer);

			dedupe = (struct dedupe_info *)buffer->b_data;
			BUG_ON(!dedupe);

			difference = sblk->all_dedupe_records_on_disk - loop_counter;
			for_loop_bound = difference > MAX_DEDUPE_RECORD_PER_BLOCK ? MAX_DEDUPE_RECORD_PER_BLOCK : difference;

			for(for_loop_count = 0; for_loop_count < for_loop_bound; ++for_loop_count)
			{
				if(dedupe->is_valid == 0)
				{
					strncpy(dedupe->hash, hash, HASH_LENGTH );
					dedupe->reference_count 	= 1;
					dedupe->is_valid			= 1;
					dedupe->data_block_number 	= data_blk;

					break_loop = 1;
					break;
				}

				++dedupe;
				loop_counter++;
			}	

			if(break_loop)
			{
				mark_buffer_dirty(buffer);
				sync_dirty_buffer(buffer);
				brelse(buffer);
				break;
			}
			brelse(buffer);
			++read_buffer_no;
		}
	}
	sblk->number_of_valid_dedupe_records++;
	superblock_sync(super_blk);
}

/*
	* This is a helper routine to copy contents on disk.
	* It is invoked by dedupefs_write to copy the contents in a while loop to disk
*/
void write_contents(struct file *filp, const char __user *user_data, size_t user_data_len, struct dedupe_inode *dedupe_fs_inode)
{
	size_t copy_number_bytes;
	char *existing_content, *kernel_data;
	
	unsigned char hash[HASH_LENGTH];
	uint64_t data_blk, blk_number;
	struct buffer_head *buffer;

	copy_number_bytes = 0;
	data_blk = blk_number = 0;
	buffer = NULL;
	kernel_data = (char *)kmalloc(DEDUPEFS_BLOCK_SIZE, GFP_KERNEL);

	while(user_data_len > 0)
	{
		copy_number_bytes = user_data_len > DEDUPEFS_BLOCK_SIZE ? DEDUPEFS_BLOCK_SIZE : user_data_len;

		memset(kernel_data, 0, DEDUPEFS_BLOCK_SIZE);			/*always clear the buffer */

		if(copy_from_user(kernel_data, user_data, copy_number_bytes) != 0)
		{
			printk(KERN_ERR "Failed in the function copy_from_user \n");
			return;
		}

		memset(&hash[0], 0, HASH_LENGTH);			/*always clear the array contents */
		compute_hash(kernel_data,hash, copy_number_bytes);
		
		if((blk_number = hash_check_and_update(filp->f_inode->i_sb, hash, 1)) == 0) /* Data block does not exist in dedupe store */
		{
			if(fetch_free_block(filp->f_inode->i_sb, &dedupe_fs_inode->associated_data_blocks[dedupe_fs_inode->number_blocks]) < 0)
				return;
			else
				printk(KERN_INFO "Fetched free block success in write \n");

			buffer = sb_bread(filp->f_inode->i_sb, dedupe_fs_inode->associated_data_blocks[dedupe_fs_inode->number_blocks]);
			BUG_ON(!buffer);

			existing_content = (char *)buffer->b_data;
			BUG_ON(!existing_content);

			memset(existing_content, 0, DEDUPEFS_BLOCK_SIZE);			/*always clear the buffer */
			memcpy(existing_content, kernel_data, copy_number_bytes);
			
			mark_buffer_dirty(buffer);
			sync_dirty_buffer(buffer);
			brelse(buffer);

			add_record_dedupe_store(filp->f_inode->i_sb, hash, dedupe_fs_inode->associated_data_blocks[dedupe_fs_inode->number_blocks]);			
		}
		else /* indicates matching hash found on disk */
		{
			dedupe_fs_inode->associated_data_blocks[dedupe_fs_inode->number_blocks] = blk_number;				
		}

		dedupe_fs_inode->file_size += copy_number_bytes;			
		dedupe_fs_inode->number_blocks++;	/*TODO: Save this inode to disk since it is now modified */
		
		user_data_len -= copy_number_bytes;		
		user_data = user_data + copy_number_bytes;
	}
	save_inode(filp->f_inode->i_sb, dedupe_fs_inode);

	kfree(kernel_data);
}
/*
	* This function is invoked when the user saves any data.
	* Currently it only handles new writes. Updates are yet to be handled
	* Function definition referenced from: http://lxr.free-electrons.com/source/include/linux/fs.h#L1507
	* References:
	1. http://lxr.free-electrons.com/source/mm/filemap.c#L2264
	2. http://www.fsl.cs.sunysb.edu/kernel-api/re257.html
	3. http://lxr.free-electrons.com/source/arch/x86/include/asm/uaccess.h#L688
*/
ssize_t dedupefs_write(struct file *filp, const char __user *user_data, size_t user_data_len, loff_t *offset_p)
{
	/*
		* Current Assumptions: 
		1. user_data_len is the amount of data that has to be writen
		2. offset_p is the offset within the file from where the data has to be writen
	*/
	
	char *existing_content, *kernel_data;
	struct buffer_head *buffer;
	struct dedupe_inode *dedupe_fs_inode;
	uint64_t blk_number;
	size_t copy_number_bytes, return_user_data_len;
	unsigned char hash[HASH_LENGTH];
	int offset_last_block, ret_val;
	
	buffer = NULL;
	dedupe_fs_inode = NULL;
	blk_number = 0;
	copy_number_bytes = 0;
	offset_last_block = 0;
	
	kernel_data = (char *)kmalloc(DEDUPEFS_BLOCK_SIZE, GFP_KERNEL);

	/* 
		generic_write_checks - is used to perform necessary checks before doing writes
		Return value: 
		0: Write is allowed
		Non zero value: Associated errror value
	*/

	ret_val = generic_write_checks(filp, offset_p, &user_data_len, 0);
	if(ret_val)
	{
		printk(KERN_ERR "Generic Write Check failed \n");
		return ret_val;
	}
	return_user_data_len = user_data_len;
	
	//*offset_p += return_user_data_len;
	
	//struct dedupe_super_block *dedupe_sb = filp->f_inode->i_sb->s_fs_info;
	dedupe_fs_inode  = (struct dedupe_inode *)filp->f_inode->i_private;
	
	if(dedupe_fs_inode->number_blocks == TWENTY)
		return -ENOSPC;
	
	if(dedupe_fs_inode->file_size % DEDUPEFS_BLOCK_SIZE == 0) /* indicates either this is the first write to a file or append to a new block */
	{
		write_contents(filp, user_data, user_data_len, dedupe_fs_inode);		
	}
	else /* indicates an append to an existing block is now required */
	{
		offset_last_block =  dedupe_fs_inode->file_size % DEDUPEFS_BLOCK_SIZE;
		
		buffer = sb_bread(filp->f_inode->i_sb, dedupe_fs_inode->associated_data_blocks[dedupe_fs_inode->number_blocks - 1]);
		BUG_ON(!buffer);
		
		existing_content = (char *)buffer->b_data;
		BUG_ON(!existing_content);

		memset(&hash[0], 0, HASH_LENGTH);						/* always clear the array contents */
		memset(kernel_data, 0, DEDUPEFS_BLOCK_SIZE);					/* always clear the buffer */
		
		compute_hash(existing_content, hash, offset_last_block);
		hash_check_and_update(filp->f_inode->i_sb, hash, 0);
		
		copy_number_bytes = ( DEDUPEFS_BLOCK_SIZE - offset_last_block > user_data_len ) ?  user_data_len : DEDUPEFS_BLOCK_SIZE - offset_last_block;
		
	 	if(copy_from_user(kernel_data, user_data, copy_number_bytes) != 0)
		{
			printk(KERN_ERR "Failed in the function copy_from_user \n");
			return -EFAULT;
		}

		strncat(existing_content, kernel_data, copy_number_bytes);

		memset(&hash[0], 0, HASH_LENGTH);						/* always clear the array contents */
		compute_hash(existing_content, hash, copy_number_bytes + offset_last_block);

		if((blk_number = hash_check_and_update(filp->f_inode->i_sb, hash, 1)) == 0) /* Data block does not exist in dedupe store */
		{
			mark_buffer_dirty(buffer);
			sync_dirty_buffer(buffer);
			brelse(buffer);

			add_record_dedupe_store(filp->f_inode->i_sb, hash, dedupe_fs_inode->associated_data_blocks[dedupe_fs_inode->number_blocks - 1]);		
		}
		else /* indicates matching hash found on disk */
		{
			clear_free_datablock(filp->f_inode->i_sb, dedupe_fs_inode->associated_data_blocks[dedupe_fs_inode->number_blocks - 1]);
			dedupe_fs_inode->associated_data_blocks[dedupe_fs_inode->number_blocks - 1] = blk_number;
			brelse(buffer);			
		}

		dedupe_fs_inode->file_size += copy_number_bytes;

		user_data = user_data + copy_number_bytes;
		user_data_len -= copy_number_bytes;

		write_contents(filp, user_data, user_data_len, dedupe_fs_inode);		
	}

	kfree(kernel_data);
	
	return return_user_data_len; //TODO: Fix this to proper failure value
}
/*
	* This function is readdir function implemented in FUSE
	* Return value of 0 indicates success

	* Function signature refered from: http://lxr.free-electrons.com/source/include/linux/fs.h#L1507
	* References:
	* 1. http://lxr.free-electrons.com/source/include/linux/fs.h#L804
	* 2. http://lxr.oss.org.cn/source/include/linux/fs.h#L2774
*/
int dedupefs_iterate(struct file *filp, struct dir_context *dir_context)
{
	struct dir_record *inode_map;
	struct buffer_head *buffer;
	int i;
	loff_t pos;
	uint64_t total_files, loop_count, loop_bound;

	struct dedupe_inode *dedupefs_inode;
	struct super_block *super_blk;

	dedupefs_inode = filp->f_inode->i_private;
	super_blk = filp->f_inode->i_sb;

	total_files = dedupefs_inode->all_file_count;
	loop_count = 0;

	pos = dir_context->pos;
	if (pos) 
	{
		return 0;
	}
	while(loop_count < dedupefs_inode->number_blocks)	/*This loop condition can be replaced by while(total_files > 0) */
	{
		buffer = sb_bread(super_blk, dedupefs_inode->associated_data_blocks[loop_count]);
		if(likely(buffer))
		{
			inode_map = (struct dir_record *)buffer->b_data;
			BUG_ON(!inode_map);

			loop_bound = total_files > MAX_DIR_RECORD_PER_BLOCK ? MAX_DIR_RECORD_PER_BLOCK : total_files;
			
			for(i = 0; i < loop_bound; ++i)
			{
				if(inode_map->is_valid == 1)	/* This check is required to ensure deleted files do not show up when the user executes ls */
				{
					dir_emit(dir_context, inode_map->fully_qualified_name, MAX_PATH_LEN, inode_map->inode_number, DT_UNKNOWN );
					dir_context->pos += sizeof(struct dir_record);	/* dir_context->pos is internally passed by dir_emit to filldir_t. Hence incrementing it is important */					
				}
				else
				{
					printk(KERN_INFO "Invlaid block \n");
				}				
				++inode_map;	/* This will actually help in reading the next content */											
			}
			++loop_count;
			total_files -= loop_bound;
		}
		else
		{
			printk(KERN_ERR "Failed to read buffer # %llu in dedupefs_iterate call \n", loop_count);
			BUG();			
		}		
		brelse(buffer);
	}

	return 0;
}

/*
	* Reference: 
	* 1. http://lxr.free-electrons.com/source/kernel/time/timekeeping.c#L1696
	* 2. http://www.fsl.cs.sunysb.edu/kernel-api/re369.html
	* 3. https://www.kernel.org/doc/htmldocs/filesystems/API-inode-init-owner.html
*/
struct inode* allocate_base_inode(struct super_block *sb)
{		
	struct inode *base_inode = new_inode(sb);
	
	BUG_ON(!base_inode);

	base_inode->i_sb = sb;

	base_inode->i_ino = ROOT_INODE_NUMBER;

	base_inode->i_op = &dedupefs_inode_operations;
	
	base_inode->i_mtime = current_kernel_time();	/* This function returns timespec structure.*/
	base_inode->i_ctime = current_kernel_time();
	base_inode->i_atime = current_kernel_time();

	base_inode->i_fop = &dedupefs_directory_operations;

	base_inode->i_private = fetch_dedupefs_inode(sb, ROOT_INODE_NUMBER);
	if(unlikely(!base_inode->i_private))
	{
		printk(KERN_ERR "Failed to get root inode \n");
		BUG();
	}

	/* Second parameter is NULL since this is the root directory. Else it will the directory in which this inode will be placed. */
	inode_init_owner(base_inode, NULL, S_IFDIR);

	return base_inode;
}

void fill_superblock(struct super_block *sb)
{
	sb->s_magic = MAGIC;
	sb->s_op = &deudpefs_sops;

	sb->s_maxbytes = DEDUPEFS_BLOCK_SIZE;

	sb->s_root = d_make_root(allocate_base_inode(sb)); /*TODO: Reference for this function*/	
}
/*
	* This function will read the superblock writen while mounting the file system
	* Super block will be located at block 0 on the disk

	* Function signature referred from: http://lxr.free-electrons.com/source/security/inode.c#L33
	* Function working referred from: http://lxr.free-electrons.com/source/fs/libfs.c#L477
	* Debugging techniques refered from Linux Kernel Development, Third Edition - Robert Love, Chapter 18
	* Return Value:
	* 0 : Success
	* Anything else : Failure
	
	* References:
	* 1. http://en.wikibooks.org/wiki/The_Linux_Kernel/sb_bread
	* 2. http://en.wikibooks.org/wiki/The_Linux_Kernel/bread
	* 3. http://www.tsri.com/jeneral/kernel/include/linux/buffer_head.h/pm/PM-SB_BREAD.html
*/
int dedupefs_superblock_fill(struct super_block *sb, void *data, int silent)
{

	struct dedupe_super_block *dedupe_sb = NULL;
	struct buffer_head *buffer = NULL;
	
	buffer = sb_bread(sb, SUPERBLOCK_BLOCK_NUMBER);	/* 0 : indicates block # where super block is stored on disk */
	if(unlikely(buffer == NULL))
	{
		printk(KERN_ERR "Failed to read super block from disk\n");
		dump_stack();
	}
	else
	{
		dedupe_sb = (struct dedupe_super_block *)buffer->b_data;
		BUG_ON(!dedupe_sb);		

		if(likely(dedupe_sb->magic_no == MAGIC))
		{
			/* In case things work weirdly check about the block size. That may be an issue. */
			sb->s_fs_info = dedupe_sb;
			fill_superblock(sb);			
			brelse(buffer);	/*TODO: If things word wierdly try putting brelse just before returning */
			return (sb->s_root == NULL) ? -ENOMEM : 0;		 
		}
		else
		{
			printk(KERN_ERR "Magic number read from disk is %llu",dedupe_sb->magic_no);				
		}
	}
	brelse(buffer);	/*TODO: If things word wierdly try putting brelse just before returning */
	return 1;
}

/*
	* Function signature refered from : http://lxr.free-electrons.com/source/fs/debugfs/inode.c#L290	
*/
struct dentry *dedupefs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data)
{
	struct dentry *dentry_ret = mount_bdev(fs_type, flags, dev_name, data, dedupefs_superblock_fill);

	if (unlikely(IS_ERR(dentry_ret)))
		printk(KERN_ERR "Error mounting dedupefs");
	else
		printk(KERN_INFO "dedupefs is succesfully mounted\n");

	return dentry_ret;
}

int dedupefs_init(void)
{
	int ret;

	dedupefs_slab_inode = kmem_cache_create("dedupefs_inode_slab_mgmt", sizeof(struct dedupe_inode), 0, (SLAB_RECLAIM_ACCOUNT| SLAB_MEM_SPREAD), NULL);

	if (unlikely(!dedupefs_slab_inode))
	{
		printk(KERN_ERR "Failed to allocate slab slab-memory-management\n");
		return -ENOMEM;
	}

	ret = register_filesystem(&dedupefs_fs_type);
	if (likely(ret == 0))
		printk(KERN_INFO "Sucessfully registered dedupefs\n");
	else
		printk(KERN_ERR "Failed to register dedupefs. Error:[%d]", ret);

	return ret;
}

void dedupefs_exit(void)
{
	if (likely(unregister_filesystem(&dedupefs_fs_type) == 0))
		printk(KERN_INFO "Sucessfully unregistered dedupefs\n");
	else
		printk(KERN_ERR "Failed to unregister dedupefs.");
	
	kmem_cache_destroy(dedupefs_slab_inode);
}

/*
	* This function is used to find number of free blocks from the super block store.
	* 1: block is used
	* 0: block is free
*/
int total_free_blocks(struct dedupe_super_block *dedupe_sb)
{
	int free_blocks, loop_outer, loop_inner;
	uint64_t comparator, one;
	free_blocks = 0;
	one = 1;

	for(loop_outer = START_DATA_BLOCK_NUMBER; loop_outer < 64; ++loop_outer)
	{
		comparator = one << loop_outer;
		if((dedupe_sb->all_data_blocks[0] & comparator) == 0)
			++free_blocks;
	}
	
	for(loop_outer = 1; loop_outer < 64; ++loop_outer)
	{
		for(loop_inner = 0; loop_inner < 64; ++loop_inner)
		{
			comparator = one << loop_inner;
			if((dedupe_sb->all_data_blocks[loop_outer] & comparator) == 0)
				++free_blocks;
		}			
	}

	return free_blocks;
}

int dedupefs_statfs(struct dentry *dentry, struct kstatfs *dedupe_stats)
{
    struct dedupe_super_block *dedupe_sb;
    
    dedupe_sb = (struct dedupe_super_block *)dentry->d_sb->s_fs_info;
    BUG_ON(!dedupe_sb);

    dedupe_stats->f_type = MAGIC;
    dedupe_stats->f_bsize = DEDUPEFS_BLOCK_SIZE;
    dedupe_stats->f_blocks = MAX_BLOCKS;

    dedupe_stats->f_bfree = dedupe_stats->f_bavail = total_free_blocks(dedupe_sb);
    
    return 0;
}


module_init(dedupefs_init);
module_exit(dedupefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denil Vira, Nitin Tak, Pooja Routray, Sumeet Hemant Bhatia");