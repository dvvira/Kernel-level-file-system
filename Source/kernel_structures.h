#ifndef _KERNEL_STRUCTURES_H
#define _KERNEL_STRUCTURES_H

/*
	* This file contains the definition of kernel structures for callbacks to dedupe FS
	* This code is submitted by Denil Vira (dvvira), Nitin Tak (ntak), Pooja Routray (proutra), Sumeet Hemant Bhatia (sbhatia3) as a part of CSC 568 - 001 Spring 2015
*/

#include <linux/fs.h>

extern struct dentry *dedupefs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);
extern int dedupefs_mkdir(struct inode *inod, struct dentry *dentry, umode_t mode);
extern int dedupefs_create(struct inode *inod, struct dentry *dentry, umode_t mode, bool excl);
extern ssize_t dedupefs_read(struct file *filp, char __user *, size_t, loff_t *);
extern ssize_t dedupefs_write(struct file *filp, const char __user *user_data, size_t user_data_len, loff_t *offset_p);
extern int dedupefs_iterate(struct file *filp, struct dir_context *dir_context);
extern struct dentry *dedupefs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data);
extern int dedupefs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *ks);
extern int dedupefs_rmdir(struct inode *parent_inode, struct dentry *dentry);
extern int dedupefs_unlink(struct inode *parent_inode, struct dentry *dentry);
extern int dedupefs_statfs(struct dentry *dentry, struct kstatfs *dedupe_stats);

struct file_system_type dedupefs_fs_type = {
	.owner 		= THIS_MODULE,
	.name 		= "dedupefs",
	.mount 		= dedupefs_mount,
	.kill_sb 	= kill_block_super,
	.fs_flags 	= FS_REQUIRES_DEV,
};

struct inode_operations dedupefs_inode_operations = {
	.create = dedupefs_create,
	.lookup = dedupefs_lookup,
	.mkdir = dedupefs_mkdir,
	.getattr = dedupefs_getattr,
	.rmdir = dedupefs_rmdir,
	.unlink = dedupefs_unlink,
};

/*
	* File operations are split into two structures to assign i_fop appropriately for regular files and directories
*/
struct file_operations dedupefs_directory_operations = {
	.owner = THIS_MODULE,
	.iterate = dedupefs_iterate,
};

struct file_operations dedupefs_file_operations = {
	.read = dedupefs_read,
	.write = dedupefs_write,
};

struct super_operations deudpefs_sops = {
	.statfs        = dedupefs_statfs,
};

#endif