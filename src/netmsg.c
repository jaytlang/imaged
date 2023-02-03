/* imaged network rpc receipt system
 * (c) jay lang 2023
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "imaged.h"

#define NETMSG_ERRSTRSIZE	500

/* disk file metadata */

struct msgfile {
	uint64_t		 fileid;
	SLIST_ENTRY(msgfile)	 entries;
};

SLIST_HEAD(msgfilelist, msgfile);

static char	*msgfile_reservepath(void);
static void	 msgfile_releasepath(char *);

static struct msgfilelist	freefiles = SLIST_HEAD_INITIALIZER(&freefiles);
static uint64_t			maxfileid = 0;


/* netmsg proper */


struct netmsg {
	uint8_t	 	  opcode;
	char		 *path;
	int	 	  descriptor;

	int		(*closestorage)(int);
	ssize_t		(*readstorage)(int, void *, size_t);
	ssize_t		(*writestorage)(int, const void *, size_t);
	off_t		(*seekstorage)(int, off_t, int);
	int		(*truncatestorage)(int, off_t);

	char		  errstr[NETMSG_ERRSTRSIZE];
};

static int	netmsg_getclaimedlabelsize(struct netmsg *, uint64_t *);
static int	netmsg_getclaimeddatasize(struct netmsg *, uint64_t *);
static ssize_t	netmsg_getexpectedsizeifvalid(struct netmsg *);

static void	netmsg_committype(struct netmsg *);

static char *
msgfile_reservepath(void)
{
	uint64_t	 newid;
	char		*newpath = NULL;

	if (SLIST_EMPTY(&freefiles)) {
		if (maxfileid == UINT64_MAX) {
			errno = EMFILE;
			goto end;
		}

		newid = maxfileid++;
	} else {
		struct msgfile	*newfile;

		newfile = SLIST_FIRST(&freefiles);
		SLIST_REMOVE_HEAD(&freefiles, entries);

		newid = newfile->fileid;
		free(newfile);
	}

	asprintf(&newpath, "%s/%llu", MESSAGES, newid);
end:
	return newpath;
}

static void
msgfile_releasepath(char *oldpath)
{
	struct msgfile	*freefile;
	uint64_t	 oldid;

	if (sscanf(oldpath, MESSAGES "/%llu", &oldid) != 1)
		log_fatalx("msgfile_free: sscanf on %s failed to extract file id");

	free(oldpath);

	freefile = malloc(sizeof(struct msgfile));
	if (freefile == NULL)
		log_fatalx("msgfile_free: failed to allocate free file description");

	freefile->fileid = oldid;
	SLIST_INSERT_HEAD(&freefiles, freefile, entries);
}

struct netmsg *
netmsg_new(uint8_t opcode)
{
	struct netmsg	*out = NULL;
	char		*path = NULL;
	int		 descriptor = -1;
	int		 error = 0;

	int		 diskmsg = 0;
	int		 flags = O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC;
	mode_t		 mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

	switch (opcode) {
	case NETOP_WRITE:
	case NETOP_BUNDLE:

		diskmsg = 1;
		path = msgfile_reservepath();

		if (path == NULL) {
			error = 1;
			goto end;
		}

		descriptor = open(path, flags, mode);
		if (descriptor < 0) {
			error = 1;
			goto end;
		}

		break;

	case NETOP_SIGN:
	case NETOP_HEARTBEAT:
	case NETOP_ACK:
	case NETOP_ERROR:
		descriptor = buffer_open();
		break;

	default:
		errno = EINVAL;
		goto end;
	}

	out = calloc(1, sizeof(struct netmsg));
	if (out == NULL) goto end;

	out->opcode = opcode;
	out->descriptor = descriptor;
	out->path = path;

	if (diskmsg) {
		out->closestorage = close;
		out->readstorage = read;
		out->writestorage = write;
		out->seekstorage = lseek;
		out->truncatestorage = ftruncate;
	} else {
		out->closestorage = buffer_close;
		out->readstorage = buffer_read;
		out->writestorage = buffer_write;
		out->seekstorage = buffer_seek;
		out->truncatestorage = buffer_truncate;
	}

	/* ensure that the struct stays consistent
	 * with the marshalled in-memory data
	 */
	netmsg_committype(out);

end:
	if (error) {
		if (descriptor >= 0) {
			if (diskmsg) close(descriptor);
			else buffer_close(descriptor);
		}

		if (path != NULL) {
			if (diskmsg) unlink(path);
			msgfile_releasepath(path);
		}
	}

	return out;
}

