/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct cs1550_disk_block
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;

#define MAX_FAT (BLOCK_SIZE/sizeof(short))

struct cs_1550_fat {
    short table[MAX_FAT];
};

void format(const char *path, char *directory, char *filename, char *extension) {
	directory[0] = '\0';
	filename[0] = '\0';
	extension[0] = '\0';

	int error = sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	if(error == 0) {
		printf("path error\n");
	}

	directory[MAX_FILENAME] = '\0';
	filename[MAX_FILENAME] = '\0';
	extension[MAX_EXTENSION] = '\0';
	return;
}

int findDirectory(char *directory, struct cs1550_directory_entry *entry) {
	FILE *file;
	int location = -1;
	file = fopen(".disk", "rb");

	if(!file) {
		printf("\n.disk error\n");
	} else {
		struct cs1550_root_directory *root = malloc(sizeof(struct cs1550_root_directory));

		if(!fread(root, sizeof(struct cs1550_root_directory), 1, file)) {
			printf("\n.disk error\n");
			fclose(file);
			return -1;
		}
		int i;
		for(i = 0; i < root->nDirectories; i++) {
			if(!strcmp(root->directories[i].dname, directory)) {
				location = root->directories[i].nStartBlock;
				int offset = location * BLOCK_SIZE;
				if(fseek(file, offset, SEEK_SET)){
					printf("\n.disk error\n");
					fclose(file);
					location = -1;
				} else {
					if(!fread(entry, sizeof(struct cs1550_directory_entry), 1, file)) {
						printf("\n.disk error\n");
						fclose(file);
						location = -1;
					}
				} break;
			}
		}
	}

	fclose(file);
	return location;
}


/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not. 
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
	char directory[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];


	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {
		format(path, filename, directory, extension);
		struct cs1550_directory_entry *entry = malloc(sizeof(struct cs1550_directory_entry));
		int location = findDirectory(directory, entry);
		if(location != -1) {
			if(strlen(filename) == 0) {
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 2;
			} else {
				int i;
				int valid = 0;

				for(i = 0; i < entry->nFiles; i++) {
					if(!strcmp(entry->files[i].fname, filename)) {
						valid = 1;
						break;
					}
				}

				if(valid) {
					//regular file, probably want to be read and write
					stbuf->st_mode = S_IFREG | 0666;
					stbuf->st_nlink = 1; //file links
					stbuf->st_size = entry->files[i].fsize;
				} else {
					res = -ENOENT;
				}
			}
		}

		else {
			//Else return that path doesn't exist
			res = -ENOENT;
		}
	}
	return res;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

	char directory[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];

	//This line assumes we have no subdirectories, need to change
	if (strcmp(path, "/") != 0) {
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);

		FILE *file;
		file = fopen(".disk", "rb");

		if(!file) {
			printf("\n.disk error\n");
		} else {
			struct cs1550_root_directory *root = malloc(sizeof(struct cs1550_root_directory));

			if(!fread(root, sizeof(struct cs1550_root_directory), 1, file)) {
				printf("\n.disk error\n");
				fclose(file);
				return -ENOENT;

			}
			int i;
			for(i = 0; i < root->nDirectories; i++) {
				filler(buf, root->directories[i].dname, NULL, 0);
			}
		}
		fclose(file);
	} else {
		format(path, filename, directory, extension);
		struct cs1550_directory_entry *entry = malloc(sizeof(struct cs1550_directory_entry));
		int location = findDirectory(directory, entry);
		if(location != 1) {
			filler(buf, ".", NULL, 0);
			filler(buf, "..", NULL, 0);
			int i;
			for(i = 0; i <entry->nFiles; i++) {
				char fullName[MAX_FILENAME + MAX_EXTENSION + 2];
				strcpy(fullName, entry->files[i].fname);
				strcat(fullName, ".");
				strcat(fullName, entry->files[i].fext);
				filler(buf, fullName, NULL, 0);
			}
		} else {
			return -ENOENT;
		}
	}
	return 0;
}

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{

	/*
	 * 	0 on success
	 * 	-ENAMETOOLONG if the name is beyond 8 chars
	 * 	-EPERM if the directory is not under the root dir only
	 * 	-EEXIST if the directory already exists
	 */
	(void) path;
	(void) mode;

	char directory[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];

	format(path, filename, directory, extension);

	struct cs1550_directory_entry *entry = malloc(sizeof(struct cs1550_directory_entry));
	int location = findDirectory(directory, entry);

	if(strlen(directory) > MAX_FILENAME) {
		printf("\ndirectory name too long\n");
		return -ENAMETOOLONG;
	} if(strlen(filename)) {
		printf("\ncan only create directory under root\n");
		return -EPERM;
	} if(location != -1) {
		return -EEXIST;
	} else {
		FILE *f;
		f = fopen(".disk", "rb+");

		if(!f) {
			printf("\n.disk error\n");
			fclose(f);
			return -1;
		}

		struct cs1550_root_directory *root = malloc(sizeof(struct cs1550_root_directory));

		if(!fread(root, sizeof(struct cs1550_root_directory), 1, f)) {
			printf("\n.disk error\n");
			fclose(f);
			return -1;
		}

		if(root->nDirectories == MAX_DIRS_IN_ROOT) {
            printf("\nroot directory reached capacity\n");
            fclose(f);
            return -EPERM;
		}

        strcpy(root->directories[root->nDirectories].dname, directory);
        if(fseek(f, -sizeof(struct cs_1550_fat), SEEK_END)){
            printf("\nfat error\n");
            fclose(f);
            return -1;
        }
        struct cs_1550_fat *fat = (struct cs_1550_fat *)malloc(BLOCK_SIZE);
        if(!fread(fat, sizeof(struct cs_1550_fat), 1, f)){
            printf("\n.disk error\n");
            fclose(f);
            return -1;
        }
        if(fat->table[0] == 0){
            int i;
            for(i = 0; i < MAX_FAT; i++){
                fat->table[i] = (short) -1;
            }
            fat->table[0] = (short) -2;
        }
        int i, free_block = -1;
        for(i = 0; i < MAX_FAT; i++){
            if(fat->table[i] == (short) -1){
                free_block = i;
                fat->table[i] = (short) -2;
                break;
            }
        }
        if(free_block == -1){
            printf("\nno free blocks\n");
            fclose(f);
            return -1;
        }

        root->directories[root->nDirectories].nStartBlock = free_block;
        fat->table[free_block] = (short) -2;
        if(fseek(f, -sizeof(struct cs_1550_fat), SEEK_END)){
            printf("\n.disk error\n");
            return -1;
        }
        if(!fwrite(fat, sizeof(struct cs_1550_fat), 1, f)){
            printf("\nError on writing FAT back to .disk after update\n");
            fclose(f);
            return -1;
        }
        int off = free_block * BLOCK_SIZE;
        if(fseek(f, off, SEEK_SET)){
            printf("Error on seeking to free block in .disk\n");
            fclose(f);
            return -1;
        }
        struct cs1550_directory_entry *new_dir = (struct cs1550_directory_entry *)malloc(sizeof(struct cs1550_directory_entry));
        memset(new_dir, 0, sizeof(struct cs1550_directory_entry));
        if(!fwrite(new_dir, sizeof(struct cs1550_directory_entry), 1, f)){
            printf("Failed to write to .disk\n");
            fclose(f);
            return -1;
        }
        root->nDirectories++;
        if(fseek(f, 0, SEEK_SET)){
            printf("\nError on seeking back to root on .disk\n");
            fclose(f);
            return -1;
        }
        if(!fwrite(root, sizeof(struct cs1550_root_directory), 1, f)){
            printf("\nError on writing root back to .disk\n");
            fclose(f);
            return -1;
        }
        fclose(f);

	}


	return 0;
}

