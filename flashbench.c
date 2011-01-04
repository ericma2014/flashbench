#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>

#include "dev.h"
#include "vm.h"

typedef long long ns_t;

#define returnif(x) do { typeof(x) __x = (x); if (__x < 0) return (__x); } while (0)

static ns_t ns_min(int count, ns_t data[])
{
	int i;
	ns_t min = LLONG_MAX;

	for (i=0; i<count; i++) {
		if (data[i] < min)
			min = data[i];
	}

	return min;
}

#if 0
static void ns_min_elements(int count, ns_t target[], ns_t new[])
{
	int i;

	for (i=0; i<count; i++) {
		if (new[i] < target[i] || target[i] == 0)
			target[i] = new[i];
	}
}
#endif

static ns_t ns_max(int count, ns_t data[])
{
	int i;
	ns_t max = 0;

	for (i=0; i<count; i++) {
		if (data[i] > max)
			max = data[i];
	}

	return max;
}

static ns_t ns_avg(int count, ns_t data[])
{
	int i;
	ns_t sum = 0;

	for (i=0; i<count; i++) {
		sum += data[i];
	}

	return sum / i;
}

static void format_ns(char *out, ns_t ns)
{
	if (ns < 1000)
		snprintf(out, 8, "%lldns", ns);
	else if (ns < 1000 * 1000)
		snprintf(out, 8, "%.3gµs", ns / 1000.0);
	else if (ns < 1000 * 1000 * 1000)
		snprintf(out, 8, "%.3gms", ns / 1000000.0);
	else {
		snprintf(out, 8, "%.4gs", ns / 1000000000.0);
	}
}

static inline void print_ns(ns_t ns)
{
	char buf[8];
	format_ns(buf, ns);
	puts(buf);
}

static void regression(ns_t ns[], off_t bytes[], int count, ns_t *atime, float *throughput)
{
	int i;
	float sum_x = 0, sum_xx = 0;
	float sum_y = 0, sum_xy = 0;
	float slope, intercept;
	char buf[8];

	for (i = 0; i < count; i++) {
		sum_x	+= (float)bytes[i];
		sum_xx	+= (float)bytes[i] * (float)bytes[i];
		sum_y	+= (float)ns[i];
		sum_xy	+= (float)ns[i] * (float)bytes[i];
	}

	/* standard linear regression method */
	slope = (float)(count * sum_xy - sum_x * sum_y) /
		(float)(count * sum_xx - sum_x * sum_x);
	intercept = (sum_y - slope * sum_x) / count;

	format_ns(buf, intercept);
	printf("%g MB/s, %s access time\n", 1000.0 / slope, buf);

	*atime = intercept;
	*throughput = 1000.0 / slope;
}

static void flush_read_cache(struct device *dev)
{
	off_t cache_size = 1024 * 1024; /* FIXME: use detected size */

	time_read(dev, 2 * cache_size, dev->size - 2 * cache_size);
}

static int time_read_interval(struct device *dev, int count, ns_t results[],
				 size_t size, off_t offset, off_t interval)
{
	int i;
	off_t pos;
	ns_t ret;

	for (i=0; i < count; i++) {
		pos = offset + i * interval;
		ret = time_read(dev, pos, size);
		returnif (ret);

		if (results[i] == 0 || results[i] > ret)
			results[i] = ret;
	}

	return 0;
}

static int try_read_cache(struct device *dev)
{
	const int rounds = 18;
	const int tries = 8;
	ns_t times[rounds];
	int i;

	for (i = 0; i < rounds; i++) {
		long blocksize = 512l << i;
		char min[8];
		int j;

		times[i] = LLONG_MAX;

		for (j = 0; j < tries; j++) {
			ns_t ns;
			ns = time_read(dev, 1024 * 1024 * 1024, blocksize);

			returnif (ns);

			if (ns < times[i])
				times[i] = ns;
		}

		format_ns(min, times[i]);

		printf("%ld bytes: %s, %g MB/s\n", blocksize, min, 
					blocksize / (times[i] / 1000.0));
	}

	return 0;
}

