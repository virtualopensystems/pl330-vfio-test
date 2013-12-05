#include "pl330_vfio.h"

#include <linux/types.h>
#include <errno.h>
#include <linux/vfio.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/fcntl.h>


static int pl330_set_burst_size(uint val, enum dst_src type, uint *reg)
{
	if(val & (val - 1) || val > CCR_BURSTSIZE_MAX) {
		// error, not power of 2
		return -1;
	}

	uchar ret = 0;
	while(val >>= 1) {
		ret++;
	} // see page 3-27

	switch(type){
	case SRC:
		*reg |= ret << CCR_SRCBURSTSIZE_SHIFT;
	case DST:
		*reg |= ret << CCR_DSTBURSTSIZE_SHIFT;
	default:
		return -1;
	}

	return 0;
}

static int pl330_set_burst_length(uint val, enum dst_src type, uint *reg)
{
	if(!val || val > CCR_BURSTLEN_MAX) {
		return -1;
	}

	switch(type) {
	case SRC:
		*reg |= (val - 1) << CCR_SRCBURSTLEN_SHIFT;
	case DST:
		*reg |= (val - 1) << CCR_DSTBURSTLEN_SHIFT;
	default:
		return -1;
	}

	return 0;
}

static void pl330_vfio_req_config_init(struct req_config *config)
{
	config->config_ops.set_burst_size = pl330_set_burst_size;
	config->config_ops.set_burst_length = pl330_set_burst_length;
}

static void apply_CCR_config(struct req_config *config, uint *reg) {

}

/*
 * end CCR  configuration
 * */

static inline uchar ith_uchar(void *buf, int pos)
{
	uchar *ptr = buf + pos;
	
	return *ptr;
}

static inline uint insert_DMAEND(uchar *buffer)
{
	buffer[0] = DMAEND;

	DEBUG_MSG("DMAEND\n");

	return DMAEND_SIZE;
}

static inline uint insert_DMAMOV(uchar *buffer, enum DMAMOV_type type,
		uint dst)
{
	buffer[0] = DMAMOV;
	switch(type) {
		case SAR:
			buffer[1] = _SAR;
			break;
		case CCR:
			buffer[1] = _CCR;
			break;
		case DAR:
			buffer[1] = _DAR;
			break;
		default:
			error(-1, EINVAL, "insert_DMAMOV()");
	}
	uint *ptr = (uint *)&buffer[2];
	*ptr = dst;

	DEBUG_MSG("DMAMOV %x %x\n", buffer[1], *((uint *)&buffer[2]));

	return DMAMOV_SIZE;
}

static inline uint insert_DMALP(uchar *buffer, enum DMA_LOOP_REGISTER type,
		uchar count)
{
	switch(type) {
		case LOOP_CNT_0_REG:
			buffer[0] = DMALP;
			break;
		case LOOP_CNT_1_REG:
			buffer[0] = DMALP | (1 << 1);
			break;
		default:
			error(-1, EINVAL, "insert_DMALP()");
	}

	buffer[1] = count - 1;

	DEBUG_MSG("DMALP LOOP_CNT_%d_REG, count: %d\n", type, count);
	
	return DMALP_SIZE;
}

struct args_DMALPEND {
	enum request_type type;  // burst, single
	enum DMA_LOOP_REGISTER loop_cnt_num;
	uchar backflip_jump;
};

static inline uint insert_DMALPEND(uchar *buffer, enum LOOP_START_TYPE type,
		struct args_DMALPEND *args)
{
	buffer[0] = DMALPEND;

	switch(type) {
		case BY_DMALP: 
			// TODO add "forever" support
			// set by dmalp
			buffer[0] |= 1 << 4;
			// set dma loop register
			buffer[0] |= type << 2;

			switch(args->type) {
			case SINGLE:
				buffer[0] |= (0 << 1) | (1 << 0);
			case BURST:
				buffer[0] |= (1 << 1) | (1 << 0);
			case ALWAYS:
				break;
			default:
				error(-1, EINVAL, "insert_DMALPEND(), type error");
			}

			buffer[1] = args->backflip_jump;
			break;
		case BY_DMALPFE:
			/*TODO*/
			break;
		default:
			error(-1, EINVAL, "insert_DMALPEND()");
	}

