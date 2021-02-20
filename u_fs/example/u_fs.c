/**
 * @author 07计算机科学与技术2班 钟志豪 200734473222
 * @mail hotrhino@qq.com
 */

#define FUSE_USE_VERSION 26
#define MAX_FILENAME 8
#define MAX_EXTENSION 8
#define MAX_DATA_INBLOCK 360 //每块空闲为376字节 留空16字节做较检  待扩展

#define MAX_INODE 8192
#define MAX_BLOCK 20480

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

struct sb{
	int fs_size;
    int first_blk;
	int bitmap;
};

struct u_fs_file_directory{
	char fname[MAX_FILENAME+1];
    char fext[MAX_EXTENSION+1];
    size_t fsize;
    int nStartBlock;
    int flag;
};

struct dir_sb{
	int nNextBlock;
	unsigned int bitmap; //4*8=32 = 512/sizeof(struct inode_map)
};

struct inode_map{
    int inode;
    char fname[MAX_FILENAME+1];
};

struct u_fs_disk_block{
    size_t size;
    int nNextBlock;
    char data[MAX_DATA_INBLOCK];
};

static const char *disk_path = "/tmp/diskimg";
static int inode_bit_base = 512; //i节点位图起始位置
static int data_bit_base = 1536; //数据位图起始位置
static int inode_base = 4096;//i节点起始位置
static size_t kill_warn = 0; //由于read和write如果不处理返回值会报警告  为简化处理 加此变量消除而不判断读写结果

static int take_block_bit(int fd,int blk);
static int free_block_bit(int fd,int blk);
static int take_inode_bit(int fd,int inode);
static int free_inode_bit(int fd,int inode);
static int add_inode(int fd,struct u_fs_file_directory *file_dir);
static int init_dir(int fd,int blk,int self,int parent);
static int add_inode_map(int fd,int blk,struct inode_map *map);
static int delete_inode_map(int fd,int blk,struct inode_map *map,int pre);
static int get_map_count(int fd,int blk);
static int get_inode(int fd,int inode,struct u_fs_file_directory *file_dir);
static int search_file(int fd,int blk,char *fname);
static int parse_path(int fd,const char *path,struct u_fs_file_directory *file_dir);
static void my_filler(int fd,int blk,void *buf,fuse_fill_dir_t filler);
static int add_file(int fd,struct u_fs_file_directory *dir,char *fname);
static void free_file_blocks(int fd,int blk);
static size_t append_data(int fd,int cur_blk,const char *buf,size_t size);
/*=============================================================================
 * 继承接口实现部分
 */
static int u_fs_getattr(const char *path, struct stat *stbuf)
{
    int res = 0;
    memset(stbuf, 0, sizeof(struct stat));
	int fd = open(disk_path,O_RDWR);
	struct u_fs_file_directory dir;
	int inode = parse_path(fd,path,&dir);
	if(inode==-1)
	{
		printf("error:file not found!\n");
		close(fd);
		return -ENOENT;
	}
	if(dir.flag==2)
	{
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 0;
	}else if(dir.flag==1)
	{
		stbuf->st_mode = S_IFREG | 0755;
		stbuf->st_nlink = 0;
		stbuf->st_size = dir.fsize;
	}else
	{
		res = -ENOENT;
	}
	if(inode==1)
		stbuf->st_nlink = 2;
	close(fd);
    return res;
}

static int u_fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;
    int res = 0;

	int fd = open(disk_path,O_RDWR);
	struct u_fs_file_directory dir;
	int inode = parse_path(fd,path,&dir);
	if(inode == -1)
	{
		printf("error:directory %s not fount!\n",path);
		res = -ENOENT;
	}else if(dir.flag != 2)
	{
		printf("error:%s is not a directory!\n",path);
		res = -ENOENT;
	}else
	{
		my_filler(fd,dir.nStartBlock,buf,filler);
	}
	close(fd);
    return res;
}