static void print_one_blocksize(int count, ns_t *times, off_t blocksize)
{
	char min[8], avg[8], max[8];

	format_ns(min, ns_min(count, times));
	format_ns(avg, ns_avg(count, times));
	format_ns(max, ns_max(count, times));

	printf("%ld bytes: min %s avg %s max %s: %g MB/s\n", blocksize,
		 min, avg, max, blocksize / (ns_min(count, times) / 1000.0));
}

static int try_interval(struct device *dev, long blocksize, ns_t *min_time, int count)
{
	int ret;
	ns_t times[count];
	memset(times, 0, sizeof(times));

	ret = time_read_interval(dev, count, times, blocksize, 0, blocksize * 9);
	returnif (ret);

	print_one_blocksize(count, times, blocksize);
	*min_time = ns_min(count, times);

	return 0;
}

static int try_intervals(struct device *dev, int count, int rounds)
{
	const int ignore = 3;
	ns_t min[rounds];
	off_t bytes[rounds];
	ns_t atime;
	float throughput;
	int i;

	for (i=0; i<rounds; i++) {
		bytes[i] = 512l << i;
		try_interval(dev, bytes[i], &min[i], count);

	}

	regression(min + ignore, bytes + ignore, rounds - ignore, &atime, &throughput);

	for (i=0; i<rounds; i++) {
		printf("bytes %lld, time %lld overhead %g\n", (long long)bytes[i], min[i],
			min[i] - atime - bytes[i] * 1000 / throughput);
	}

	return 0;
}

static int try_align(struct device *dev)
{
	const int rounds = 11;
	const int count = 16;
	int i;

	ns_t aligned[rounds];
	ns_t unaligned[rounds];

	for (i=0; i<rounds; i++) {
		off_t blocksize = 512l << (i + 1);
		ns_t times[count];
		char buf_a[8], buf_u[8];
		ns_t avg_a, avg_u;
		int ret;

		flush_read_cache(dev);

		memset(times, 0, sizeof(times));
		ret = time_read_interval(dev, count, times, blocksize, 0, blocksize * 8);
		returnif (ret);

		aligned[i] = ns_min(count, times);
		avg_a = ns_avg(count, times);

		memset(times, 0, sizeof(times));
		ret = time_read_interval(dev, count, times, blocksize, blocksize / 2, blocksize * 8);
		returnif (ret);

		unaligned[i] = ns_min(count, times);
		avg_u = ns_avg(count, times);

		format_ns(buf_a, aligned[i]);
		format_ns(buf_u, unaligned[i]);

		printf("%ld bytes: aligned %s unaligned %s diff %lld, %02g%% min %02g%% avg\n",
			blocksize, buf_a, buf_u, unaligned[i] - aligned[i],
			 100.0 * (unaligned[i] - aligned[i]) / aligned[i],
			 100.0 * (avg_u - avg_a) / avg_a);
	}

	return 0;
}

/*
 * Linear feedback shift register
 *
 * We use this to randomize the block positions for random-access
 * tests. Unlike real random data, we know that within 2^bits
 * accesses, every possible value up to 2^bits will be seen
 * exactly once, with the exception of zero, for which we have
 * a special treatment.
 */
static int lfsr(unsigned short v, unsigned int bits)
{
	unsigned short bit;

	if (v >= (1 << bits)) {
		fprintf(stderr, "flashbench: internal error\n");
		exit(-EINVAL);
	}

	if (v == 0)
		v = ((1 << bits) - 1) & 0xace1;

	switch (bits) {
	case 8: /* x^8 + x^6 + x^5 + x^4 + 1 */
		bit = ((v >> 0) ^ (v >> 2) ^ (v >> 3) ^ (v >> 4)) & 1;
		break;
	case 9: /* x9 + x5 + 1 */
		bit = ((v >> 0) ^ (v >> 4)) & 1;
		break;
	case 10: /* x10 + x7 + 1 */
		bit = ((v >> 0) ^ (v >> 3)) & 1;
		break;
	case 11: /* x11 + x9 + 1 */
		bit = ((v >> 0) ^ (v >> 2)) & 1;
		break;
	case 12:
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 8)) & 1;
		break;
	case 13: /* x^13 + x^12 + x^11 + x^8 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 5)) & 1;
		break;
	case 14: /* x^14 + x^13 + x^12 + x^2 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ^ (v >> 2) ^ (v >> 12)) & 1;
		break;
	case 15: /* x^15 + x^14 + 1 */
		bit = ((v >> 0) ^ (v >> 1) ) & 1;
		break;
	case 16: /* x^16 + x^14 + x^13 + x^11 + 1 */
		bit = ((v >> 0) ^ (v >> 2) ^ (v >> 3) ^ (v >> 5) ) & 1;
		break;
	default:
		fprintf(stderr, "flashbench: internal error\n");
		exit(-EINVAL);
	}

	return v >> 1 | bit << (bits - 1);
}