	return DMALPEND_SIZE;
}

// TODO SINGLE and BURST cases
static inline uint insert_DMALD(uchar *buf)
{
	buf[0] = DMALD;
	return DMALD_SIZE;
}

// TODO SINGLE and BURST cases
static inline uint insert_DMAST(uchar *buf)
{
	buf[0] = DMAST;
	return DMAST_SIZE;
}

static inline uint insert_DMARMB(uchar *buf)
{
	buf[0] = DMARMB; 
	return DMARMB_SIZE;
}

static inline uint insert_DMAWMB(uchar *buf)
{
	buf[0] = DMAWMB;
	return DMAWMB_SIZE;
}

static inline uint insert_DMAGO(uchar *buf, uchar channel,
		uint address, bool nonsecure)
{
	buf[0] = DMAGO;
	buf[0] |= (nonsecure) ? (1 << 1) : (0 << 1);
	buf[1] = channel & 0x7;

	*((uint *)&buf[2]) = address;

	return DMAGO_SIZE;
}

static inline uint insert_DMASEV(uchar *buf, uchar event) 
{
	buf[0] = DMASEV;

	event &= 0x1f;
	buf[1] = (event << 3); // see page 4-15

	DEBUG_MSG("DMASEV event: %d\n", event);

	return DMASEV_SIZE;
}

static inline void submit_to_DBGINST(uchar *dbg_instrs, uchar* base_regs)
{
	uint val;

	val = (dbg_instrs[0] << 16) | (dbg_instrs[1] << 24);

	*((uint *)(base_regs + DBGINST0)) = val;

	*((uint *)(base_regs + DBGINST1)) = *((uint *)&dbg_instrs[2]);

	// GO
	*((uint *)(base_regs + DBGCMD)) = 0;
}

static bool is_dmac_idle(const uchar *regs)
{
	if (*((uint *)(regs + DBGSTATUS)) & DBG_BUSY_MASK) {
		return false;
	} else {
		return true;
	}
}

static void build_cmds(uchar *buffer)
{

}

static inline void pl330_vfio_build_CCR(uint * ccr, struct req_config *config)
{
	config->config_ops.set_burst_size(config->src_burst_size, SRC, ccr);
	config->config_ops.set_burst_length(config->src_burst_len, SRC, ccr);
	config->config_ops.set_burst_size(config->dst_burst_size, DST, ccr);
	config->config_ops.set_burst_length(config->dst_burst_len, DST, ccr);

	*ccr |= (config->src_inc & 0x1);
	*ccr |= ((config->dst_inc & 0x1) << CCR_DSTINC_SHIFT);

	*ccr |= ((config->src_prot_ctrl & 0x7) << CCR_SRCPROTCTRL_SHIFT);
	*ccr |= ((config->dst_prot_ctrl & 0x7) << CCR_DSTPROTCTRL_SHIFT);

	*ccr |= ((config->src_cache_ctrl & 0x7) << CCR_SRCCACHECTRL_SHIFT);
	*ccr |= ((config->dst_cache_ctrl & 0x7) << CCR_DSTCACHECTRL_SHIFT);

}

static void pl330_vfio_dma_map_init(struct vfio_iommu_type1_dma_map *map,
						u64 iova, u64 size)
{
	map->argsz = sizeof(map);
	map->vaddr = (u64)mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	map->size = size;
	map->iova = iova;
	map->flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
}

static void setup_load_store(uchar *buf, uint *offset, enum transfer_type t_type)
{
	switch(t_type) {
	case MEM2MEM: // TODO handle different design revision
		*offset += insert_DMALD(&buf[*offset]);
		*offset += insert_DMARMB(&buf[*offset]);
		*offset += insert_DMAST(&buf[*offset]);
		*offset += insert_DMAWMB(&buf[*offset]);
		break;
	case MEM2DEV: // TODO
	case DEV2MEM: // TODO
	default:
		error(-1, EINVAL, "setup_load_store");
	}
}