static int u_fs_mkdir(const char *path, mode_t mode)
{
	(void)mode;
	int fd = open(disk_path,O_RDWR);
	char *tpath;
	char dirname[MAX_FILENAME+1];
	char *pt;
	struct u_fs_file_directory dir;
	struct sb sblock;
	lseek(fd,0,SEEK_SET);
	kill_warn = read(fd,&sblock,sizeof(struct sb));
	tpath = malloc(strlen(path)+1);
	strcpy(tpath,path+1);
	strncpy(dirname,path,1);
	if(dirname[0]!='/')
	{
		free(tpath);
		printf("error:path not start with /!\n");
		close(fd);
		return -EPERM;
	}
	if(parse_path(fd,path,&dir)!=-1)
	{
		free(tpath);
		printf("error:director exist!\n");
		close(fd);
		return -EEXIST;
	}

	pt = strchr(tpath,'/');
	if(pt)
	{
		char *test;
		test = malloc(strlen(path)+1);
		strcpy(test,pt+1);
		while(strchr(test,'/')-test==0) strcpy(test,test+1);
		if(strcmp(test,"\0")!=0)
		{
			free(test);
			free(tpath);
			printf("error:director not under root!\n");
			close(fd);
			return -EPERM;
		}
		free(test);
		if(pt-tpath>MAX_FILENAME)
		{
			free(tpath);
			printf("error:director's name too long!\n");
			close(fd);
			return -ENAMETOOLONG;
		}
		strncpy(dirname,tpath,pt-tpath);
		dirname[pt-tpath] = '\0';
	}
	else
	{
		if(strlen(tpath)>MAX_FILENAME)
		{
			free(tpath);
			close(fd);
			return -ENAMETOOLONG;
		}
		strcpy(dirname,tpath);
	}
	free(tpath);
	strcpy(dir.fname,dirname);
	strcpy(dir.fext,"");
	dir.flag = 2;
	int inode = add_inode(fd,&dir);
	if(inode==-1)
	{
		printf("error:can not create inode!\n");
		close(fd);
		return -EPERM;
	}
	struct inode_map map;
	strcpy(map.fname,dir.fname);
	map.inode = inode;
	if(add_inode_map(fd,sblock.first_blk,&map)==-1)
	{
		printf("error:can not create inode_map!\n");
		free_inode_bit(fd,inode);
		close(fd);
		return -EPERM;
	}
	int blk = init_dir(fd,-1,inode,1);
	if(blk==-1)
	{
		printf("error:disk is full!\n");
		free_inode_bit(fd,inode);
		delete_inode_map(fd,sblock.first_blk,&map,-1);
		close(fd);
		return -EPERM;
	}

	lseek(fd,inode_base,SEEK_SET);
	lseek(fd,(inode-1)*sizeof(struct u_fs_file_directory),SEEK_CUR);
	dir.nStartBlock = blk;
	kill_warn = write(fd,&dir,sizeof(struct u_fs_file_directory));

	close(fd);
    return 0;
}

static int u_fs_rmdir(const char *path)
{
	int fd = open(disk_path,O_RDWR);
	struct sb sblock;
	lseek(fd,0,SEEK_SET);
	kill_warn = read(fd,&sblock,sizeof(struct sb));
	struct u_fs_file_directory dir;
	int inode = parse_path(fd,path,&dir);
	if(inode==-1)
	{
		printf("error:directory not found!\n");
		close(fd);
		return -ENOENT;
	}else if(inode == 1)
	{
		printf("error:can not remove root directory!\n");
		close(fd);
		return -ENOTEMPTY;
	}

	if(dir.flag != 2)
	{
		printf("error:%s is not a directory!\n",path);
		close(fd);
		return -ENOTDIR;
	}
	int map_count = get_map_count(fd,dir.nStartBlock);
	if(map_count>2)
	{
		printf("error:can not remove a none empty directory %s!\n",path);
		close(fd);
		return -ENOTEMPTY;
	}

	struct inode_map map;
	strcpy(map.fname,dir.fname);
	map.inode = inode;
	delete_inode_map(fd,sblock.first_blk,&map,-1); //文件夹只存在于根目录中
	free_inode_bit(fd,inode);
	free_block_bit(fd,dir.nStartBlock);

	close(fd);
    return 0;
}

static int u_fs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	(void)mode;
	(void)rdev;
	int fd = open(disk_path,O_RDWR);
	char *tpath,*filename;
	char *pt;
	struct u_fs_file_directory dir;
	int res = 0;

	if(parse_path(fd,path,&dir)!=-1)
	{
		printf("error:can not create new file for file %s exist!\n",path);
		return -EEXIST;
	}

	tpath = malloc(strlen(path)+1);
	filename = malloc(strlen(path)+1);
	pt =  strrchr(path,'/');
	strcpy(filename,pt+1);
	strcpy(tpath,path);
	pt = strrchr(tpath,'/');
	strcpy(pt+1,"\0");
	int inode = parse_path(fd,tpath,&dir);

	if(inode==-1)
	{
		printf("error:directory %s not found!\n",tpath);
		res = -EPERM;
	}else if(inode == 1) //按要求 根目录不能包含文件
	{
		printf("error:can not create file in root directory!\n");
		res = -EPERM;
	}else if(strlen(filename)>MAX_FILENAME)
	{
		printf("error:filename too long!\n");
		res = -ENAMETOOLONG;
	}else if(dir.flag != 2)
	{
		printf("error:%s is not a directory!\n",tpath);
		res = -EPERM;
	}else if(add_file(fd,&dir,filename) == -1)
	{
		printf("error:can not create file %s!\n",path);
		res = -EPERM;
	}

	free(tpath);
	free(filename);
	close(fd);
	return res;
}

