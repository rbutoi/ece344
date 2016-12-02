#include "testfs.h"
#include "list.h"
#include "super.h"
#include "block.h"
#include "inode.h"

/* given logical block number, read the corresponding physical block into block.
 * return physical block number.
 * returns 0 if physical block does not exist.
 * returns negative value on other errors. */
static int
testfs_read_block(struct inode *in, int log_block_nr, char *block)
{
	int phy_block_nr = 0;

	assert(log_block_nr >= 0);
	if (log_block_nr < NR_DIRECT_BLOCKS) {
		phy_block_nr = (int)in->in.i_block_nr[log_block_nr];
	} else {
		log_block_nr -= NR_DIRECT_BLOCKS;

		if (in->in.i_indirect > 0) {
			read_blocks(in->sb, block, in->in.i_indirect, 1);
			phy_block_nr = ((int *)block)[log_block_nr];
		}
		log_block_nr -= NR_INDIRECT_BLOCKS;

		if (in->in.i_dindirect > 0) {
			assert(log_block_nr >= 0);

			read_blocks(in->sb, block, in->in.i_dindirect, 1);
			int indirect_block_phy_nr = ((int *)block)[log_block_nr / NR_INDIRECT_BLOCKS];
			read_blocks(in->sb, block, indirect_block_phy_nr, 1);
			phy_block_nr = ((int *)block)[log_block_nr % NR_INDIRECT_BLOCKS];
		}
	}
	if (phy_block_nr > 0) {
		read_blocks(in->sb, block, phy_block_nr, 1);
	} else {
		/* we support sparse files by zeroing out a block that is not
		 * allocated on disk. */
		bzero(block, BLOCK_SIZE);
	}
	return phy_block_nr;
}

int
testfs_read_data(struct inode *in, char *buf, off_t start, size_t size)
{
	char block[BLOCK_SIZE];
	long block_nr = start / BLOCK_SIZE;
	long block_ix = start % BLOCK_SIZE;
	int ret;
	long read = 0;

	assert(buf);
	if (start + (off_t) size > in->in.i_size) { /* clamp read to end of file */
		size = in->in.i_size - start;
	}

	if (block_ix + size <= BLOCK_SIZE) {
		/* just copy one block */
		if ((ret = testfs_read_block(in, block_nr, block)) < 0)
			return ret;
		memcpy(buf, block + block_ix, size);
		read += size;
	} else {
		/* copy multiple blocks */
		while ((unsigned) read != size) {
			assert((unsigned) read < size);

			block_nr = (start + read) / BLOCK_SIZE;
			block_ix = (start + read) % BLOCK_SIZE;

			if ((ret = testfs_read_block(in, block_nr, block)) < 0)
				return ret;
			long to_read = MIN((unsigned) (size - read), (unsigned) (BLOCK_SIZE - block_ix));
			memcpy(buf + read, block + block_ix, to_read);
			read += to_read;
		}
	}
	/* return the number of bytes read or any error */
	return read;
}

/* given logical block number, allocate a new physical block, if it does not
 * exist already, and return the physical block number that is allocated.
 * returns negative value on error. */