void
netmsg_teardown(struct netmsg *m)
{
	m->closestorage(m->descriptor);

	if (m->path != NULL) {
		unlink(m->path);
		msgfile_releasepath(m->path);
	}

	free(m);
}

const char *
netmsg_error(struct netmsg *m)
{
	return m->errstr;
}

void
netmsg_clearerror(struct netmsg *m)
{
	*m->errstr = '\0';
}

ssize_t
netmsg_write(struct netmsg *m, void *bytes, size_t count)
{
	ssize_t	status;

	status = m->writestorage(m->descriptor, bytes, count);
	if (status < 0)
		strncpy(m->errstr, strerror(errno), NETMSG_ERRSTRSIZE);

	return status;
}

ssize_t
netmsg_read(struct netmsg *m, void *bytes, size_t count)
{
	ssize_t	status;

	status = m->readstorage(m->descriptor, bytes, count);
	if (status < 0)
		strncpy(m->errstr, strerror(errno), NETMSG_ERRSTRSIZE);

	return status;
}

ssize_t
netmsg_seek(struct netmsg *m, ssize_t offset, int whence)
{
	ssize_t status;

	status = m->seekstorage(m->descriptor, offset, whence);
	if (status < 0)
		strncpy(m->errstr, strerror(errno), NETMSG_ERRSTRSIZE);

	return status;
}

int
netmsg_truncate(struct netmsg *m, ssize_t offset)
{
	ssize_t status;

	status = m->truncatestorage(m->descriptor, offset);
	if (status < 0)
		strncpy(m->errstr, strerror(errno), NETMSG_ERRSTRSIZE);

	return status;	
}

static int
netmsg_getclaimedlabelsize(struct netmsg *m, uint64_t *out)
{
	ssize_t		offset, bytesread;
	int		status = -1;

	offset = sizeof(uint8_t);

	if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
		log_fatal("netmsg_getclaimedlabelsize: could not seek to %lu", offset);

	bytesread = m->readstorage(m->descriptor, out, sizeof(uint64_t));

	if (bytesread < 0)
		log_fatal("netmsg_getclaimedlabelsize: could not read buffer");
	else if (bytesread < (ssize_t)sizeof(uint64_t)) {
		strncpy(m->errstr, "label size is incompletely received", NETMSG_ERRSTRSIZE);
		goto end;
	}

	*out = be64toh(*out);
	status = 0;
end:
	return status;
}

static int
netmsg_getclaimeddatasize(struct netmsg *m, uint64_t *out)
{
	uint64_t	labelsize;
	ssize_t		offset, bytesread;
	int		status = -1;

	if (netmsg_getclaimedlabelsize(m, &labelsize) < 0) goto end;
	else offset = sizeof(uint8_t) + sizeof(uint64_t) + labelsize;

	if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
		log_fatal("netmsg_getclaimeddatasize: could not seek to %lu", offset);

	bytesread = m->readstorage(m->descriptor, out, sizeof(uint64_t));

	if (bytesread < 0)
		log_fatal("netmsg_getclaimeddatasize: could not read buffer");
	else if (bytesread < (ssize_t)sizeof(uint64_t)) {
		strncpy(m->errstr, "data size is incompletely received", NETMSG_ERRSTRSIZE);
		goto end;
	}

	*out = be64toh(*out);
	status = 0;
end:
	return status;
}

static ssize_t
netmsg_getexpectedsizeifvalid(struct netmsg *m)
{
	uint64_t	scratchsize;
	ssize_t		total = -1;

	/* XXX: be careful, this assumes the message is valid */

	total = sizeof(uint8_t);

	if (netmsg_getclaimedlabelsize(m, &scratchsize) == 0)
		total += (ssize_t)scratchsize + sizeof(uint64_t);

	if (netmsg_getclaimeddatasize(m, &scratchsize) == 0)
		total += (ssize_t)scratchsize + sizeof(uint64_t);

	netmsg_clearerror(m);
	return total;
}

static void
netmsg_committype(struct netmsg *m)
{
	ssize_t		byteswritten;

	if (m->seekstorage(m->descriptor, 0, SEEK_SET) < 0)
		log_fatal("netmsg_committype: could not seek to start of buffer");

	byteswritten = m->writestorage(m->descriptor, &m->opcode, sizeof(uint8_t));

	if (byteswritten < 0)
		log_fatal("netmsg_committype: could not write buffer");
	else if (byteswritten < (ssize_t)sizeof(uint8_t))
		log_fatalx("netmsg_committype: could not flush opcode to buffer");

	if (m->seekstorage(m->descriptor, 0, SEEK_SET) != 0)
		log_fatal("netmsg_committype: could not seek message to start post-type-commit");
}