static int u_fs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    (void) fi;
	int fd = open(disk_path,O_RDWR);
    struct u_fs_file_directory file;
    int inode = parse_path(fd,path,&file);
    if(inode == -1 || file.flag==0)
    {
    	printf("error:file not fount!\n");
    	close(fd);
    	return -ENOENT;
    }else if(file.flag == 2)
    {
    	printf("error:%s is a directory!\n",path);
    	close(fd);
    	return -EISDIR;
    }

    struct u_fs_disk_block block;
	int cur_blk = file.nStartBlock;
    if(offset<0) offset = 0;
    if(offset >= file.fsize) //加在文件末尾
    {
    	lseek(fd,(cur_blk-1)*512,SEEK_SET);
    	kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
    	while(block.nNextBlock != -1)
    	{
    		cur_blk = block.nNextBlock;
    		lseek(fd,(cur_blk-1)*512,SEEK_SET);
    		kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
    	}
    	int app = append_data(fd,cur_blk,buf,size);
    	file.fsize += app;
    	lseek(fd,inode_base,SEEK_SET);
    	lseek(fd,(inode-1)*sizeof(struct u_fs_file_directory),SEEK_CUR);
    	kill_warn = write(fd,&file,sizeof(struct u_fs_file_directory));
    	close(fd);
    	return app;
    	//return -EFBIG;
    }

    size_t remain = size;
	if(offset+remain>file.fsize)
		remain = file.fsize - offset;
	size_t buf_add = 0; //buf后接位置
	size_t act_size = 0;

	lseek(fd,(cur_blk-1)*512,SEEK_SET);
	kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
	while(offset>=MAX_DATA_INBLOCK)
	{
		if(block.nNextBlock==-1)
		{
			printf("error:wrong offset operation!\n");
			close(fd);
			return 0;
		}
		offset -= MAX_DATA_INBLOCK;
		cur_blk = block.nNextBlock;
		lseek(fd,(cur_blk-1)*512,SEEK_SET);
		kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
	}
	while(remain>0)
	{
		int count = block.size - offset;
		if(count>remain) count = remain;
		memcpy(block.data+offset,buf+buf_add,count);
		lseek(fd,(cur_blk-1)*512,SEEK_SET);
		kill_warn = write(fd,&block,sizeof(struct u_fs_disk_block));
		remain -= count;
		act_size += count;
		buf_add += count;
		offset = 0;
		if(block.nNextBlock ==-1) break;
		cur_blk = block.nNextBlock;
		lseek(fd,(cur_blk-1)*512,SEEK_SET);
		kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
	}

	if(act_size<size)
	{
		int app = append_data(fd,cur_blk,buf+buf_add,size-act_size);
		act_size += app;
		file.fsize += app;
    	lseek(fd,inode_base,SEEK_SET);
    	lseek(fd,(inode-1)*sizeof(struct u_fs_file_directory),SEEK_CUR);
    	kill_warn = write(fd,&file,sizeof(struct u_fs_file_directory));
	}

    close(fd);
    return act_size;
}

static int u_fs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    (void) fi;
	int fd = open(disk_path,O_RDWR);
    struct u_fs_file_directory file;
    int inode = parse_path(fd,path,&file);
    if(inode == -1 || file.flag==0)
    {
    	printf("error:file not fount!\n");
    	close(fd);
    	return -ENOENT;
    }else if(file.flag == 2)
    {
    	printf("error:%s is a directory!\n",path);
    	close(fd);
    	return -EISDIR;
    }

    struct u_fs_disk_block block;
    if(offset<0) offset=0;
    if(offset<file.fsize)
    {
    	if(offset+size>file.fsize)
    		size = file.fsize - offset;
    	size_t remain = size;
    	size_t buf_add = 0; //buf后接位置
    	size_t act_size = 0;
    	int cur_blk = file.nStartBlock;
    	lseek(fd,(cur_blk-1)*512,SEEK_SET);
    	kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
    	while(offset>=MAX_DATA_INBLOCK)
    	{
    		if(block.nNextBlock == -1)
    		{
    			close(fd);
    			return 0;
    		}
    		offset -= MAX_DATA_INBLOCK;
    		cur_blk = block.nNextBlock;
    		lseek(fd,(cur_blk-1)*512,SEEK_SET);
    		kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
    	}
    	while(remain>0)
    	{
    		int count = block.size - offset;
    		if(count>remain) count = remain;
    		memcpy(buf+buf_add,block.data+offset,count);
    		remain -= count;
    		act_size += count;
    		buf_add += count;
    		offset = 0;
    		if(block.nNextBlock ==-1) break;
    		cur_blk = block.nNextBlock;
    		lseek(fd,(cur_blk-1)*512,SEEK_SET);
    		kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
    	}
    	size = act_size;
    }
    else
    	size = 0;

    close(fd);
    return size;
}