/* 
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;

	return 0;
}

/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;
    char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    format(path, directory, filename, extension);

    if(strlen(filename) > MAX_FILENAME + 1){
        return -ENAMETOOLONG;
    } else if(strlen(extension) > MAX_EXTENSION + 1){
        return -ENAMETOOLONG;
    }
    struct cs1550_directory_entry *entry = malloc(sizeof(struct cs1550_directory_entry));
    int dir = findDirectory(directory, entry);
    if(dir == -1) {
        print("\ndirectory doesn't exist");
        return -EPERM;
    } else if(entry->nFiles == MAX_FILES_IN_DIR){
        printf("\ndirectory at max files\n");
        return -1;
    }
    int i;
    for(i = 0; i < entry->nFiles; i++){
        if(strcmp(entry->files[i].fname, filename) && strcmp(entry->files[i].fext, extension)){
            printf("File exists\n");
            return -EEXIST;
        }
    }
    FILE *file;
    file = fopen(".disk", "rb+");





    return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error

	size = 0;

	return size;
}

/* 
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size, 
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;
    char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    format(path, directory, filename, extension);


    //check to make sure path exists
    struct cs1550_directory_entry *entry = malloc(sizeof(struct cs1550_directory_entry));
    int location = findDirectory(directory, entry);
    if(location == -1){
        printf("\npath doesn't exist\n");
        return 0;
    }
	//check that size is > 0
	size_t size = 0;
    int found_index = -1;
    int found_location = -1;
    int i = 0;
    for(i = 0; i < entry->nFiles; i++) {
        if(!strcmp(filename, entry->files[i].fname) && !strcmp(extension, entry->files[i].fext)) {
            found_index = i;
            found_location = entry->files[i].nStartBlock;
            size = entry->files[i].fsize;
            break;
        }
    }

    if(size <= 0 || found_index == -1) {
        printf("\nfile size must be nonzero");

    }
	//check that offset is <= to the file size

	if(offset > size) {
        printf("\noffest can't be larger than file size");
	    return -EFBIG;
	}
	//write data

	//set size (should be same as input) and return, or error

	return size;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or 
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but 
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file 
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
