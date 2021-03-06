/*
 * A virtio device implementing a hardware random number generator.
 *
 * Copyright 2012 Red Hat, Inc.
 * Copyright 2012 Amit Shah <amit.shah@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/iov.h"
#include "hw/qdev.h"
#include "qapi/qmp/qerror.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-rng.h"
#include "sysemu/rng.h"

static bool is_guest_ready(VirtIORNG *vrng)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vrng);
    if (virtio_queue_ready(vrng->vq)
        && (vdev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return true;
    }
    return false;
}

static size_t get_request_size(VirtQueue *vq, unsigned quota)
{
    unsigned int in, out;

    virtqueue_get_avail_bytes(vq, &in, &out, quota, 0);
    return in;
}

static void virtio_rng_process(VirtIORNG *vrng);

/* Send data from a char device over to the guest */
static void chr_read(void *opaque, const void *buf, size_t size)
{
    VirtIORNG *vrng = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(vrng);
    VirtQueueElement elem;
    size_t len;
    int offset;

    if (!is_guest_ready(vrng)) {
        return;
    }

    vrng->quota_remaining -= size;

    offset = 0;
    while (offset < size) {
        if (!virtqueue_pop(vrng->vq, &elem)) {
            break;
        }
        len = iov_from_buf(elem.in_sg, elem.in_num,
                           0, buf + offset, size - offset);
        offset += len;

        virtqueue_push(vrng->vq, &elem, len);
    }
    virtio_notify(vdev, vrng->vq);
}

static void virtio_rng_process(VirtIORNG *vrng)
{
    size_t size;
    unsigned quota;

    if (!is_guest_ready(vrng)) {
        return;
    }

    if (vrng->quota_remaining < 0) {
        quota = 0;
    } else {
        quota = MIN((uint64_t)vrng->quota_remaining, (uint64_t)UINT32_MAX);
    }
    size = get_request_size(vrng->vq, quota);
    size = MIN(vrng->quota_remaining, size);
    if (size) {
        rng_backend_request_entropy(vrng->rng, size, chr_read, vrng);
    }
}

static void handle_input(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIORNG *vrng = VIRTIO_RNG(vdev);
    virtio_rng_process(vrng);
}

static uint32_t get_features(VirtIODevice *vdev, uint32_t f)
{
    return f;
}

static void virtio_rng_save(QEMUFile *f, void *opaque)
{
    VirtIODevice *vdev = opaque;

    virtio_save(vdev, f);
}

static int virtio_rng_load(QEMUFile *f, void *opaque, int version_id)
{
    VirtIORNG *vrng = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(vrng);

    if (version_id != 1) {
        return -EINVAL;
    }
    virtio_load(vdev, f);

    /* We may have an element ready but couldn't process it due to a quota
     * limit.  Make sure to try again after live migration when the quota may
     * have been reset.
     */
    virtio_rng_process(vrng);

    return 0;
}

static void check_rate_limit(void *opaque)
{
    VirtIORNG *vrng = opaque;

    vrng->quota_remaining = vrng->conf.max_bytes;
    virtio_rng_process(vrng);
    qemu_mod_timer(vrng->rate_limit_timer,
                   qemu_get_clock_ms(vm_clock) + vrng->conf.period_ms);
}

static int virtio_rng_device_init(VirtIODevice *vdev)
{
    DeviceState *qdev = DEVICE(vdev);
    VirtIORNG *vrng = VIRTIO_RNG(vdev);
    Error *local_err = NULL;

    if (vrng->conf.rng == NULL) {
        vrng->conf.default_backend = RNG_RANDOM(object_new(TYPE_RNG_RANDOM));

        object_property_add_child(OBJECT(qdev),
                                  "default-backend",
                                  OBJECT(vrng->conf.default_backend),
                                  NULL);

        object_property_set_link(OBJECT(qdev),
                                 OBJECT(vrng->conf.default_backend),
                                 "rng", NULL);
    }

    virtio_init(vdev, "virtio-rng", VIRTIO_ID_RNG, 0);

    vrng->rng = vrng->conf.rng;
    if (vrng->rng == NULL) {
        qerror_report(QERR_INVALID_PARAMETER_VALUE, "rng", "a valid object");
        return -1;
    }

    rng_backend_open(vrng->rng, &local_err);
    if (local_err) {
        qerror_report_err(local_err);
        error_free(local_err);
        return -1;
    }

    vrng->vq = virtio_add_queue(vdev, 8, handle_input);

    assert(vrng->conf.max_bytes <= INT64_MAX);
    vrng->quota_remaining = vrng->conf.max_bytes;

    vrng->rate_limit_timer = qemu_new_timer_ms(vm_clock,
                                               check_rate_limit, vrng);

    qemu_mod_timer(vrng->rate_limit_timer,
                   qemu_get_clock_ms(vm_clock) + vrng->conf.period_ms);

    register_savevm(qdev, "virtio-rng", -1, 1, virtio_rng_save,
                    virtio_rng_load, vrng);

    return 0;
}

static int virtio_rng_device_exit(DeviceState *qdev)
{
    VirtIORNG *vrng = VIRTIO_RNG(qdev);
    VirtIODevice *vdev = VIRTIO_DEVICE(qdev);

    qemu_del_timer(vrng->rate_limit_timer);
    qemu_free_timer(vrng->rate_limit_timer);
    unregister_savevm(qdev, "virtio-rng", vrng);
    virtio_cleanup(vdev);
    return 0;
}

static Property virtio_rng_properties[] = {
    DEFINE_VIRTIO_RNG_PROPERTIES(VirtIORNG, conf),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_rng_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    dc->exit = virtio_rng_device_exit;
    dc->props = virtio_rng_properties;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->init = virtio_rng_device_init;
    vdc->get_features = get_features;
}

static void virtio_rng_initfn(Object *obj)
{
    VirtIORNG *vrng = VIRTIO_RNG(obj);

    object_property_add_link(obj, "rng", TYPE_RNG_BACKEND,
                             (Object **)&vrng->conf.rng, NULL);
}

static const TypeInfo virtio_rng_info = {
    .name = TYPE_VIRTIO_RNG,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIORNG),
    .instance_init = virtio_rng_initfn,
    .class_init = virtio_rng_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_rng_info);
}

type_init(virtio_register_types)