static int u_fs_unlink(const char *path)
{
	int fd = open(disk_path,O_RDWR);
	struct u_fs_file_directory file;
	int inode = parse_path(fd,path,&file);
	if(inode==-1 || file.flag==0)
	{
		printf("error:file not found!\n");
		close(fd);
		return -ENOENT;
	}else if(file.flag == 2)
	{
		printf("error:%s is a directory!\n",path);
		close(fd);
		return -EISDIR;
	}

	char *tpath,*pt;
	tpath = malloc(strlen(path)+1);
	strcpy(tpath,path);
	pt = strrchr(tpath,'/');
	strcpy(pt+1,"\0");
	struct u_fs_file_directory dir;
	parse_path(fd,tpath,&dir);

	struct inode_map map;
	strcpy(map.fname,file.fname);
	map.inode = inode;
	delete_inode_map(fd,dir.nStartBlock,&map,-1);
	free_inode_bit(fd,inode);
	free_file_blocks(fd,file.nStartBlock);

	close(fd);
    return 0;
}

static int u_fs_open(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	(void)fi;
    return 0;
}

static int u_fs_flush(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	(void)fi;
    return 0;
}

static int u_fs_truncate(const char *path, off_t size)
{
	(void)path;
	(void)size;
    return 0;
}

static struct fuse_operations u_fs_oper = {
    .getattr	= u_fs_getattr,
    .readdir	= u_fs_readdir,
    .mkdir	= u_fs_mkdir,
    .rmdir	= u_fs_rmdir,
    .mknod	= u_fs_mknod,
    .write	= u_fs_write,
    .read	= u_fs_read,
    .unlink	= u_fs_unlink,
    .open	= u_fs_open,
    .flush	= u_fs_flush,
    .truncate	= u_fs_truncate,
};

/*=============================================================================
 * 基础函数部分
 */

/**
 * 尝试占用一个block bitmap 位
 * blk block bitmap位号，为-1时表示顺序搜索第一个空闲的位
 * 成功：return 占用的bitmap位号
 * 失败：return -1
 */
static int take_block_bit(int fd,int blk)
{
	//printf("take_block_bit %d \n",blk);
	if(blk>MAX_BLOCK) return -1;
	if(blk!=-1)
	{
		int start_bit8 = blk/8;
		int offset = blk%8;
		if(offset==0)
		{
			offset = 8;
			start_bit8--;
		}
		lseek(fd,data_bit_base,SEEK_SET);
		lseek(fd,start_bit8,SEEK_CUR);
		unsigned char bit8;
		kill_warn = read(fd,&bit8,1);
		unsigned char test_bit8 = bit8>>(8-offset);
		test_bit8 = test_bit8 & 1;
		if(test_bit8) return -1;
		test_bit8=1;
		test_bit8 = test_bit8<<(8-offset);
		bit8 = bit8 | test_bit8;
		lseek(fd,-1,SEEK_CUR);
		kill_warn = write(fd,&bit8,1);
		return blk;
	}
	lseek(fd,data_bit_base,SEEK_SET);
	unsigned char bit8;
	kill_warn = read(fd,&bit8,1);
	int start_bit8 = 0;
	while((unsigned char)~bit8==0 && start_bit8<5*512)
	{
		kill_warn = read(fd,&bit8,1);
		start_bit8++;
	}
	if(start_bit8==5*512) return -1;
	int offset = 1;
	unsigned char test_bit8;
	while(offset<=8)
	{
		test_bit8 = bit8>>(8-offset);
		test_bit8 = test_bit8 & 1;
		if(!test_bit8) break;
		offset++;
	}
	test_bit8 = 1;
	test_bit8 = test_bit8<<(8-offset);
	bit8 = bit8 | test_bit8;
	lseek(fd,-1,SEEK_CUR);
	kill_warn = write(fd,&bit8,1);
	return start_bit8*8+offset;
}

/**
 * 回收一个数据块
 * blk 要回收的数据块号
 * 数据块本来非空：return 所回收的数据块号
 * 数据块本来空闲: return -1 表示不用回收
 */
static int free_block_bit(int fd,int blk)
{
	if(blk>MAX_BLOCK) return -1;
	int start_bit8 = blk/8;
	int offset = blk%8;
	if(offset==0)
	{
		offset = 8;
		start_bit8--;
	}
	lseek(fd,data_bit_base,SEEK_SET);
	lseek(fd,start_bit8,SEEK_CUR);
	unsigned char bit8;
	kill_warn = read(fd,&bit8,1);
	unsigned char test_bit8 = bit8>>(8-offset);
	test_bit8 = test_bit8 & 1;
	if(!test_bit8) return -1;
	test_bit8=1;
	test_bit8 = test_bit8<<(8-offset);
	test_bit8 = ~test_bit8;
	bit8 = bit8 & test_bit8;
	lseek(fd,-1,SEEK_CUR);
	kill_warn = write(fd,&bit8,1);
	return blk;
}