static int try_scatter_io(struct device *dev, int tries, int scatter_order,
			int scatter_span, int blocksize, FILE *out)
{
	int i, j;
	const int count = 1 << scatter_order;
	ns_t time;
	ns_t min[count];
	unsigned long pos;

	memset(min, 0, sizeof(min));
	for (i = 0; i < tries; i++) {
		pos = 0;
		for (j = 0; j < count; j++) {
			time = time_read(dev, (pos * blocksize), scatter_span * blocksize);
			returnif (time);

			if (i == 0 || time < min[pos])
				min[pos] = time;

			pos = lfsr(pos, scatter_order);
		}
	}

	for (j = 0; j < count; j++) {
		fprintf(out, "%d	%lld\n", j * blocksize, min[j]);
	}

	return 0;
}

static int try_read_alignment(struct device *dev, int tries, int count,
				off_t maxalign, off_t align, size_t blocksize)
{
	ns_t pre[count], on[count], post[count];
	char pre_s[8], on_s[8], post_s[8], diff_s[8];
	int i, ret;

	memset(pre, 0, sizeof(pre));
	memset(on, 0, sizeof(on));
	memset(post, 0, sizeof(post));

	for (i = 0; i < tries; i++) {
		ret = time_read_interval(dev, count, pre, blocksize,
					 align - blocksize, maxalign);
		returnif(ret);

		ret = time_read_interval(dev, count, on, blocksize,
					 align - blocksize / 2, maxalign);
		returnif(ret);

		ret = time_read_interval(dev, count, post, blocksize,
					 align, maxalign);
		returnif(ret);
	}

	format_ns(pre_s,  ns_avg(count, pre));
	format_ns(on_s,   ns_avg(count, on));
	format_ns(post_s, ns_avg(count, post));
	format_ns(diff_s, ns_avg(count, on) - (ns_avg(count, pre) + ns_avg(count, post)) / 2);
	printf("align %ld\tpre %s\ton %s\tpost %s\tdiff %s\n", align, pre_s, on_s, post_s, diff_s);

	return 0;
}

static int try_read_alignments(struct device *dev, int tries, int blocksize)
{
	const int count = 32;
	int ret;
	off_t align, maxalign;

	/* make sure we can fit eight power-of-two blocks in the device */
	for (maxalign = blocksize * 2; maxalign < dev->size / count; maxalign *= 2)
		;

	for (align = maxalign; align >= blocksize * 2; align /= 2) {
		ret = try_read_alignment(dev, tries, count, maxalign, align, blocksize);
		returnif (ret);
	}

	return 0;
}