uint8_t
netmsg_gettype(struct netmsg *m)
{
	return m->opcode;
}

char *
netmsg_getlabel(struct netmsg *m)
{
	char		*out = NULL;
	ssize_t		 bytesread, offset;
	uint64_t	 labelsize;

	offset = sizeof(uint8_t) + sizeof(uint64_t);
	if (netmsg_getclaimedlabelsize(m, &labelsize) < 0) goto end;

	if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
		log_fatal("netmsg_getlabel: could not seek to %lu", offset);

	out = reallocarray(NULL, labelsize + 1, sizeof(char));
	if (out == NULL)
		log_fatal("netmsg_getlabel: reallocarray for netmsg label copy");

	bytesread = m->readstorage(m->descriptor, out, labelsize);

	if (bytesread < 0)
		log_fatal("netmsg_getlabel: could not read buffer");

	out[bytesread] = '\0';
end:
	return out;
}


int
netmsg_setlabel(struct netmsg *m, char *newlabel)
{
	char		*datacopy = NULL;
	uint64_t	 labelsize, datacopysize = 0;
	uint64_t	 newlabelsize, benewlabelsize;

	int		 status = 0;

	/* if there's already a label here, there also might be
	 * some data...
	 */

	if (netmsg_getclaimedlabelsize(m, &labelsize) == 0) {
		ssize_t	totalsize, offset;

		totalsize = m->seekstorage(m->descriptor, 0, SEEK_END);
		if (totalsize < 0) log_fatal("netmsg_setlabel: failed to find eof");

		offset = labelsize + sizeof(uint64_t) + sizeof(uint8_t);
		datacopysize = totalsize - offset;

		if (datacopysize > 0) {
			datacopy = reallocarray(NULL, datacopysize, sizeof(char));
			if (datacopy == NULL) log_fatal("netmsg_setlabel: reallocarray datacopy");

			if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
				log_fatal("netmsg_setlabel: could not seek to data to be backed up");

			if (m->readstorage(m->descriptor, datacopy, datacopysize)
				!= (ssize_t)datacopysize)

				log_fatal("netmsg_setlabel: could not read out data to be backed up");
		}
	}

	if (m->truncatestorage(m->descriptor, sizeof(uint8_t)) < 0)
		log_fatal("netmsg_setlabel: failed to truncate buffer down before relabel");

	if (m->seekstorage(m->descriptor, 0, SEEK_END) < 0)
		log_fatal("netmsg_setlabel: failed to seek to end of truncated buffer");

	newlabelsize = strlen(newlabel);
	benewlabelsize = htobe64(newlabelsize);

	if (m->writestorage(m->descriptor, &benewlabelsize, sizeof(uint64_t))
		!= sizeof(uint64_t))

		log_fatal("netmsg_setlabel: failed to write new label size");

	if (m->writestorage(m->descriptor, newlabel, newlabelsize) != (ssize_t)newlabelsize)
		log_fatal("netmsg_setlabel: failed to write new label");

	if (datacopysize > 0) {
		if (m->writestorage(m->descriptor, datacopy, datacopysize) !=
			(ssize_t)datacopysize)

			log_fatal("netmsg_setlabel: failed to restore backed up data");

		free(datacopy);
	}

	return status;
}

char *
netmsg_getdata(struct netmsg *m, uint64_t *sizeout)
{
	char		*out = NULL;
	uint64_t	 datasize, labelsize;
	ssize_t		 bytesread, offset;

	if (netmsg_getclaimedlabelsize(m, &labelsize) < 0) goto end;
	else if (netmsg_getclaimeddatasize(m, &datasize) < 0) goto end;

	offset = sizeof(uint8_t) + sizeof(uint64_t) + labelsize + sizeof(uint64_t);

	if (m->seekstorage(m->descriptor, offset, SEEK_SET) < 0)
		log_fatal("netmsg_getdata: failed to seek to start of data");

	out = reallocarray(NULL, datasize, sizeof(char));
	if (out == NULL)
		log_fatal("netmsg_getdata: reallocarray for read data failed");

	bytesread = m->readstorage(m->descriptor, out, datasize);

	if (bytesread < 0)
		log_fatal("netmsg_getdata: could not read buffer");

	*sizeout = bytesread;
end:
	return out;
}