/**
 *尝试占用一个inode bitmap位
 *inode inode bitmap位序号 为-1表示顺序搜索第一个空闲位
 *成功：return 占用的inode号
 *失败：return -1
 */
static int take_inode_bit(int fd,int inode)
{
	if(inode>MAX_INODE) return -1;
	if(inode!=-1)
	{
		int start_bit8 = inode/8;
		int offset = inode%8;
		if(offset==0)
		{
			offset = 8;
			start_bit8--;
		}
		lseek(fd,inode_bit_base,SEEK_SET);
		lseek(fd,start_bit8,SEEK_CUR);
		unsigned char bit8;
		kill_warn = read(fd,&bit8,1);
		unsigned char test_bit8 = bit8>>(8-offset);
		test_bit8 = test_bit8 & 1;
		if(test_bit8) return -1;
		test_bit8=1;
		test_bit8 = test_bit8<<(8-offset);
		bit8 = bit8 | test_bit8;
		lseek(fd,-1,SEEK_CUR);
		kill_warn = write(fd,&bit8,1);
		return inode;
	}
	lseek(fd,inode_bit_base,SEEK_SET);
	unsigned char bit8;
	kill_warn = read(fd,&bit8,1);
	int start_bit8 = 0;
	while((unsigned char)~bit8==0 && start_bit8<2*512)
	{
		kill_warn = read(fd,&bit8,1);
		start_bit8++;
	}
	if(start_bit8==2*512) return -1;
	int offset = 1;
	unsigned char test_bit8;
	while(offset<=8)
	{
		test_bit8 = bit8>>(8-offset);
		test_bit8 = test_bit8 & 1;
		if(!test_bit8) break;
		offset++;
	}
	test_bit8 = 1;
	test_bit8 = test_bit8<<(8-offset);
	bit8 = bit8 | test_bit8;
	lseek(fd,-1,SEEK_CUR);
	kill_warn = write(fd,&bit8,1);
	return start_bit8*8+offset;
}

/**
 * 回收一个inode节点
 * 节点非空： 回收该节点 后 return 该节点号
 * 节点空：return -1
 */
static int free_inode_bit(int fd,int inode)
{
	if(inode>MAX_INODE) return -1;
	int start_bit8 = inode/8;
	int offset = inode%8;
	if(offset==0)
	{
		offset = 8;
		start_bit8--;
	}
	lseek(fd,inode_bit_base,SEEK_SET);
	lseek(fd,start_bit8,SEEK_CUR);
	unsigned char bit8;
	kill_warn = read(fd,&bit8,1);
	unsigned char test_bit8 = bit8>>(8-offset);
	test_bit8 = test_bit8 & 1;
	if(!test_bit8) return -1;
	test_bit8=1;
	test_bit8 = test_bit8<<(8-offset);
	test_bit8 = ~test_bit8;
	bit8 = bit8 & test_bit8;
	lseek(fd,-1,SEEK_CUR);
	kill_warn = write(fd,&bit8,1);
	return inode;
}

/**
 * 增加一个inode
 * file_dir 节点信息
 * 成功：return保存所在的inode序号
 * 失败：return -1
 */
static int add_inode(int fd,struct u_fs_file_directory *file_dir)
{
	int inode = take_inode_bit(fd,-1);
	if(inode==-1) return -1;
	lseek(fd,inode_base,SEEK_SET);
	lseek(fd,(inode-1)*sizeof(struct u_fs_file_directory),SEEK_CUR);
	kill_warn = write(fd,file_dir,sizeof(struct u_fs_file_directory));
	return inode;
}

/**
 * 为一个文件夹作初始化工作 包括申请数据块
 * blk 所申请的数据块号 为-1表示顺序找一个空闲的数据块
 * self 该文件夹本身的inode号
 * parent 该文件夹得父亲文件夹的inode号
 * 成功：该文件夹的第一个数据块
 * 失败：-1
 */
static int init_dir(int fd,int blk,int self,int parent)
{
    blk = take_block_bit(fd,blk);
    if(blk == -1) return -1;
	struct dir_sb dir;
	dir.nNextBlock = -1;
	dir.bitmap = 1;
	dir.bitmap = dir.bitmap<<31;
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = write(fd,&dir,sizeof(struct dir_sb));

    struct inode_map self_dir;
    strcpy(self_dir.fname,".");
    self_dir.inode = self;
    add_inode_map(fd,blk,&self_dir);
    struct inode_map parent_dir;
    strcpy(parent_dir.fname,"..");
    parent_dir.inode = parent;
    add_inode_map(fd,blk,&parent_dir);
    return blk;
}