static int try_program(struct device *dev)
{
#if 0
	struct operation program[] = {
		{O_REPEAT, 4},
		{O_SEQUENCE, 3},
			{O_PRINT, .string = "Hello, World!\n"},
			{O_DROP},
				{O_PRINTF},
				{O_FORMAT},
				{O_REDUCE, 8, .aggregate = A_AVERAGE},
				{O_LEN_POW2, 8, 512},
				{O_OFF_LIN, 8, 4096 },
				{O_SEQUENCE, 3},
					{O_PRINTF},
						{O_READ},
					{O_NEWLINE},
					{O_DROP},
						{O_READ},
					{O_END},
			{O_NEWLINE},
			{O_END},
		{O_END},
	};
#endif

#if 0
	struct operation program[] = {
		{O_SEQUENCE, 3},
			{O_PRINT, .string="read by size\n"},
			{O_LEN_POW2, 12, 512},
				{O_DROP},
				{O_SEQUENCE, 4},
					{O_PRINTF},
					{O_FORMAT},
					{O_LENGTH},
				{O_PRINT, .string = ": \t"},
				{O_PRINTF},
					{O_FORMAT},
					{O_REDUCE, .aggregate = A_MINIMUM},
					{O_OFF_LIN, 8, 4096 * 1024},
					{O_READ},
				{O_NEWLINE},
				{O_END},
			{O_NEWLINE},
			{O_END},
		{O_END},
	};
#endif	

	struct operation program[] = {
		{O_LEN_POW2, 10, -8192},
		{O_SEQUENCE, 3},
			{O_DROP},
				{O_PRINTF},
				{O_FORMAT},
				{O_LENGTH},
			{O_PRINT, .string = " linear write0/write1/writerand/read\n"},
			{O_OFF_FIXED, .val = 1024 * 4096 * 4}, {O_DROP},
			{O_SEQUENCE, 11},
				{O_DROP}, {O_LEN_FIXED, .val = 1024 * 4096},
					{O_WRITE_RAND},
				{O_PRINTF}, {O_FORMAT},
					{O_REDUCE, .aggregate = A_TOTAL},
					{O_OFF_LIN, 2048, -1},
					{O_WRITE_ZERO},
				{O_PRINTF}, {O_FORMAT},
					{O_REDUCE, .aggregate = A_TOTAL},
					{O_OFF_LIN, 2048, -1},
					{O_WRITE_ONE},
				{O_PRINTF}, {O_FORMAT},
					{O_REDUCE, .aggregate = A_TOTAL},
					{O_OFF_LIN, 2048, -1},
					{O_WRITE_RAND},
				{O_PRINTF}, {O_FORMAT},
					{O_REDUCE, .aggregate = A_TOTAL},
					{O_OFF_LIN, 2048, -1},
					{O_READ},
				{O_NEWLINE},
				{O_PRINTF}, {O_FORMAT},
					{O_REDUCE, .aggregate = A_TOTAL},
					{O_OFF_RAND, 2048, -1},
					{O_WRITE_ZERO},
				{O_PRINTF}, {O_FORMAT},
					{O_REDUCE, .aggregate = A_TOTAL},
					{O_OFF_RAND, 2048, -1},
					{O_WRITE_ONE},
				{O_PRINTF}, {O_FORMAT},
					{O_REDUCE, .aggregate = A_TOTAL},
					{O_OFF_RAND, 2048, -1},
					{O_WRITE_RAND},
				{O_PRINTF}, {O_FORMAT},
					{O_REDUCE, .aggregate = A_TOTAL},
					{O_OFF_RAND, 2048, -1},
					{O_READ},
				{O_NEWLINE},
				{O_END},
#if 0
				{O_DROP},
				{O_PRINTF},
				{O_SEQUENCE, 3},
					{O_FORMAT}, {O_OFFSET},
					{O_FORMAT}, {O_LENGTH},
					{O_NEWLINE},
					{O_END},
#endif
			{O_END},
		{O_END},
	};

	call(program, dev, 0, 4096 * 1024, 1);

	return 0;
}

static void print_help(const char *name)
{
	printf("%s [OPTION]... [DEVICE]\n", name);
	printf("run tests on DEVICE, pointing to a flash storage medium.\n\n");
	printf("-o, --out=FILE	write output to FILE instead of stdout\n");
	printf("-s, --scatter	run scatter read test\n");
	printf("    --scatter-order=N scatter across 2^N blocks\n");
	printf("-r, --rcache	determine read cache size\n");
	printf("-v, --verbose	increase verbosity of output\n");
	printf("-c, --count=N	run each test N times (default: 8\n");	
}

struct arguments {
	const char *dev;
	const char *out;
	bool scatter, rcache, align, interval, program;
	int verbosity;
	int count;
	int blocksize;
	int scatter_order;
	int scatter_span;
	int interval_order;
};

