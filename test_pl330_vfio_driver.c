#include "pl330_vfio_driver/pl330_vfio.h"

#include <linux/vfio.h>
#include <linux/types.h>
#include <linux/vfio.h>
#include <linux/vfio.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/fcntl.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	int container, group, device;
	unsigned int i;

	struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(iommu_info) };
	// source memory area the DMA controller will read from
	struct vfio_iommu_type1_dma_map dma_map_src = { .argsz = sizeof(dma_map_src) };
	// destination memory area the DMA controller will read to
	struct vfio_iommu_type1_dma_map dma_map_dst = { .argsz = sizeof(dma_map_dst) };
	/* 
	 * memory area where the DMA controller will grub the instructions
	 * to execute. We will tell to the controller how to reach these
	 * instructions through the DEBUG registers.
	 */
	struct vfio_iommu_type1_dma_map dma_map_inst = { .argsz = sizeof(dma_map_inst) };

	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };

	int ret;

	if (argc != 3) {
		printf("Usage: ./vfio-dt /dev/vfio/${group_id} device_id\n");
		return 2;
	}

	/* Create a new container */
	container = open("/dev/vfio/vfio", O_RDWR);

	if (ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		printf("Unknown API version\n");
		return 1;
	}

	if (!ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
		printf("Doesn't support the IOMMU driver we want\n");
		return 1;
	}

	/* Open the group */
	group = open(argv[1], O_RDWR);

	/* Test the group is viable and available */
	ioctl(group, VFIO_GROUP_GET_STATUS, &group_status);

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		printf("Group is not viable (not all devices bound for vfio)\n");
		return 1;
	}

	/* Add the group to the container */
	ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);

	/* Enable the IOMMU model we want */
	ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

	/* Get addition IOMMU info */
	ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info);

	// easy and safer map
	int size_to_map = getpagesize();

	// source map for the dma copy
	dma_map_src.vaddr = (u64)mmap(NULL, size_to_map, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	dma_map_src.size = size_to_map;
	dma_map_src.iova = 0;
	dma_map_src.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	// destination map for the dma copy
	dma_map_dst.vaddr = (u64)mmap(NULL, size_to_map, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	dma_map_dst.size = size_to_map;
	dma_map_dst.iova = dma_map_src.size;
	dma_map_dst.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	// memory which stores the commands executed by the dma controller
	int cmds_len = size_to_map;
	dma_map_inst.vaddr = (u64)mmap(NULL, cmds_len, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	dma_map_inst.size = cmds_len;
	dma_map_inst.iova = dma_map_src.size + dma_map_dst.size;
	dma_map_inst.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	ret = ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map_src);
	ret |= ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map_dst);
	ret |= ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map_inst);

	if(ret) {
		printf("Could not map DMA memory\n");
		return 1;
	}

	/* Get a file descriptor for the device */
	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, argv[2]);
	printf("=== VFIO device file descriptor %d ===\n", device);

	/* Test and setup the device */
	ret = ioctl(device, VFIO_DEVICE_GET_INFO, &device_info);

	if(ret) {
		printf("Could not get VFIO device\n");
		return 1;
	}

	printf("Device has %d region(s):\n", device_info.num_regions);

	struct vfio_region_info reg = { .argsz = sizeof(reg) };
	uchar *base_regs;

	reg.index = 0;
	ret = ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &reg);

	if(ret) {
		printf("Couldn't get region %d info\n", reg.index);
		return 1;
	}

	printf("- Region %d: size=0x%llx offset=0x%llx flags=0x%x\n",
			reg.index,
			reg.size,
			reg.offset,
			reg.flags );

	base_regs = (uchar *)mmap(NULL, reg.size, PROT_READ | PROT_WRITE, MAP_SHARED,
			device, reg.offset);

	if (base_regs != MAP_FAILED)
		printf("  - Successful MMAP to address %p\n", base_regs);

	int *src_ptr = (int *)dma_map_src.vaddr;
	int *dst_ptr = (int *)dma_map_dst.vaddr;

	*src_ptr = 0xDEADBEEF;
	*dst_ptr = 0x00000000;

	printf("source value: 0x%x\n", *src_ptr);
	printf("destination value: 0x%x\n", *dst_ptr);

	pl330_vfio_mem2mem_int(base_regs, (uchar *)dma_map_inst.vaddr, dma_map_inst.iova,
						dma_map_src.iova, dma_map_dst.iova);
	/*
	 * check result
	 */
	printf("source value: 0x%x\n", *((uint *)src_ptr));
	printf("destination value: 0x%x\n", *((uint *)dst_ptr));

	return 0;
}