/**
 *增加一个inode映射项
 *当在一个文件夹内新建一个文件或者文件夹时 要将其登记在该文件夹的子文件列表中
 *过程中存在递归调用，已便在文件夹中找到一个空闲位置，或新建一个数据块保存。
 *blk 数据块号
 *map 映射项
 *成功：return 0
 *失败：return -1
 */
static int add_inode_map(int fd,int blk,struct inode_map *map)
{
	struct dir_sb dir;
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = read(fd,&dir,sizeof(struct dir_sb));
	int offset = 2;
	unsigned int test_bit32;
	while(offset<=32)
	{
		test_bit32 = dir.bitmap>>(32-offset);
		test_bit32 = test_bit32 & 1;
		if(!test_bit32) break;
		offset++;
	}
	if(offset<=32) //在本块有空闲位置  放在本块
	{
		lseek(fd,(blk-1)*512,SEEK_SET);
		lseek(fd,(offset-1)*16,SEEK_CUR);
		kill_warn = write(fd,map,sizeof(struct inode_map));
		test_bit32 = 1;
		test_bit32 = test_bit32<<(32-offset);
		dir.bitmap = dir.bitmap | test_bit32;
		lseek(fd,(blk-1)*512,SEEK_SET);
		kill_warn = write(fd,&dir,sizeof(struct dir_sb));
		return 0;
	}

	//本块没有空闲位置了   如果存在下一块 则放到下一块
	if(dir.nNextBlock!=-1)
		return add_inode_map(fd,dir.nNextBlock,map);

	//不存在下一块  建立新块
	int new_blk = take_block_bit(fd,-1);
	if(new_blk == -1) return -1;
	dir.nNextBlock = new_blk;
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = write(fd,&dir,sizeof(struct dir_sb)); //update
	struct dir_sb new_dir;
	new_dir.nNextBlock = -1;
	new_dir.bitmap = 1;
	new_dir.bitmap = new_dir.bitmap<<31;
	lseek(fd,(new_blk-1)*512,SEEK_SET);
	kill_warn = write(fd,&new_dir,sizeof(struct dir_sb));

	return add_inode_map(fd,dir.nNextBlock,map);
}

/**
 * 删除一个子文件或子文件夹映射项
 * 当在一个文件夹中删除一个文件或文件夹，要将所删除的文件或文件夹从该文件夹的列表清除
 * 过程存在递归调用，以便变量该文件夹的所有数据块把子文件映射项删除
 * 当删除操作导致某个数据为空  要进行回收操作
 * 成功：return 0
 * 失败：return -1 （不存在该子文件或子文件夹）
 */
static int delete_inode_map(int fd,int blk,struct inode_map *map,int pre)
{
	struct dir_sb dir;
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = read(fd,&dir,sizeof(struct dir_sb));
	int offset = 2;
	unsigned int test_bit32;
	int blk_map_count = 0;
	while(offset<=32)
	{
		test_bit32 = dir.bitmap>>(32-offset);
		test_bit32 = test_bit32 & 1;
		if(test_bit32)
			blk_map_count++;
		offset++;
	}
	offset = 2;
	while(offset<=32)
	{
		test_bit32 = dir.bitmap>>(32-offset);
		test_bit32 = test_bit32 & 1;
		if(test_bit32)
		{
			lseek(fd,(blk-1)*512,SEEK_SET);
			lseek(fd,(offset-1)*16,SEEK_CUR);
			struct inode_map tmap;
			kill_warn = read(fd,&tmap,sizeof(struct inode_map));
			if(strcmp(map->fname,tmap.fname)==0 && map->inode==tmap.inode)
				break;
		}
		offset++;
	}
	if(offset<=32) //在本块找到了  删除
	{
		test_bit32 = 1;
		test_bit32 = test_bit32<<(32-offset);
		test_bit32 = ~test_bit32;
		dir.bitmap = dir.bitmap & test_bit32;
		lseek(fd,(blk-1)*512,SEEK_SET);
		kill_warn = write(fd,&dir,sizeof(struct dir_sb));
		blk_map_count--;
		if(blk_map_count==0 && pre!=-1) //该块没有记录了 删除该块
		{
			free_block_bit(fd,blk);
			struct dir_sb pre_blk;
			lseek(fd,(pre-1)*512,SEEK_SET);
			kill_warn = read(fd,&pre_blk,sizeof(struct dir_sb));
			pre_blk.nNextBlock = dir.nNextBlock;
			lseek(fd,(pre-1)*512,SEEK_SET);
			kill_warn = write(fd,&pre_blk,sizeof(struct dir_sb));
		}
		return 0;
	}

	//在本块找不到   如果存在下一块 则到下一块处理
	if(dir.nNextBlock!=-1)
		return delete_inode_map(fd,dir.nNextBlock,map,blk);
	return -1;
}

