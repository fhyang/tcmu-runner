/*
 * Copyright 2015, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
*/

#define _GNU_SOURCE

#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <scsi/scsi.h>
#include <glusterfs/api/glfs.h>

#include "tcmu-runner.h"

#define ALLOWED_BSOFLAGS (O_SYNC | O_DIRECT | O_RDWR | O_LARGEFILE)

#define GLUSTER_PORT 24007

struct glfs_state {
	char *name;
	glfs_t *fs;
	glfs_fd_t *gfd;
	char *servername;
	char *volname;
	char *pathname;

	/*
	 * Current tcmu helper API reports WCE=1, but doesn't
	 * implement inquiry VPD 0xb2, so clients will not know UNMAP
	 * or WRITE_SAME are supported. TODO: fix this
	 */
};

/*
 * Break image string into server, volume, and path components.
 * Returns -1 on failure.
 */
static int parse_imagepath(
	char *cfgstring,
	char **servername,
	char **volname,
	char **pathname)
{
	char *origp = strdup(cfgstring);
	char *t_servername = NULL;
	char *t_volname = NULL;
	char *t_pathname = NULL;
	char *p, *sep;

	if (!origp)
		goto fail;

	/* part before '@' is the volume name */
	p = origp;
	sep = strchr(p, '@');
	if (!sep)
		goto fail;

	*sep = '\0';
	t_volname = strdup(p);
	if (!t_volname)
		goto fail;

	/* part between '@' and 1st '/' is the server name */
	p = sep + 1;
	sep = strchr(p, '/');
	if (!sep)
		goto fail;

	*sep = '\0';
	t_servername = strdup(p);
	if (!t_servername)
		goto fail;

	/* The rest is the path name */
	p = sep + 1;
	t_pathname = strdup(p);
	if (!t_pathname)
		goto fail;

	if (!strlen(t_servername) || !strlen(t_volname) || !strlen(t_pathname))
		goto fail;

	free(origp);
	*servername = t_servername;
	*volname = t_volname;
	*pathname = t_pathname;

	return 0;

fail:
	free(t_volname);
	free(t_servername);
	free(t_pathname);
	free(origp);

	return -1;
}

static glfs_t * tcmu_create_glfs_object(char *config, char *hostaddr,
                                        char *volname, char *path)
{
    glfs_t *fs =  NULL;
    int ret = -1;

	if (parse_imagepath(config, &hostaddr, &volname, &path) == -1) {
		errp("hostaddr, volname, or path missing\n");
		goto fail;
	}

	fs = glfs_new(volname);
	if (!fs) {
		errp("glfs_new failed\n");
		goto fail;
	}

	ret = glfs_set_volfile_server(fs, "tcp", hostaddr, GLUSTER_PORT);
	if (ret) {
		errp("glfs_set_volfile_server failed: %m\n");
		goto fail;
	}


	ret = glfs_init(fs);
	if (ret) {
		errp("glfs_init failed: %m\n");
		goto fail;
	}

    return fs;

 fail:
	if (fs)
		glfs_fini(fs);

    return NULL;
}

