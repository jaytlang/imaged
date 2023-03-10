#include <sys/types.h>

#include <err.h>
#include <stdlib.h>

#include "bundled.h"

static uint32_t	key = 65535;
int		debug = 1, verbose = 1;

int
main()
{
	struct archive *a;

	config_parse("bundled.conf");

	a = archive_new(key);
	if (a == NULL) err(1, "archive_new");

	if (archive_fromkey(key) != a) err(1, "unable to find newly created archive");

	if (!archive_isvalid(a))
		errx(1, "initial archive_isvalid: %s", archive_error(a));

	archive_teardown(a);

	if (archive_fromkey(key) != NULL) errx(1, "found now-nonexistent archive");

	a = archive_new(key);
	if (a == NULL) err(1, "archive_new");

	archive_teardown(a);
}