static int
testfs_allocate_block(struct inode *in, int log_block_nr, char *block)
{
	int phy_block_nr;
	char buf[BLOCK_SIZE];

	assert(log_block_nr >= 0);
	phy_block_nr = testfs_read_block(in, log_block_nr, block);

	/* phy_block_nr > 0: block exists, so we don't need to allocate it, 
	   phy_block_nr < 0: some error */
	if (phy_block_nr != 0)
		return phy_block_nr;

	/* allocate a direct block */
	if (log_block_nr < NR_DIRECT_BLOCKS) {
		assert(in->in.i_block_nr[log_block_nr] == 0);
		phy_block_nr = testfs_alloc_block_for_inode(in);
		if (phy_block_nr >= 0) {
			in->in.i_block_nr[log_block_nr] = phy_block_nr;
		}
		return phy_block_nr;
	}
	log_block_nr -= NR_DIRECT_BLOCKS;

	if (log_block_nr < NR_INDIRECT_BLOCKS) {
		int indirect_allocated = 0;
		if (in->in.i_indirect == 0) {	/* allocate an indirect block */
			bzero(buf, BLOCK_SIZE);
			phy_block_nr = testfs_alloc_block_for_inode(in);
			if (phy_block_nr < 0)
				return phy_block_nr;
			indirect_allocated = 1;
			in->in.i_indirect = phy_block_nr;
		} else {	/* read indirect block */
			read_blocks(in->sb, buf, in->in.i_indirect, 1);
		}

		/* allocate direct block */
		assert(((int *)buf)[log_block_nr] == 0);	
		phy_block_nr = testfs_alloc_block_for_inode(in);

		if (phy_block_nr >= 0) {
			/* update indirect block */
			((int *)buf)[log_block_nr] = phy_block_nr;
			write_blocks(in->sb, buf, in->in.i_indirect, 1);
		} else if (indirect_allocated) {
			/* free the indirect block that was allocated */
			testfs_free_block_from_inode(in, in->in.i_indirect);
			in->in.i_indirect = 0;
		}
		return phy_block_nr;
	}
	log_block_nr -= NR_INDIRECT_BLOCKS;

	if (log_block_nr >= 0) {
		int dindirect_allocated = 0;
		if (in->in.i_dindirect == 0) {    /* allocate a dindirect block */
			bzero(buf, BLOCK_SIZE);
			phy_block_nr = testfs_alloc_block_for_inode(in);
			if (phy_block_nr < 0)
				return phy_block_nr;
			dindirect_allocated = 1;
			in->in.i_dindirect = phy_block_nr;
		} else {
			read_blocks(in->sb, buf, in->in.i_dindirect, 1);
		}

		/* buf has dindirect block */

		int indirect_allocated = 0;
		long idx_in_dindirect = log_block_nr / NR_INDIRECT_BLOCKS;
		assert(idx_in_dindirect < NR_INDIRECT_BLOCKS);
		long indirect_block = ((int *)buf)[idx_in_dindirect];
		if (!indirect_block) {
			/* must needs alloc an indirect block */
			indirect_block = testfs_alloc_block_for_inode(in);
			if (indirect_block < 0) {
				if (dindirect_allocated) {
					/* free the dindirect block that was allocated */
					testfs_free_block_from_inode(in, in->in.i_dindirect);
					in->in.i_dindirect = 0;
				}
				return indirect_block;
			}
			indirect_allocated = 1;
			((int *)buf)[idx_in_dindirect] = indirect_block;
			write_blocks(in->sb, buf, in->in.i_dindirect, 1);

			bzero(buf, BLOCK_SIZE);
		} else {
			read_blocks(in->sb, buf, indirect_block, 1);
		}

		/* buf has indirect block */

		long idx_in_indirect = log_block_nr % NR_INDIRECT_BLOCKS;
		
		/* allocate direct block */
		assert(((int *)buf)[idx_in_indirect] == 0);
		phy_block_nr = testfs_alloc_block_for_inode(in);

		if (phy_block_nr >= 0) {
			/* update indirect block */
			((int *)buf)[idx_in_indirect] = phy_block_nr;
			write_blocks(in->sb, buf, indirect_block, 1);
		} else if (indirect_allocated) {
			/* free the dindirect & indirect block that was allocated */
			testfs_free_block_from_inode(in, indirect_block);
			((int *)buf)[idx_in_dindirect] = 0;
			write_blocks(in->sb, buf, in->in.i_dindirect, 1);

			if (dindirect_allocated) {
				testfs_free_block_from_inode(in, in->in.i_dindirect);
				in->in.i_dindirect = 0;
			}
		}
		return phy_block_nr;
	}

	assert(0);
	return -1;
}