static int parse_arguments(int argc, char **argv, struct arguments *args)
{
	static const struct option long_options[] = {
		{ "out", 1, NULL, 'o' },
		{ "scatter", 0, NULL, 's' },
		{ "scatter-order", 1, NULL, 'S' },
		{ "scatter-span", 1, NULL, '$' },
		{ "rcache", 0, NULL, 'r' },
		{ "align", 0, NULL, 'a' },
		{ "interval", 0, NULL, 'i' },
		{ "interval-order", 1, NULL, 'I' },
		{ "verbose", 0, NULL, 'v' },
		{ "count", 1, NULL, 'c' },
		{ "blocksize", 1, NULL, 'b' },
		{ NULL, 0, NULL, 0 },
	};

	memset(args, 0, sizeof(*args));
	args->count = 8;
	args->scatter_order = 12;
	args->scatter_span = 8;
	args->blocksize = 8192;

	while (1) {
		int c;

		c = getopt_long(argc, argv, "o:sraivc:b:p", long_options, &optind);

		if (c == -1)
			break;

		switch (c) {
		case 'o':
			args->out = optarg;
			break;

		case 's':
			args->scatter = 1;
			break;

		case 'S':
			args->scatter_order = atoi(optarg);
			break;

		case '$':
			args->scatter_span = atoi(optarg);
			break;

		case 'r':
			args->rcache = 1;
			break;

		case 'a':
			args->align = 1;
			break;

		case 'i':
			args->interval = 1;
			break;

		case 'I':
			args->interval_order = atoi(optarg);
			break;

		case 'p':
			args->program = 1;
			break;

		case 'v':
			args->verbosity++;
			break;

		case 'c':
			args->count = atoi(optarg);
			break;

		case 'b':
			args->blocksize = atoi(optarg);
			break;

		case '?':
			print_help(argv[0]);
			return -EINVAL;
			break;
		}
	}

	if (optind != (argc - 1))  {
		fprintf(stderr, "%s: invalid arguments\n", argv[0]);
		return -EINVAL;
	}

	args->dev = argv[optind];

	if (!(args->scatter || args->rcache ||
	      args->align || args->interval ||
	      args->program)) {
		fprintf(stderr, "%s: need at least one action\n", argv[0]);
		return -EINVAL;
	}

	if (args->scatter && (args->scatter_order > 16)) {
		fprintf(stderr, "%s: scatter_order must be at most 16\n", argv[0]);
		return -EINVAL;
	}

	return 0;
}

static FILE *open_output(const char *filename)
{
	if (!filename || !strcmp(filename, "-"))
		return fdopen(0, "w"); /* write to stdout */

	return fopen(filename, "w+");
}

int main(int argc, char **argv)
{
	struct device dev;
	struct arguments args;
	FILE *output;
	int ret;

	returnif(parse_arguments(argc, argv, &args));

	returnif(setup_dev(&dev, args.dev));

	output = open_output(args.out);
	if (!output) {
		perror(args.out);
		return -errno;
	}

	if (args.verbosity) {
		printf("filename: \"%s\"\n", argv[1]);
		printf("filesize: 0x%llx\n", (unsigned long long)dev.size);
	}

	if (args.scatter) {
		ret = try_scatter_io(&dev, args.count, args.scatter_order,
				 args.scatter_span, args.blocksize, output);
		if (ret < 0) {
			errno = -ret;
			perror("try_scatter_io");
			return ret;
		}
	}

	if (args.align) {
		ret = try_read_alignments(&dev, args.count, args.blocksize);
		if (ret < 0) {
			errno = -ret;
			perror("try_align");
			return ret;
		}
	}

	if (args.rcache) {
		ret = try_read_cache(&dev);
		if (ret < 0) {
			errno = -ret;
			perror("try_read_cache");
			return ret;
		}
	}

	if (args.interval) {
		ret = try_intervals(&dev, args.count, args.interval_order);
		if (ret < 0) {
			errno = -ret;
			perror("try_intervals");
			return ret;
		}
	}

	if (args.program) {
		try_program(&dev);
	}

	return 0;
}