static uint add_inner_outer_loops(uchar *buf, uint *offset, uint in_cnt,
					uint out_cnt, enum transfer_type t_type)
{
	int out_off, in_off = 0;
	struct args_DMALPEND args;

	if(out_cnt > 1) {
		// outer loop : LOOP_CNT_1_REG
		*offset += insert_DMALP(&buf[*offset], LOOP_CNT_1_REG, out_cnt);
		out_off = *offset;	
		DEBUG_MSG("        outer_loop_off:%u, cnt: %u\n", out_off, out_cnt);
	}
	// inner loop : LOOP_CNT_0_REG
	*offset += insert_DMALP(&buf[*offset], LOOP_CNT_0_REG, in_cnt);
	in_off = *offset;
	DEBUG_MSG("        inner_loop_off:%u, cnt: %u\n", in_off, in_cnt);

	/*
	 * Load&Store operations
	 * */
	setup_load_store(buf, offset, t_type); 

	// insert end of inner loop
	args.type = ALWAYS;
	args.loop_cnt_num = LOOP_CNT_0_REG;
	args.backflip_jump = *offset - in_off;
	*offset += insert_DMALPEND(&buf[*offset], LOOP_CNT_0_REG, &args);
	DEBUG_MSG("        inner_loop_end:%u, backjmp: %d\n", *offset, args.backflip_jump);
	
	if(out_cnt > 1) {
		args.type = ALWAYS;
		args.loop_cnt_num = LOOP_CNT_1_REG;
		args.backflip_jump = *offset - out_off;
		*offset += insert_DMALPEND(&buf[*offset], BY_DMALP, &args);
		DEBUG_MSG("        outer_loop_end:%u, backjmp: %d\n", *offset, args.backflip_jump);
	}
}

static int setup_req_loops(uchar *buf_cmds, uint *offset, uint burst_size, uint burst_len, uint size,
								enum transfer_type t_type)
{
	/*
	 * full loop means two nested loop of
	 * 256 iterations each
	 * */
	int full_loop_cnt = 0;
	int full_loop_len = 65536; // 256*256

	unsigned long remaining_burst = 0;

	/* 
	 * size has to be aligned with the amount of data
	 * moved every burst
	 * */
	if(size % (burst_size * burst_len)) {
		return -1;
	}
	unsigned long burst_count = NUM_OF_BURST(size,
				burst_len, burst_size);

	DEBUG_MSG("set up loops:\n");
	DEBUG_MSG("    burst_cnt:%lu\n", burst_count);

	full_loop_cnt = burst_count / full_loop_len;
	remaining_burst = burst_count % full_loop_len;

	DEBUG_MSG("    full_loop_cnt:%u\n", full_loop_cnt);
	DEBUG_MSG("    remaining_burst:%lu\n", remaining_burst);

	while(full_loop_cnt--) {
		add_inner_outer_loops(buf_cmds, offset, 256, 256, t_type);	
	}

	// there could be n < 256*256 bursts left
	// TODO add loop to handle more than one full loop 
	if(remaining_burst >= 256) {
		add_inner_outer_loops(buf_cmds, offset, 256, remaining_burst/256, t_type);
		remaining_burst %= 256;
		DEBUG_MSG("    remaining_burst_1:%lu\n", remaining_burst);
	}

	// there could be n < 256 bursts left
	if(remaining_burst) {
		add_inner_outer_loops(buf_cmds, offset, remaining_burst, 0, t_type);
	}
	
	return 0;
}

/*
 * insert required commands to set up the request.
 * */
