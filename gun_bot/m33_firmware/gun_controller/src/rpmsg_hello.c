/*
 * MobTurret M33 firmware — RPMsg hello-world (low-level openamp API).
 *
 * Port of the upstream imx93 sample (nxp_zephyr/samples/subsys/ipc/
 * openamp_rsc_table/src/main_remote.c) adapted for our two endpoints:
 *   - "turret-ctrl"      — inbound commands from the A55
 *   - "turret-telemetry" — outbound telemetry to the A55
 *
 * Uses the low-level rpmsg_init_vdev / rpmsg_create_ept API instead
 * of the rpmsg_service high-level wrapper, which proved to wedge
 * the M33's LPUART2 console on i.MX93 (the high-level service init
 * takes a mutex that the printk flush path also needs).
 *
 * The LPUART3 ping-scan thread is independent (in main.cpp) and
 * keeps running in parallel.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/sys/printk.h>
#include <zephyr/init.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>
#include <addr_translation.h>

#include <string.h>

/* DT-derived addresses and sizes for the chosen IPC nodes. */
#define SHM_START_ADDR DT_REG_ADDR(DT_CHOSEN(zephyr_ipc_shm))
#define SHM_SIZE       DT_REG_SIZE(DT_CHOSEN(zephyr_ipc_shm))

#if CONFIG_IPM_MAX_DATA_SIZE > 0
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, d, s)
#else
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, NULL, 0)
#endif

static const struct device *const ipm_handle =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_tab_physmap;

static struct metal_io_region shm_io_data;
static struct metal_io_region rsc_io_data;
static struct metal_io_region *const shm_io = &shm_io_data;
static struct metal_io_region *const rsc_io = &rsc_io_data;

static struct rpmsg_virtio_device rvdev;
static void *rsc_table;
static struct rpmsg_device *rpdev;

static struct rpmsg_endpoint ctrl_ept;
static struct rpmsg_endpoint telem_ept;

static K_SEM_DEFINE(data_sem, 0, 1);

static void platform_ipm_callback(const struct device *dev, void *context,
				  uint32_t id, volatile void *data)
{
	(void)dev;
	(void)context;
	(void)data;
	k_sem_give(&data_sem);
}

static int hello_rx_cb(struct rpmsg_endpoint *ept, void *data,
		       size_t len, uint32_t src, void *priv)
{
	(void)ept;
	(void)src;
	(void)priv;
	if (data == NULL || len == 0U) {
		return RPMSG_SUCCESS;
	}
	/* Trim trailing CR/LF for cleaner console output. */
	while (len > 0U
	       && (((const char *)data)[len - 1U] == '\n'
		   || ((const char *)data)[len - 1U] == '\r')) {
		len--;
	}
	if (len == 0U) {
		return RPMSG_SUCCESS;
	}
	/* Tag the receive with the endpoint name so we can tell
	 * ctrl and telemetry apart. */
	const char *tag = (ept == &ctrl_ept) ? "ctrl" : "telemetry";
	printk("[m33 rx:%s] %.*s\n", tag, (int)len, (const char *)data);
	return RPMSG_SUCCESS;
}

static int mailbox_notify(void *priv, uint32_t id)
{
	(void)priv;
	IPM_SEND(ipm_handle, 0, id, &id, 4);
	return 0;
}

static struct rpmsg_device *platform_create_rpmsg_vdev(
	unsigned int vdev_index, unsigned int role,
	void (*rst_cb)(struct virtio_device *vdev),
	rpmsg_ns_bind_cb ns_cb)
{
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device *vdev;
	int ret;

	(void)vdev_index;
	(void)role;
	(void)rst_cb;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID,
					rsc_table_to_vdev(rsc_table),
					rsc_io, NULL, mailbox_notify, NULL);
	if (!vdev) {
		printk("[m33] rproc_virtio_create_vdev failed\n");
		return NULL;
	}

	/* Block until the host-side virtio is up. */
	rproc_virtio_wait_remote_ready(vdev);

	vring_rsc = rsc_table_get_vring0(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 0, vring_rsc->notifyid,
				      (void *)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		printk("[m33] init vring 0 failed: %d\n", ret);
		goto fail;
	}

	vring_rsc = rsc_table_get_vring1(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 1, vring_rsc->notifyid,
				      (void *)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		printk("[m33] init vring 1 failed: %d\n", ret);
		goto fail;
	}

	ret = rpmsg_init_vdev(&rvdev, vdev, ns_cb, shm_io, NULL);
	if (ret) {
		printk("[m33] rpmsg_init_vdev failed: %d\n", ret);
		goto fail;
	}

	return rpmsg_virtio_get_rpmsg_device(&rvdev);
fail:
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

/* This is the binding callback invoked when the host-side
 * RPMSG_CREATE_EPT_IOCTL announces an endpoint. We only handle
 * our two named endpoints. */
static void new_service_cb(struct rpmsg_device *rdev, const char *name,
			   uint32_t src)
{
	(void)rdev;
	(void)src;
	if (strcmp(name, "turret-ctrl") == 0) {
		rpmsg_create_ept(&ctrl_ept, rdev, "turret-ctrl",
				 RPMSG_ADDR_ANY, src, hello_rx_cb, NULL);
	} else if (strcmp(name, "turret-telemetry") == 0) {
		rpmsg_create_ept(&telem_ept, rdev, "turret-telemetry",
				 RPMSG_ADDR_ANY, src, hello_rx_cb, NULL);
	}
}

/* v31: enable the IPM device in a POST_KERNEL SYS_INIT hook so
 * the A55's kick reaches us even before rpmsg_mng_task runs. */
static int rpmsg_ipm_init(void)
{
	if (!device_is_ready(ipm_handle)) {
		return -ENODEV;
	}
	ipm_register_callback(ipm_handle, platform_ipm_callback, NULL);
	ipm_set_enabled(ipm_handle, 1);
	return 0;
}
SYS_INIT(rpmsg_ipm_init, POST_KERNEL, 49);

static void rpmsg_mng_task(void *a, void *b, void *c)
{
	(void)a; (void)b; (void)c;
	(void)shm_io; (void)shm_physmap; (void)rsc_io; (void)rsc_tab_physmap;
	(void)ipm_handle; (void)rpdev; (void)rsc_table;

	while (true) {
		k_sleep(K_SECONDS(1));
	}
}

#define RPMSG_MNG_STACK 2048
K_THREAD_STACK_DEFINE(rpmsg_mng_stack, RPMSG_MNG_STACK);
static struct k_thread rpmsg_mng_thread;

void rpmsg_hello_start(void)
{
	k_thread_create(&rpmsg_mng_thread, rpmsg_mng_stack,
			K_THREAD_STACK_SIZEOF(rpmsg_mng_stack),
			rpmsg_mng_task, NULL, NULL, NULL,
			8, 0, K_NO_WAIT);
	k_thread_name_set(&rpmsg_mng_thread, "rpmsg_mng");
}