int
testfs_write_data(struct inode *in, const char *buf, off_t start, size_t size)
{
	char block[BLOCK_SIZE];
	long block_nr = start / BLOCK_SIZE;
	long block_ix = start % BLOCK_SIZE;
	u64 max_file_size = ((u64) NR_DIRECT_BLOCKS + NR_INDIRECT_BLOCKS +
	                     NR_INDIRECT_BLOCKS * NR_INDIRECT_BLOCKS) * BLOCK_SIZE;
	int ret;
	long written = 0;

	if (block_ix + size <= BLOCK_SIZE) {
		/* write to single block */

		/* ret is the newly allocated physical block number */
		if ((ret = testfs_allocate_block(in, block_nr, block)) < 0)
			return ret;
		memcpy(block + block_ix, buf, size);
		write_blocks(in->sb, block, ret, 1);
		written = size;
	} else {
		/* write across multiple blocks */
		while ((unsigned) written != size) {
			assert((unsigned) written < size);

			block_nr = (start + written) / BLOCK_SIZE;
			block_ix = (start + written) % BLOCK_SIZE;

			if ((ret = testfs_allocate_block(in, block_nr, block)) < 0)
				return ret;
			long to_write = MIN((signed) (size - written), (signed) (BLOCK_SIZE - block_ix));
			if ((unsigned) (start + written + to_write) > max_file_size) {
				return -EFBIG;
			}

			memcpy(block + block_ix, buf + written, to_write);
			write_blocks(in->sb, block, ret, 1);
			written += to_write;
		}
	}
	/* increment i_size by the number of bytes written. */
	in->in.i_size = MAX(in->in.i_size, start + written);
	in->i_flags |= I_FLAGS_DIRTY;
	/* return the number of bytes written or any error */
	return written;
}

int
testfs_free_blocks(struct inode *in)
{
	int i;
	int e_block_nr;
	char block[BLOCK_SIZE];

	/* last block number */
	e_block_nr = DIVROUNDUP(in->in.i_size, BLOCK_SIZE);

	/* remove direct blocks */
	for (i = 0; i < e_block_nr && i < NR_DIRECT_BLOCKS; i++) {
		if (in->in.i_block_nr[i] == 0)
			continue;
		testfs_free_block_from_inode(in, in->in.i_block_nr[i]);
		in->in.i_block_nr[i] = 0;
	}
	e_block_nr -= NR_DIRECT_BLOCKS;

	/* remove indirect blocks */
	if (in->in.i_indirect > 0) {
		read_blocks(in->sb, block, in->in.i_indirect, 1);
		for (i = 0; i < e_block_nr && i < NR_INDIRECT_BLOCKS; i++) {
			testfs_free_block_from_inode(in, ((int *)block)[i]);
			((int *)block)[i] = 0;
		}
		testfs_free_block_from_inode(in, in->in.i_indirect);
		in->in.i_indirect = 0;
	}

	e_block_nr -= NR_INDIRECT_BLOCKS;
	if (e_block_nr >= 0) {
		char indirect_block[BLOCK_SIZE];
		int j;

		read_blocks(in->sb, block, in->in.i_dindirect, 1);
		for (i = 0; i < NR_INDIRECT_BLOCKS; ++i) {
			if (((int *)block)[i]) {
				read_blocks(in->sb, indirect_block, ((int *)block)[i], 1);
				for (j = 0; (i * NR_INDIRECT_BLOCKS + j) < e_block_nr && i < NR_INDIRECT_BLOCKS; ++j) {
					if (((int *)indirect_block)[j]) {
						testfs_free_block_from_inode(in, ((int *)indirect_block)[j]);
						((int *)indirect_block)[j] = 0;
					}
				}
				testfs_free_block_from_inode(in, ((int *)block)[i]);
				((int *)block)[i] = 0;
			}
		}
		testfs_free_block_from_inode(in, in->in.i_dindirect);
		in->in.i_dindirect = 0;
	}

	in->in.i_size = 0;
	in->i_flags |= I_FLAGS_DIRTY;
	return 0;
}