/**
 * 获取一个文件夹下的子文件和子文件夹数量
 * 递归调用求总数
 * return 子文件和子文件夹总数
 */
static int get_map_count(int fd,int blk)
{
	if(blk==-1) return 0;
	struct dir_sb dir;
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = read(fd,&dir,sizeof(struct dir_sb));
	int count = 0;
	int offset = 2;
	unsigned int test_bit32;
	while(offset<=32)
	{
		test_bit32 = dir.bitmap>>(32-offset);
		test_bit32 = test_bit32 & 1;
		if(test_bit32)
			count++;
		offset++;
	}
	count = count + get_map_count(fd,dir.nNextBlock);
	return count;
}

/**
 * 初始化
 */
static int init(void)
{
    int fd = open(disk_path,O_RDWR);
    if(fd==-1)
    {
    	printf("error:initialize error.can not open file %s!\n",disk_path);
    	exit(1);
    }
    struct sb sblock;
    sblock.fs_size = MAX_BLOCK;
    int inode_bytes = sizeof(struct u_fs_file_directory)*MAX_INODE;
    int inode_bs = inode_bytes/512;
    if(inode_bytes%512!=0) inode_bs++;
    sblock.first_blk = 1+2+5+inode_bs+1;
    sblock.bitmap = MAX_BLOCK;
    kill_warn = write(fd,&sblock,sizeof(struct sb));

    //设置数据部分bitmap
    int j = 1;
    while(j<sblock.first_blk) //把前面的基础性模块登记为已用
    {
    	take_block_bit(fd,j++);
    }
    unsigned char last_bit8 = ~0;
    lseek(fd,data_bit_base,SEEK_SET);
    lseek(fd,5*512-1,SEEK_CUR);
    kill_warn = write(fd,&last_bit8,1); //设置最后8个数据块为不可用

    //设置第一个inode 根目录
    struct u_fs_file_directory root_dir;
    strcpy(root_dir.fname,".");
    strcpy(root_dir.fext,""); //文件拓展信息简化处理  不实现
    root_dir.fsize = 0;
    root_dir.nStartBlock = sblock.first_blk;
    root_dir.flag = 2;
    int root_inode = add_inode(fd,&root_dir);
    if(root_inode==-1)
    {
    	printf("error:initialize error!\n");
    	exit(1);
    }

    init_dir(fd,root_dir.nStartBlock,root_inode,root_inode);

    close(fd);
    return 0;
}

/**
 *获取一个inode项
 * inode inode的序号
 * file_dir 保存取得的inode项的变量
 * return 0；
 */
static int get_inode(int fd,int inode,struct u_fs_file_directory *file_dir)
{
	lseek(fd,inode_base,SEEK_SET);
	lseek(fd,(inode-1)*sizeof(struct u_fs_file_directory),SEEK_CUR);
	kill_warn = read(fd,file_dir,sizeof(struct u_fs_file_directory));
	return 0;
}

/**
 * 查找文件
 * fname 文件或文件夹名字
 * 成功:return 所在inode编号
 * 失败:return -1
 */
static int search_file(int fd,int blk,char *fname)
{
	struct dir_sb dir;
	struct inode_map map;
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = read(fd,&dir,sizeof(struct dir_sb));
	unsigned int test_bit32;
	int offset = 2;
	while(offset<=32)
	{
		test_bit32 = dir.bitmap>>(32-offset);
		test_bit32 = test_bit32 & 1;
		if(test_bit32)
		{
			lseek(fd,(blk-1)*512,SEEK_SET);
			lseek(fd,(offset-1)*16,SEEK_CUR);
			kill_warn = read(fd,&map,sizeof(struct inode_map));
			if(strcmp(map.fname,fname) == 0)
				return map.inode;
		}
		offset++;
	}
	if(dir.nNextBlock != -1)
		return search_file(fd,dir.nNextBlock,fname);
	return -1;
}

/**
 * 解析路径
 * path路径
 * file_dir结果保存位置
 * 成功：return 所在inode序号
 * 失败：return -1 不存该在路径或文件
 */
static int parse_path(int fd,const char *path,struct u_fs_file_directory *file_dir)
{
	char *tpath;
	char fname[256];
	char *pt;
	tpath = malloc(strlen(path)+1);
	strcpy(tpath,path);
	strncpy(fname,tpath,1);
	if(fname[0]!='/')
	{
		return -1;
	}
	int inode = 1;
	get_inode(fd,1,file_dir);
	strcpy(tpath,tpath+1);
	while(strcmp(tpath,"\0")!=0)
	{
		pt = strchr(tpath,'/');
		if(pt)
		{
			strncpy(fname,tpath,pt-tpath);
			fname[pt-tpath] = '\0';
			strcpy(tpath,pt+1);
		}
		else
		{
			strcpy(fname,tpath);
			strcpy(tpath,"\0");
		}
		if(file_dir->flag != 2)
		{
			free(tpath);
			return -1; //不是文件夹的没有子文件和子文件夹
		}
		inode = search_file(fd,file_dir->nStartBlock,fname);
		if(inode == -1)
		{
			free(tpath);
			return -1;
		}
		get_inode(fd,inode,file_dir);
	}
	free(tpath);
	return inode;
}