int
netmsg_setdata(struct netmsg *m, char *newdata, uint64_t datasize)
{
	uint64_t	labelsize, bedatasize;
	ssize_t		offset;
	int		status = -1;	

	if (netmsg_getclaimedlabelsize(m, &labelsize) < 0) goto end;

	offset = sizeof(uint8_t) + sizeof(uint64_t) + labelsize;

	if (m->truncatestorage(m->descriptor, offset) < 0)
		log_fatal("netmsg_setdata: failed to truncate buffer to type+label");

	if (m->seekstorage(m->descriptor, 0, SEEK_END) < 0)
		log_fatal("netmsg_setdata: failed to seek to end of label");

	bedatasize = htobe64(datasize);

	if (m->writestorage(m->descriptor, &bedatasize, sizeof(uint64_t))
		!= sizeof(uint64_t))

		log_fatal("netmsg_setdata: failed to write new data size");

	if (m->writestorage(m->descriptor, newdata, datasize) != (ssize_t)datasize)
		log_fatal("netmsg_setdata: failed to write new data");

	status = 0;
end:
	return status;
}

int
netmsg_isvalid(struct netmsg *m, int *fatal)
{
	char		*copied;
	uint64_t	 claimedsize, copiedsize;
	int		 needlabel, needdata, status = 0;

	ssize_t		 actualtypesize;
	uint8_t		 actualtype;

	ssize_t		 actualmessagesize, calculatedmessagesize;

	/* usually, validity failures are not fatal, i.e.
	 * more data can resolve the issue at hand
	 */
	*fatal = 0;

	switch (m->opcode) {
	case NETOP_WRITE:
	case NETOP_BUNDLE:
		needlabel = 1;
		needdata = 1;
		break;

	case NETOP_ERROR:
		needlabel = 1;
		needdata = 0;
		break;

	case NETOP_SIGN:
	case NETOP_HEARTBEAT:
	case NETOP_ACK:
		needlabel = 0;
		needdata = 0;
		break;

	default:
		snprintf(m->errstr, NETMSG_ERRSTRSIZE, "illegal message type %d", m->opcode);
		*fatal = 1;
		goto end;
	}

	if (m->seekstorage(m->descriptor, 0, SEEK_SET) < 0)
		log_fatal("netmsg_isvalid: failed to seek to start of message to check type");

	actualtypesize = m->readstorage(m->descriptor, &actualtype, sizeof(uint8_t));

	if (actualtypesize < 0)
		log_fatal("netmsg_isvalid: failed to pull actual type off message");

	else if (actualtypesize != sizeof(uint8_t)) {
		strncpy(m->errstr, "netmsg_isvalid: complete message type not present",
			NETMSG_ERRSTRSIZE);
		goto end;

	} else if (actualtype != m->opcode) {
		snprintf(m->errstr, NETMSG_ERRSTRSIZE,
			"cached opcode %u doesn't match marshalled opcode %u",
			m->opcode, actualtype);
		*fatal = 1;
		goto end;
	}

	if (needlabel) {
		if (netmsg_getclaimedlabelsize(m, &claimedsize) < 0) goto end;

		copied = netmsg_getlabel(m);
		if (copied == NULL) goto end;

		copiedsize = strlen(copied);

		if (copiedsize != claimedsize) {
			snprintf(m->errstr, NETMSG_ERRSTRSIZE, "claimed label size %llu != actual label strlen %llu",
				claimedsize, copiedsize);
			goto end;
		}
	}

	if (needdata) {
		if (netmsg_getclaimeddatasize(m, &claimedsize) < 0) goto end;

		copied = netmsg_getdata(m, &copiedsize);
		if (copied == NULL) goto end;

		if (copiedsize != claimedsize) {
			strncpy(m->errstr, "claimed data size != actual data size",
				NETMSG_ERRSTRSIZE);
			goto end;
		}
	}

	calculatedmessagesize = netmsg_getexpectedsizeifvalid(m);
	actualmessagesize = m->seekstorage(m->descriptor, 0, SEEK_END);

	if (actualmessagesize < 0)
		log_fatal("netmsg_isvalid: seek for actual message size");

	else if (actualmessagesize != calculatedmessagesize) {
		snprintf(m->errstr, NETMSG_ERRSTRSIZE,
			"claimed message size %ld != actual message size %ld",
			calculatedmessagesize, actualmessagesize);

		*fatal = 1;
		goto end;
	}

	status = 1;
end:
	return status;
}