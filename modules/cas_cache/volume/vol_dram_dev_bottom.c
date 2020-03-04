/*
* Copyright(c) 2012-2019 Intel Corporation
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include "cas_cache.h"

#define CAS_DEBUG_IO 0

#if CAS_DEBUG_IO == 1
#define CAS_DEBUG_TRACE() printk(KERN_DEBUG \
		"[IO] %s:%d\n", __func__, __LINE__)

#define CAS_DEBUG_MSG(msg) printk(KERN_DEBUG \
		"[IO] %s:%d - %s\n", __func__, __LINE__, msg)

#define CAS_DEBUG_PARAM(format, ...) printk(KERN_DEBUG \
		"[IO] %s:%d - "format"\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#define CAS_DEBUG_TRACE()
#define CAS_DEBUG_MSG(msg)
#define CAS_DEBUG_PARAM(format, ...)
#endif

struct dram_object {
	void *data;
	unsigned long capacity;
};

struct dram_io {
	struct blk_data *data; /* IO data buffer */

	/* BIO vector iterator for sending IO */
	struct bio_vec_iter iter;
};

static inline struct dram_io *cas_io_to_dram_io(struct ocf_io *io)
{
	return ocf_io_get_priv(io);
}

static inline struct dram_object *dram_object(ocf_volume_t vol)
{
	return ocf_volume_get_priv(vol);
}

static int dram_dev_open_object(ocf_volume_t vol, void *volume_params)
{
	struct dram_object *dobj = dram_object(vol);
	const struct ocf_volume_uuid *uuid = ocf_volume_get_uuid(vol);

	dobj->capacity = *(unsigned int *)uuid->data;
	dobj->data = vmalloc(dobj->capacity << 30);
	if (!dobj->data)
		return -ENOMEM;
	return 0;
}

static void dram_dev_close_object(ocf_volume_t vol)
{
	struct dram_object *dobj = dram_object(vol);
	vfree(dobj->data);
}

static unsigned int dram_dev_get_max_io_size(ocf_volume_t vol)
{
	return 128*1024;
}

static uint64_t dram_dev_get_byte_length(ocf_volume_t vol)
{
	struct dram_object *dobj = dram_object(vol);
	uint64_t l = dobj->capacity;

	return l << 30;
}

static void dram_dev_submit_flush(struct ocf_io *io)
{
	io->end(io, 0);
}

static void dram_dev_submit_discard(struct ocf_io *io)
{
	io->end(io, 0);
}

/*
 *
 */
static void dram_dev_submit_io(struct ocf_io *io)
{
	struct dram_io *dio = cas_io_to_dram_io(io);
	struct dram_object *dobj = dram_object(ocf_io_get_volume(io));
	struct bio_vec_iter *iter = &dio->iter;
	uint64_t addr = io->addr;
	uint32_t bytes = io->bytes;
	int dir = io->dir;
	int error = 0;

	if (!CAS_IS_WRITE_FLUSH_FUA(io->flags) &&
			CAS_IS_WRITE_FLUSH(io->flags)) {
		CAS_DEBUG_MSG("Flush request");
		/* It is flush requests handle it */
		dram_dev_submit_flush(io);
		return;
	}

	CAS_DEBUG_PARAM("Address = %llu, bytes = %u\n", dio->addr,
			dio->bytes);

	while (cas_io_iter_is_next(iter) && bytes) {

		/* Copy pages */
		while (cas_io_iter_is_next(iter) && bytes) {
			struct page *page = cas_io_iter_current_page(iter);
			uint32_t offset = cas_io_iter_current_offset(iter);
			uint32_t length = cas_io_iter_current_length(iter);
			void *p = page_address(page) + offset;

			if (length > bytes)
				length = bytes;

			/* DO MEMCPY */
			if (dir == OCF_READ) {
				memcpy(p, dobj->data + addr, length);
			} else if (dir == OCF_WRITE) {
				memcpy(dobj->data + addr, p, length);
			}

			bytes -= length;
			addr += length;

			/* Update BIO vector iterator */
			if (length != cas_io_iter_move(iter, length)) {
				error = -ENOBUFS;
				break;
			}
		}
		if (error)
			break;
	}

	io->end(io, error);
}

static int dram_io_set_data(struct ocf_io *io,
		ctx_data_t *ctx_data, uint32_t offset)
{
	struct dram_io *dio = cas_io_to_dram_io(io);
	struct blk_data *data = ctx_data;

	/* Set BIO vector (IO data) and initialize iterator */
	dio->data = data;
	if (dio->data) {
		cas_io_iter_init(&dio->iter, dio->data->vec,
				dio->data->size);

		/* Move into specified offset in BIO vector iterator */
		if (offset != cas_io_iter_move(&dio->iter, offset)) {
			return -ENOBUFS;
		}
	}

	return 0;
}

/*
 *
 */
static ctx_data_t *dram_io_get_data(struct ocf_io *io)
{
	struct dram_io *dio = cas_io_to_dram_io(io);
	return dio->data;
}

const struct ocf_volume_properties cas_object_dram_properties = {
	.name = "DRAM_Device",
	.io_priv_size = sizeof(struct dram_io),
	.volume_priv_size = sizeof(struct dram_object),
	.caps = {
		.atomic_writes = 0, /* Atomic writes not supported */
	},
	.ops = {
		.submit_io = dram_dev_submit_io,
		.submit_flush = dram_dev_submit_flush,
		.submit_metadata = NULL,
		.submit_discard = dram_dev_submit_discard,
		.open = dram_dev_open_object,
		.close = dram_dev_close_object,
		.get_max_io_size = dram_dev_get_max_io_size,
		.get_length = dram_dev_get_byte_length,
	},
	.io_ops = {
		.set_data = dram_io_set_data,
		.get_data = dram_io_get_data,
	},
	.deinit = NULL,
};

int dram_dev_init(void)
{
	int ret;

	ret = ocf_ctx_register_volume_type(cas_ctx, DRAM_DEVICE_VOLUME,
			&cas_object_dram_properties);
	if (ret < 0)
		return ret;

	return 0;
}