/**
 * filler 填充目录的子文件和子文件夹
 * 递归目录的所有数据块进行填充。
 */
static void my_filler(int fd,int blk,void *buf,fuse_fill_dir_t filler)
{
	if(blk==-1) return;
	struct dir_sb dir;
	struct inode_map map;
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = read(fd,&dir,sizeof(struct dir_sb));
	unsigned int test_bit32;
	int offset = 2;
	while(offset<=32)
	{
		test_bit32 = dir.bitmap>>(32-offset);
		test_bit32 = test_bit32 & 1;
		if(test_bit32)
		{
			lseek(fd,(blk-1)*512,SEEK_SET);
			lseek(fd,(offset-1)*16,SEEK_CUR);
			kill_warn = read(fd,&map,sizeof(struct inode_map));
			filler(buf,map.fname,NULL,0);
		}
		offset++;
	}
	if(dir.nNextBlock != -1)
		return my_filler(fd,blk,buf,filler);
	return ;
}

/**
 * 新建一个文件
 * dir 所在目录
 * 成功：return 0；
 * 失败：return -1；
 */
static int add_file(int fd,struct u_fs_file_directory *dir,char *fname)
{
	struct u_fs_disk_block file_blk;
	file_blk.size = 0;
	file_blk.nNextBlock = -1;
	int blk = take_block_bit(fd,-1);
	if(blk==-1)
	{
		printf("error:disk is full!\n");
		return -1;
	}
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = write(fd,&file_blk,sizeof(struct u_fs_disk_block));

	struct u_fs_file_directory newfile;
	strcpy(newfile.fname,fname);
	strcpy(newfile.fext,"");
	newfile.flag = 1;
	newfile.fsize = 0;
	newfile.nStartBlock = blk;
	int inode = add_inode(fd,&newfile);
	if(inode==-1)
	{
		printf("error:can not create inode!\n");
		free_block_bit(fd,blk);
		return -1;
	}

	struct inode_map map;
	strcpy(map.fname,fname);
	map.inode = inode;
	if(add_inode_map(fd,dir->nStartBlock,&map)==-1)
	{
		printf("error:can not create inode_map!\n");
		free_block_bit(fd,blk);
		free_inode_bit(fd,inode);
		return -1;
	}

	return 0;
}

/**
 * 回收某文件所占得所有数据块
 * 当删除一个文件时要对其占用的所有数据块进行回收
 */
static void free_file_blocks(int fd,int blk)
{
	if(blk==-1) return;
	free_block_bit(fd,blk);
	struct u_fs_disk_block block;
	lseek(fd,(blk-1)*512,SEEK_SET);
	kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));
	if(block.nNextBlock != -1)
		free_file_blocks(fd,block.nNextBlock);
	return;
}

/**
 * 把数据加到某文件末尾 append
 * cur_blk 当前最后一个数据块号
 * buf 要加入的数据
 * size 要加入的数据长度
 */
static size_t append_data(int fd,int cur_blk,const char *buf,size_t size)
{
	if(cur_blk == -1 || size==0) return 0;

	struct u_fs_disk_block block;
	lseek(fd,(cur_blk-1)*512,SEEK_SET);
	kill_warn = read(fd,&block,sizeof(struct u_fs_disk_block));

	size_t count = MAX_DATA_INBLOCK - block.size;
	if(count>size) count = size;
	memcpy(block.data+block.size,buf,count);
	size -= count;
	buf += count;
	block.size += count;
	if(size!=0)
	{
		int new_blk = take_block_bit(fd,-1);
		if(new_blk!=-1)
		{
			block.nNextBlock = new_blk;
			struct u_fs_disk_block block_next;
			block_next.size = 0;
			block_next.nNextBlock = -1;
			lseek(fd,(new_blk-1)*512,SEEK_SET);
			kill_warn = write(fd,&block_next,sizeof(struct u_fs_disk_block));
		}
	}
	lseek(fd,(cur_blk-1)*512,SEEK_SET);
	kill_warn = write(fd,&block,sizeof(struct u_fs_disk_block));
	return count+append_data(fd,block.nNextBlock,buf,size);
}

int main(int argc, char *argv[])
{
    init();
    return fuse_main(argc,argv,&u_fs_oper,NULL);
}