int generate_cmds_from_request(uchar *cmds_buf, struct req_config *config)
{
	uint offset = 0;

	uint ccr_conf = 0;
	pl330_vfio_build_CCR(&ccr_conf, config);

	// add instructions to configure CCR, SAR and DAR
	offset += insert_DMAMOV(cmds_buf, CCR, ccr_conf);
	offset += insert_DMAMOV(&cmds_buf[offset], SAR, config->iova_src);
	offset += insert_DMAMOV(&cmds_buf[offset], DAR, config->iova_dst);
	
	// set up loops, if any TODO handle src and dst burst size/length
	if (setup_req_loops(cmds_buf, &offset, config->src_burst_size,
			config->src_burst_len, config->size, config->t_type)) {
		return -1;
	}

	// enable interrupts for the only supported thread
	offset += insert_DMASEV(&cmds_buf[offset], 0);

	// terminate transaction
	offset += insert_DMAEND(&cmds_buf[offset]);
}

int pl330_vfio_mem2mem_defconfig(struct req_config *config)
{
	pl330_vfio_req_config_init(config);

	config->src_inc = config->dst_inc = INC_DEF_VAL;
	config->src_prot_ctrl = config->dst_prot_ctrl = CCR_PROTCTRL_DEF_VAL;
	config->src_cache_ctrl = config->dst_cache_ctrl = CCR_CACHECTRL_DEF_VAL;

	config->src_burst_size = config->dst_burst_size = CCR_BURSTSIZE_MAX;
	config->src_burst_len = config->dst_burst_len = CCR_BURSTLEN_MAX;

	config->t_type = MEM2MEM;
}

int pl330_vfio_submit_req(uchar *regs, uchar *cmds, u64 iova_cmds)
{
	if(!is_dmac_idle(regs)) {
		return -1;
	}

	uchar ins_debug[6] = {0, 0, 0, 0, 0, 0};
	
	uchar channel_id = 0;
	bool non_secure = true;

	insert_DMAGO(ins_debug, channel_id, iova_cmds,
			non_secure);

	submit_to_DBGINST(ins_debug, regs); 

	return 0;
}

int pl330_vfio_mem2mem_int(uchar *regs, uchar *cmds, u64 iova_cmds,
					u64 iova_src, u64 iova_dst)
{
	int offset = 0;
	struct req_config config;
	pl330_vfio_req_config_init(&config);

	config.src_inc = config.dst_inc = INC_DEF_VAL;
	config.src_prot_ctrl = config.dst_prot_ctrl = CCR_PROTCTRL_DEF_VAL;
	config.src_cache_ctrl = config.dst_cache_ctrl = CCR_CACHECTRL_DEF_VAL;
	config.t_type = MEM2MEM;

	config.size = 4; // int size
	config.src_burst_size = config.dst_burst_size = 4;
	config.src_burst_len = config.dst_burst_len = 1;

	uint ccr_conf = 0;
	pl330_vfio_build_CCR(&ccr_conf, &config);

	offset += insert_DMAMOV(cmds, CCR, ccr_conf);
	offset += insert_DMAMOV(&cmds[offset], SAR, iova_src);
	offset += insert_DMAMOV(&cmds[offset], DAR, iova_dst);
	offset += insert_DMALP(&cmds[offset], LOOP_CNT_0_REG, 0);
	offset += insert_DMALD(&cmds[offset]);
	offset += insert_DMARMB(&cmds[offset]);
	offset += insert_DMAST(&cmds[offset]);
	offset += insert_DMAWMB(&cmds[offset]);

	struct args_DMALPEND args = {ALWAYS, LOOP_CNT_0_REG, 4};
	offset += insert_DMALPEND(&cmds[offset], BY_DMALP, &args);

	offset += insert_DMAEND(&cmds[offset]);

	if (!is_dmac_idle(regs)) {
		return -1;
	}

	uchar ins_debug[6] = {0, 0, 0, 0, 0, 0};
	
	uchar channel_id = 0;
	bool non_secure = true;

	insert_DMAGO(ins_debug, channel_id, iova_cmds,
			non_secure);

	submit_to_DBGINST(ins_debug, regs); 

	return 0;
}