static bool glfs_check_config(const char *cfgstring, char **reason)
{
	char *path;
	char *servername = NULL;
	char *volname = NULL;
	char *pathname = NULL;
	glfs_t *fs = NULL;
	glfs_fd_t *gfd = NULL;
	bool result = true;

	path = strchr(cfgstring, '/');
	if (!path) {
		if (asprintf(reason, "No path found") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}
	path += 1; /* get past '/' */

	fs = tcmu_create_glfs_object(path, servername, volname, pathname);
	if (!fs) {
		errp("tcmu_create_glfs_object failed\n");
		goto done;
	}

	gfd = glfs_open(fs, pathname, ALLOWED_BSOFLAGS);
	if (!gfd) {
		if (asprintf(reason, "glfs_open failed: %m") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}

	if (glfs_access(fs, path, R_OK|W_OK) == -1) {
		if (asprintf(reason, "glfs_access file not present, or not writable") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}

done:
	if (gfd)
		glfs_close(gfd);
	if (fs)
		glfs_fini(fs);
	free(servername);
	free(volname);
	free(pathname);

	return result;
}

static int tcmu_glfs_open(struct tcmu_device *dev)
{
	struct glfs_state *gfsp;
	int ret = 0;
	char *config;
	struct stat st;
	int block_size;
	int64_t size;

	gfsp = calloc(1, sizeof(*gfsp));
	if (!gfsp)
		return -ENOMEM;

	tcmu_set_dev_private(dev, gfsp);

	block_size = tcmu_get_attribute(dev, "hw_block_size");
	if (block_size < 0) {
		errp("Could not get hw_block_size setting\n");
		goto fail;
	}
	tcmu_set_dev_block_size(dev, block_size);

	size = tcmu_get_device_size(dev);
	if (size < 0) {
		errp("Could not get device size\n");
		goto fail;
	}
	tcmu_set_dev_num_lbas(dev, size / block_size);

	config = strchr(tcmu_get_dev_cfgstring(dev), '/');
	if (!config) {
		errp("no configuration found in cfgstring\n");
		goto fail;
	}
	config += 1; /* get past '/' */

	gfsp->fs = tcmu_create_glfs_object(config, gfsp->servername, gfsp->volname, gfsp->pathname);
	if (!gfsp->fs) {
		errp("tcmu_create_glfs_object failed\n");
		goto fail;
	}

	gfsp->gfd = glfs_open(gfsp->fs, gfsp->pathname, ALLOWED_BSOFLAGS);
	if (!gfsp->gfd) {
		errp("glfs_open failed: %m\n");
		goto fail;
	}

	ret = glfs_lstat(gfsp->fs, gfsp->pathname, &st);
	if (ret) {
		errp("glfs_lstat failed: %m\n");
		goto fail;
	}

	if (st.st_size != tcmu_get_device_size(dev)) {
		errp("device size and backing size disagree: "
		       "device %lld backing %lld\n",
		       tcmu_get_device_size(dev),
		       (long long) st.st_size);
		goto fail;
	}

	return 0;

fail:
	if (gfsp->gfd)
		glfs_close(gfsp->gfd);
	if (gfsp->fs)
		glfs_fini(gfsp->fs);
	free(gfsp->volname);
	free(gfsp->pathname);
	free(gfsp->servername);
	free(gfsp);

	return -EIO;
}

static void tcmu_glfs_close(struct tcmu_device *dev)
{
	struct glfs_state *gfsp = tcmu_get_dev_private(dev);

	glfs_close(gfsp->gfd);
	glfs_fini(gfsp->fs);
	free(gfsp->volname);
	free(gfsp->pathname);
	free(gfsp->servername);
	free(gfsp);
}

static ssize_t tcmu_glfs_read(struct tcmu_device *dev, struct iovec *iov,
			      size_t iov_cnt, off_t offset)
{
        struct glfs_state *state = tcmu_get_dev_private(dev);

        return glfs_preadv(state->gfd, iov, iov_cnt, offset, SEEK_SET);
}

static ssize_t tcmu_glfs_write(struct tcmu_device *dev, struct iovec *iov,
			       size_t iov_cnt, off_t offset)
{
	struct glfs_state *state = tcmu_get_dev_private(dev);

        return glfs_pwritev(state->gfd, iov, iov_cnt, offset, ALLOWED_BSOFLAGS);
}

static int tcmu_glfs_flush(struct tcmu_device *dev)
{
	struct glfs_state *state = tcmu_get_dev_private(dev);

	return glfs_fdatasync(state->gfd);
}

static const char glfs_cfg_desc[] =
	"glfs config string is of the form:\n"
	"\"volume@hostname/filename\"\n"
	"where:\n"
	"  volume:    The volume on the Gluster server\n"
	"  hostname:  The server's hostname\n"
	"  filename:  The backing file";

struct tcmur_handler glfs_handler = {
	.name 		= "Gluster glfs handler",
	.subtype 	= "glfs",
	.cfg_desc	= glfs_cfg_desc,

	.check_config 	= glfs_check_config,

	.open 		= tcmu_glfs_open,
	.close 		= tcmu_glfs_close,
	.read 		= tcmu_glfs_read,
	.write		= tcmu_glfs_write,
	.flush		= tcmu_glfs_flush,
};

/* Entry point must be named "handler_init". */
int handler_init(void)
{
	return tcmur_register_handler(&glfs_handler);
}
