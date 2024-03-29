/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#define    FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

//size of a disk block
#define    BLOCK_SIZE 512

//we'll use 8.3 filenames
#define    MAX_FILENAME 8
#define    MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry {
    int nFiles;    //How many files are in this directory.
    //Needs to be less than MAX_FILES_IN_DIR

    struct cs1550_file_directory {
        char fname[MAX_FILENAME + 1];    //filename (plus space for nul)
        char fext[MAX_EXTENSION + 1];    //extension (plus space for nul)
        size_t fsize;                    //file size
        long nStartBlock;                //where the first block is on disk
    } __attribute__((packed)) files[MAX_FILES_IN_DIR];    //There is an array of these

    //This is some space to get this to be exactly the size of the disk block.
    //Don't use it for anything.
    char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
};

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory {
    int nDirectories;    //How many subdirectories are in the root
    //Needs to be less than MAX_DIRS_IN_ROOT
    struct cs1550_directory {
        char dname[MAX_FILENAME + 1];    //directory name (plus space for nul)
        long nStartBlock;                //where the directory block is on disk
    } __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];    //There is an array of these

    //This is some space to get this to be exactly the size of the disk block.
    //Don't use it for anything.
    char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
};


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define    MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct cs1550_disk_block {
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
    directory[0] = '\0'; //put terminators before and after string in char array
    filename[0] = '\0';
    extension[0] = '\0';

    int error = sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension); //regex from proj description

    if (error == 0) {
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

    if (!file) {
        printf("\n.disk error\n");
    } else {
        struct cs1550_root_directory *root = malloc(sizeof(struct cs1550_root_directory));

        if (!fread(root, sizeof(struct cs1550_root_directory), 1, file)) {
            printf("\n.disk error\n");
            fclose(file);
            return -1;
        }
        int i;
        for (i = 0; i < root->nDirectories; i++) {
            if (!strcmp(root->directories[i].dname, directory)) { //directory at array of root's directories matches the new root
                location = root->directories[i].nStartBlock;
                int offset = location * BLOCK_SIZE;
                if (fseek(file, offset, SEEK_SET)) {
                    printf("\n.disk error\n");
                    fclose(file);
                    location = -1;
                } else {
                    if (!fread(entry, sizeof(struct cs1550_directory_entry), 1, file)) {
                        printf("\n.disk error\n");
                        fclose(file);
                        location = -1;
                    }
                }
                break;
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
static int cs1550_getattr(const char *path, struct stat *stbuf) {
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
        if (location != -1) {
            if (strlen(filename) == 0) {
                stbuf->st_mode = S_IFDIR | 0755;
                stbuf->st_nlink = 2;
            } else {
                int i;
                int valid = 0;

                for (i = 0; i < entry->nFiles; i++) {
                    if (!strcmp(entry->files[i].fname, filename)) {
                        valid = 1;
                        break;
                    }
                }

                if (valid) {
                    //regular file, probably want to be read and write
                    stbuf->st_mode = S_IFREG | 0666;
                    stbuf->st_nlink = 1; //file links
                    stbuf->st_size = entry->files[i].fsize;
                } else {
                    res = -ENOENT;
                }
            }
        } else {
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
                          off_t offset, struct fuse_file_info *fi) {
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

        if (!file) {
            printf("\n.disk error\n");
        } else {
            struct cs1550_root_directory *root = malloc(sizeof(struct cs1550_root_directory));

            if (!fread(root, sizeof(struct cs1550_root_directory), 1, file)) {
                printf("\n.disk error\n");
                fclose(file);
                return -ENOENT;

            }
            int i;
            for (i = 0; i < root->nDirectories; i++) {
                filler(buf, root->directories[i].dname, NULL, 0);
            }
        }
        fclose(file);
    } else {
        format(path, filename, directory, extension);
        struct cs1550_directory_entry *entry = malloc(sizeof(struct cs1550_directory_entry));
        int location = findDirectory(directory, entry);
        if (location != 1) {
            filler(buf, ".", NULL, 0);
            filler(buf, "..", NULL, 0);
            int i;
            for (i = 0; i < entry->nFiles; i++) {
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
static int cs1550_mkdir(const char *path, mode_t mode) {

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

    if (strlen(directory) > MAX_FILENAME) {
        printf("\ndirectory name too long\n");
        return -ENAMETOOLONG;
    }
    if (strlen(filename)) {
        printf("\ncan only create directory under root\n");
        return -EPERM;
    }
    if (location != -1) {
        return -EEXIST;
    } else {
        FILE *f;
        f = fopen(".disk", "rb+");

        if (!f) {
            printf("\n.disk error\n");
            fclose(f);
            return -1;
        }

        struct cs1550_root_directory *root = malloc(sizeof(struct cs1550_root_directory));

        if (!fread(root, sizeof(struct cs1550_root_directory), 1, f)) {
            printf("\n.disk error\n");
            fclose(f);
            return -1;
        }

        if (root->nDirectories == MAX_DIRS_IN_ROOT) {
            printf("\nroot directory reached capacity\n");
            fclose(f);
            return -EPERM;
        }

        strcpy(root->directories[root->nDirectories].dname, directory);
        if (fseek(f, -sizeof(struct cs_1550_fat), SEEK_END)) {
            printf("\nfat error\n");
            fclose(f);
            return -1;
        }
        struct cs_1550_fat *fat = (struct cs_1550_fat *) malloc(BLOCK_SIZE);
        if (!fread(fat, sizeof(struct cs_1550_fat), 1, f)) {
            printf("\n.disk error\n");
            fclose(f);
            return -1;
        }
        if (fat->table[0] == 0) {
            int i;
            for (i = 0; i < MAX_FAT; i++) {
                fat->table[i] = (short) -1;
            }
            fat->table[0] = (short) -2;
        }
        int i, free_block = -1;
        for (i = 0; i < MAX_FAT; i++) {
            if (fat->table[i] == (short) -1) {
                free_block = i;
                fat->table[i] = (short) -2;
                break;
            }
        }
        if (free_block == -1) {
            printf("\nno free blocks\n");
            fclose(f);
            return -1;
        }

        root->directories[root->nDirectories].nStartBlock = free_block;
        fat->table[free_block] = (short) -2;
        if (fseek(f, -sizeof(struct cs_1550_fat), SEEK_END)) {
            printf("\n.disk error\n");
            return -1;
        }
        if (!fwrite(fat, sizeof(struct cs_1550_fat), 1, f)) {
            printf("\nError on writing FAT back to .disk after update\n");
            fclose(f);
            return -1;
        }
        int off = free_block * BLOCK_SIZE;
        if (fseek(f, off, SEEK_SET)) {
            printf("error freeing block on .disk\n");
            fclose(f);
            return -1;
        }
        struct cs1550_directory_entry *new_dir = (struct cs1550_directory_entry *) malloc(
                sizeof(struct cs1550_directory_entry));
        memset(new_dir, 0, sizeof(struct cs1550_directory_entry));
        if (!fwrite(new_dir, sizeof(struct cs1550_directory_entry), 1, f)) {
            printf("error writing to .disk\n");
            fclose(f);
            return -1;
        }
        root->nDirectories++;
        if (fseek(f, 0, SEEK_SET)) {
            printf("\nerror seeking root on .disk\n");
            fclose(f);
            return -1;
        }
        if (!fwrite(root, sizeof(struct cs1550_root_directory), 1, f)) {
            printf("\nerror writing root to .disk\n");
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
static int cs1550_rmdir(const char *path) {
    (void) path;

    return 0;
}

/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev) {
    (void) mode;
    (void) dev;
    char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    format(path, directory, filename, extension);

    if (strlen(filename) > MAX_FILENAME + 1) {
        return -ENAMETOOLONG;
    } else if (strlen(extension) > MAX_EXTENSION + 1) {
        return -ENAMETOOLONG;
    }
    struct cs1550_directory_entry *entry = malloc(sizeof(struct cs1550_directory_entry));
    int dir = findDirectory(directory, entry);
    if (dir == -1) {
        printf("\ndirectory doesn't exist");
        return -EPERM;
    } else if (entry->nFiles == MAX_FILES_IN_DIR) {
        printf("\ndirectory at max files\n");
        return -1;
    }
    int i;
    for (i = 0; i < entry->nFiles; i++) {
        if (strcmp(entry->files[i].fname, filename) && strcmp(entry->files[i].fext, extension)) {
            printf("File exists\n");
            return -EEXIST;
        }
    }
    FILE *file;
    file = fopen(".disk", "rb+");
    if (!file) {
        printf("\nerror opening .disk\n");
        fclose(file);
        return -1;
    }
    if (fseek(file, -sizeof(struct cs_1550_fat), SEEK_END)) {
        fclose(file);
        return -1;
    }
    struct cs_1550_fat *fat = (struct cs_1550_fat *) malloc(BLOCK_SIZE);
    if (!fread(fat, sizeof(struct cs_1550_fat), 1, file)) {
        printf("\nerror reading table from disk\n");
        fclose(file);
        return -1;
    }
    int free_block = -1;
    for (i = 0; i < MAX_FAT; i++) {
        if (fat->table[i] == (short) -1) {
            free_block = i;
            fat->table[i] = (short) -2;
            break;
        }
    }
    if (free_block == -1) {
        printf("\nno free blocs in table\n");
        fclose(file);
        return -1;
    }


    strcpy(entry->files[entry->nFiles].fname, filename);
    strcpy(entry->files[entry->nFiles].fext, extension);
    entry->files[entry->nFiles].nStartBlock = free_block;
    entry->files[entry->nFiles].fsize = 0;
    fat->table[free_block] = (short) -2;
    if (fseek(file, -sizeof(struct cs_1550_fat), SEEK_END)) {
        printf("\nerror seeking in fat\n");
        fclose(file);
        return -1;
    }
    if (!fwrite(fat, sizeof(struct cs_1550_fat), 1, file)) {
        printf("\nerror writing fat to .disk\n");
        fclose(file);
        return -1;
    }
    int off = free_block * BLOCK_SIZE;
    if (fseek(file, off, SEEK_SET)) {
        printf("\nerror freeing block in disk\n");
        fclose(file);
        return -1;
    }
    struct cs1550_disk_block *block = malloc(sizeof(cs1550_disk_block));
    for (i = 0; i < MAX_DATA_IN_BLOCK; i++) {
        block->data[i] = 0;
    }
    if (!fwrite(block, sizeof(struct cs1550_file_directory), 1, file)) {
        printf("error writing to .disk\n");
        fclose(file);
        return -1;
    }
    entry->nFiles++;
    off = dir * BLOCK_SIZE;
    if (fseek(file, off, SEEK_SET)) {
        printf("\n.disk error\n");
        fclose(file);
        return -1;
    }
    if (!fwrite(entry, sizeof(struct cs1550_directory_entry), 1, file)) {
        printf("\nerror writing directory to .disk\n");
        fclose(file);
        return -1;
    }


    fclose(file);

    return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path) {
    (void) path;

    return 0;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi) {
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
    if (location == -1) {
        printf("\npath doesnt exist\n");
        return 0;
    }
    int i;
    int file_location = -1;
    size_t file_size = 0;
    for (i = 0; i < entry->nFiles; i++) {
        if (!strcmp(filename, entry->files[i].fname) && !strcmp(extension, entry->files[i].fext)) {
            file_location = entry->files[i].nStartBlock;
            file_size = entry->files[i].fsize;
            break;
        }
    }
    if (file_location == -1) {
        printf("\nfile does not exist in directory\n");
        return 0;
    }
    //check that size is > 0

    if (size <= 0) {
        printf("\nsize can;t be zero\n");
        return 0;
    }
    //check that offset is <= to the file size
    if (file_size < offset) {
        printf("\noffset cant be greater than filezise\n");
        return 0;
    }
    //read in data
    if (file_size < (offset + size)) {
        size = file_size - offset;
    }
    FILE *file;
    file = fopen(".disk", "rb");
    if (!file) {
        printf("\nerror opening .disk\n");
        fclose(file);
        return 0;
    }
    struct cs_1550_fat *fat = malloc(sizeof(struct cs_1550_fat));
    if (fseek(file, -sizeof(struct cs_1550_fat), SEEK_END)) {
        printf("\nerror seeking to fat on .disk\n");
        fclose(file);
        return 0;
    }
    if (!fread(fat, sizeof(struct cs_1550_fat), 1, file)) {
        printf("\nerror reading fat from .disk\n");
        fclose(file);
        return 0;
    }

    //set size and return, or error


    while (offset > BLOCK_SIZE) {
        offset -= BLOCK_SIZE;
        if (fat->table[file_location] == -2) {
            break;
        }
        file_location = fat->table[file_location];
    }
    int off = file_location * BLOCK_SIZE;
    if (fseek(file, off, SEEK_SET)) {
        printf("\nerror seeking to file location on disk\n");
        fclose(file);
        return 0;
    }
    int count = 1;
    if ((offset + size) > BLOCK_SIZE) {
        count = (offset + size) / BLOCK_SIZE;
        if (((offset + size) % BLOCK_SIZE) > 0) {
            count++;
        }
    }
    struct cs1550_disk_block *block = malloc(sizeof(struct cs1550_disk_block));
    if (!fread(block->data, MAX_DATA_IN_BLOCK, 1, file)) {
        printf("\nerror reading disk block\n");
        fclose(file);
        return 0;
    }
    if (count > 1) {
        strncat(buf, &block->data[offset], MAX_DATA_IN_BLOCK);
        size -= MAX_DATA_IN_BLOCK;
        int readCount = 1;
        while (readCount < count) {
            int read_size = MAX_DATA_IN_BLOCK;
            if (size < MAX_DATA_IN_BLOCK) {
                read_size = size;
            }
            file_location = fat->table[file_location];
            if (file_location == -2) {
                printf("\nHit EOF before completing requested read\n");
                break;
            }
            off = file_location * BLOCK_SIZE;
            if (fseek(file, off, SEEK_SET)) {
                printf("\nError on seeking to next disk block of file\n");
                break;
            }
            if (!fread(block->data, read_size, 1, file)) {
                printf("\nError on reading disk block\n");
                break;
            }
            strncat(buf, &block->data[0], read_size);
            readCount++;
            size -= MAX_DATA_IN_BLOCK;
        }
    } else {
        strncat(buf, &block->data[offset], size);
    }
    size = strlen(buf);
    strcat(buf, "\n");

    size = 0;

    return size;
}

/* 
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi) {
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
    if (location == -1) {
        printf("\npath doesn't exist\n");
        return 0;
    }
    //check that size is > 0
    size_t file_size = 0;
    int found_index = -1;
    int found_location = -1;
    int i = 0;
    for (i = 0; i < entry->nFiles; i++) {
        if (!strcmp(filename, entry->files[i].fname) && !strcmp(extension, entry->files[i].fext)) {
            found_index = i;
            found_location = entry->files[i].nStartBlock;
            file_size = entry->files[i].fsize;
            break;
        }
    }

    if (file_size <= 0 || found_index == -1) {
        printf("\nfile size must be nonzero");
        return 0;

    }
    //check that offset is <= to the file size

    if (offset > file_size) {
        printf("\noffest can't be larger than file size");
        return -EFBIG;
    }
    //write data
    FILE *file;
    file = fopen(".disk", "rb+");
    if (!file) {
        fclose(file);
        printf("\ncouldn't open disk");
        return 0;
    }

    //set size (should be same as input) and return, or error

    struct cs_1550_fat *fat = malloc(sizeof(struct cs_1550_fat));
    if (fseek(file, -sizeof(struct cs_1550_fat), SEEK_END)) {
        printf("\nerror seeking fat on .disk\n");
        fclose(file);
        return 0;
    }
    if (!fread(fat, sizeof(struct cs_1550_fat), 1, file)) {
        printf("\nerror reading fat on .disk\n");
        fclose(file);
        return 0;
    }
    int real_offset = offset;
    while (real_offset > BLOCK_SIZE) {
        real_offset -= BLOCK_SIZE;
        if (fat->table[found_location] == -2) {
            break;
        }
        found_location = fat->table[found_location];
    }
    int off = found_location * BLOCK_SIZE;
    if (fseek(file, off, SEEK_SET)) {
        printf("\nerror seeking file location on .disk\n");
        fclose(file);
        return 0;
    }
    int num_blocks = 1;
    if ((offset + size) > BLOCK_SIZE) {
        num_blocks = (offset + size) / BLOCK_SIZE;
        if (((offset + size) % BLOCK_SIZE) > 0) {
            num_blocks++;
        }
    }
    struct cs1550_disk_block *block = malloc(sizeof(struct cs1550_disk_block));
    int blocks_written = 0;
    int bytes_written = 0;
    int bytes_remaining = size;
    while (blocks_written < num_blocks) {
        int write_size = MAX_DATA_IN_BLOCK;
        if (bytes_remaining < MAX_DATA_IN_BLOCK) {
            write_size = bytes_remaining;
        }
        if (!fread(block->data, MAX_DATA_IN_BLOCK, 1, file)) {
            printf("\nerorr reading block from .disk\n");
            fclose(file);
            return 0;
        }
        if (blocks_written == 0) {
            if ((write_size + offset) > MAX_DATA_IN_BLOCK) {
                write_size = MAX_DATA_IN_BLOCK - offset;
            }
        } else {
            real_offset = 0;
        }
        block->data[real_offset] = 0;
        strncat(&block->data[real_offset], buf, write_size);
        buf += write_size * sizeof(char);
        bytes_written += write_size;
        bytes_remaining -= write_size;
        off = found_location * BLOCK_SIZE;
        if (fseek(file, off, SEEK_SET)) {
            printf("error seeking beginning of block\n");
            return 0;
        }
        if (!fwrite(block->data, BLOCK_SIZE, 1, file)) {
            printf("\nerror writing .disk\n");
            return 0;
        }
        if (bytes_remaining) {
            if (fat->table[found_location] != -2) {
                found_location = fat->table[found_location];
            } else {
                int free_block = -1;
                for (i = 0; i < MAX_FAT; i++) {
                    if (fat->table[i] == (short) -1) {
                        free_block = i;
                        fat->table[found_location] = i;
                        fat->table[i] = (short) -2;
                        break;
                    }
                }
                if (free_block == -1) {
                    printf("\ndisk full\n");
                    fclose(file);
                    return bytes_written;
                }
                if (fseek(file, -sizeof(struct cs_1550_fat), SEEK_END)) {
                    printf("\neroor seeking fat on .disk\n");
                    fclose(file);
                    return 0;
                }
                if (!fwrite(fat, sizeof(struct cs_1550_fat), 1, file)) {
                    printf("\nerror writing fat to .disk\n");
                    fclose(file);
                    return 0;
                }
                found_location = free_block;
            }
            off = found_location * BLOCK_SIZE;
            if (fseek(file, off, SEEK_SET)) {
                printf("\nwrite erorr\n");
                return bytes_written;
            }
        } else {
            break;
        }
        blocks_written++;
    }
    if ((offset + size) > file_size) {
        entry->files[found_index].fsize += (offset + size) - file_size;
    } else if ((offset + size) < file_size) {
        entry->files[found_index].fsize = offset + size;
    }
    if (file_size != entry->files[found_index].fsize) {
        off = location * BLOCK_SIZE;
        if (fseek(file, off, SEEK_SET)) {
            printf("\nerror finding directory on .disk\n");
            fclose(file);
            return 0;
        }
        if (!fwrite(entry, sizeof(struct cs1550_directory_entry), 1, file)) {
            printf("\nerror writing directory to .disk\n");
            fclose(file);
            return 0;
        }
    }
    fclose(file);

    return bytes_written;
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
static int cs1550_truncate(const char *path, off_t size) {
    (void) path;
    (void) size;

    return 0;
}


/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi) {
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
static int cs1550_flush(const char *path, struct fuse_file_info *fi) {
    (void) path;
    (void) fi;

    return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
        .getattr    = cs1550_getattr,
        .readdir    = cs1550_readdir,
        .mkdir    = cs1550_mkdir,
        .rmdir = cs1550_rmdir,
        .read    = cs1550_read,
        .write    = cs1550_write,
        .mknod    = cs1550_mknod,
        .unlink = cs1550_unlink,
        .truncate = cs1550_truncate,
        .flush = cs1550_flush,
        .open    = cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &hello_oper, NULL);
}