
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/param.h>
#include <inttypes.h>

#include <fcntl.h>		/* low-level i/o */
#include "cvi_buffer.h"
#include "cvi_ae_comm.h"
#include "cvi_awb_comm.h"
#include "cvi_comm_isp.h"
#include "cvi_comm_sns.h"
#include "cvi_ae.h"
#include "cvi_awb.h"
#include "cvi_isp.h"
#include "cvi_sns_ctrl.h"
#include "sample_comm.h"
#include "ae_test.h"

static SAMPLE_VI_CONFIG_S g_stViConfig;
static SAMPLE_INI_CFG_S g_stIniCfg;

static int sys_vi_init(void)
{
	MMF_VERSION_S stVersion;
	SAMPLE_INI_CFG_S	   stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;

	PIC_SIZE_E enPicSize;
	SIZE_S stSize;
	CVI_S32 s32Ret = CVI_SUCCESS;
	LOG_LEVEL_CONF_S log_conf;

	CVI_SYS_GetVersion(&stVersion);
	SAMPLE_PRT("MMF Version:%s\n", stVersion.version);

	log_conf.enModId = CVI_ID_LOG;
	log_conf.s32Level = CVI_DBG_INFO;
	CVI_LOG_SetLevelConf(&log_conf);

	// Get config from ini if found.
	if (SAMPLE_COMM_VI_ParseIni(&stIniCfg)) {
		SAMPLE_PRT("Parse complete\n");
	}

	//Set sensor number
	CVI_VI_SetDevNum(stIniCfg.devNum);
	/************************************************
	 * step1:  Config VI
	 ************************************************/
	s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	memcpy(&g_stViConfig, &stViConfig, sizeof(SAMPLE_VI_CONFIG_S));
	memcpy(&g_stIniCfg, &stIniCfg, sizeof(SAMPLE_INI_CFG_S));

	/************************************************
	 * step2:  Get input size
	 ************************************************/
	s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(stIniCfg.enSnsType[0], &enPicSize);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_VI_GetSizeBySensor failed with %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed with %#x\n", s32Ret);
		return s32Ret;
	}

	/************************************************
	 * step3:  Init modules
	 ************************************************/
	s32Ret = SAMPLE_PLAT_SYS_INIT(stSize);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("sys init failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi init failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}

	return CVI_SUCCESS;
}

static void sys_vi_deinit(void)
{
	SAMPLE_COMM_VI_DestroyIsp(&g_stViConfig);

	SAMPLE_COMM_VI_DestroyVi(&g_stViConfig);

	SAMPLE_COMM_SYS_Exit();
}

static CVI_S32 _vi_get_chn_frame(CVI_U8 chn)
{
	VIDEO_FRAME_INFO_S stVideoFrame;
	VI_CROP_INFO_S crop_info = {0};

	if (CVI_VI_GetChnFrame(0, chn, &stVideoFrame, 3000) == 0) {
		FILE *output;
		size_t image_size = stVideoFrame.stVFrame.u32Length[0] + stVideoFrame.stVFrame.u32Length[1]
				  + stVideoFrame.stVFrame.u32Length[2];
		CVI_VOID *vir_addr;
		CVI_U32 plane_offset, u32LumaSize, u32ChromaSize;
		CVI_CHAR img_name[128] = {0, };

		SAMPLE_PRT("width: %d, height: %d, total_buf_length: %zu\n",
			   stVideoFrame.stVFrame.u32Width,
			   stVideoFrame.stVFrame.u32Height, image_size);

		snprintf(img_name, sizeof(img_name), "sample_%d.yuv", chn);

		output = fopen(img_name, "wb");
		if (output == NULL) {
			memset(img_name, 0x0, sizeof(img_name));
			snprintf(img_name, sizeof(img_name), "/mnt/data/sample_%d.yuv", chn);
			output = fopen(img_name, "wb");
			if (output == NULL) {
				CVI_VI_ReleaseChnFrame(0, chn, &stVideoFrame);
				SAMPLE_PRT("fopen fail\n");
				return CVI_FAILURE;
			}
		}

		u32LumaSize =  stVideoFrame.stVFrame.u32Stride[0] * stVideoFrame.stVFrame.u32Height;
		u32ChromaSize =  stVideoFrame.stVFrame.u32Stride[1] * stVideoFrame.stVFrame.u32Height / 2;
		CVI_VI_GetChnCrop(0, chn, &crop_info);
		if (crop_info.bEnable) {
			u32LumaSize = ALIGN((crop_info.stCropRect.u32Width * 8 + 7) >> 3, DEFAULT_ALIGN) *
				ALIGN(crop_info.stCropRect.u32Height, 2);
			u32ChromaSize = (ALIGN(((crop_info.stCropRect.u32Width >> 1) * 8 + 7) >> 3, DEFAULT_ALIGN) *
				ALIGN(crop_info.stCropRect.u32Height, 2)) >> 1;
		}
		vir_addr = CVI_SYS_Mmap(stVideoFrame.stVFrame.u64PhyAddr[0], image_size);
		CVI_SYS_IonInvalidateCache(stVideoFrame.stVFrame.u64PhyAddr[0], vir_addr, image_size);
		plane_offset = 0;
		for (int i = 0; i < 3; i++) {
			if (stVideoFrame.stVFrame.u32Length[i] != 0) {
				stVideoFrame.stVFrame.pu8VirAddr[i] = vir_addr + plane_offset;
				plane_offset += stVideoFrame.stVFrame.u32Length[i];
				SAMPLE_PRT(
					   "plane(%d): paddr(%#"PRIx64") vaddr(%p) stride(%d) length(%d)\n",
					   i, stVideoFrame.stVFrame.u64PhyAddr[i],
					   stVideoFrame.stVFrame.pu8VirAddr[i],
					   stVideoFrame.stVFrame.u32Stride[i],
					   stVideoFrame.stVFrame.u32Length[i]);
				fwrite((void *)stVideoFrame.stVFrame.pu8VirAddr[i]
					, (i == 0) ? u32LumaSize : u32ChromaSize, 1, output);
			}
		}
		CVI_SYS_Munmap(vir_addr, image_size);

		if (CVI_VI_ReleaseChnFrame(0, chn, &stVideoFrame) != 0)
			SAMPLE_PRT("CVI_VI_ReleaseChnFrame NG\n");

		fclose(output);
		return CVI_SUCCESS;
	}
	SAMPLE_PRT("CVI_VI_GetChnFrame NG\n");
	return CVI_FAILURE;
}

static long diff_in_us(struct timespec t1, struct timespec t2)
{
	struct timespec diff;

	if (t2.tv_nsec-t1.tv_nsec < 0) {
		diff.tv_sec  = t2.tv_sec - t1.tv_sec - 1;
		diff.tv_nsec = t2.tv_nsec - t1.tv_nsec + 1000000000;
	} else {
		diff.tv_sec  = t2.tv_sec - t1.tv_sec;
		diff.tv_nsec = t2.tv_nsec - t1.tv_nsec;
	}
	return (diff.tv_sec * 1000000.0 + diff.tv_nsec / 1000.0);
}

static void ViConfigReInit(SAMPLE_VI_CONFIG_S *p_stViConfig, SAMPLE_INI_CFG_S *p_stIniCfg)
{
	int s32WorkSnsId = 0;

	for (s32WorkSnsId = 0; s32WorkSnsId < p_stIniCfg->devNum; s32WorkSnsId++) {
		p_stViConfig->s32WorkingViNum					= 1 + s32WorkSnsId;
		p_stViConfig->as32WorkingViId[s32WorkSnsId]			= s32WorkSnsId;
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.enSnsType	=
			p_stIniCfg->enSnsType[s32WorkSnsId];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.MipiDev		=
			p_stIniCfg->MipiDev[s32WorkSnsId];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.s32BusId	=
			p_stIniCfg->s32BusId[s32WorkSnsId];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as16LaneId[0]   =
			p_stIniCfg->as16LaneId[s32WorkSnsId][0];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as16LaneId[1]   =
			p_stIniCfg->as16LaneId[s32WorkSnsId][1];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as16LaneId[2]   =
			p_stIniCfg->as16LaneId[s32WorkSnsId][2];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as16LaneId[3]   =
			p_stIniCfg->as16LaneId[s32WorkSnsId][3];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as16LaneId[4]   =
			p_stIniCfg->as16LaneId[s32WorkSnsId][4];

		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as8PNSwap[0] =
			p_stIniCfg->as8PNSwap[s32WorkSnsId][0];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as8PNSwap[1] =
			p_stIniCfg->as8PNSwap[s32WorkSnsId][1];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as8PNSwap[2] =
			p_stIniCfg->as8PNSwap[s32WorkSnsId][2];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as8PNSwap[3] =
			p_stIniCfg->as8PNSwap[s32WorkSnsId][3];
		p_stViConfig->astViInfo[s32WorkSnsId].stSnsInfo.as8PNSwap[4] =
			p_stIniCfg->as8PNSwap[s32WorkSnsId][4];
		p_stViConfig->astViInfo[s32WorkSnsId].stDevInfo.enWDRMode =
			p_stIniCfg->enWDRMode[s32WorkSnsId];
	}
}

static CVI_S32 sensor_dump_yuv(void)
{
	CVI_S32 loop = 0;
	CVI_U32 ok = 0, ng = 0;
	CVI_U8  chn = 0;
	int tmp;
	struct timespec start, end;

	SAMPLE_PRT("Get frm from which chn(0~1): ");
	scanf("%d", &tmp);
	chn = tmp;
	SAMPLE_PRT("how many loops to do(11111 is infinite: ");
	scanf("%d", &loop);
	while (loop > 0) {
		clock_gettime(CLOCK_MONOTONIC, &start);
		if (_vi_get_chn_frame(chn) == CVI_SUCCESS) {
			++ok;
			clock_gettime(CLOCK_MONOTONIC, &end);
			SAMPLE_PRT("ms consumed: %f\n",
						(CVI_FLOAT)diff_in_us(start, end)/1000);
		} else
			++ng;
		//sleep(1);
		if (loop != 11111)
			loop--;
	}
	SAMPLE_PRT("VI GetChnFrame OK(%d) NG(%d)\n", ok, ng);

	SAMPLE_PRT("Dump VI yuv TEST-PASS\n");

	return CVI_SUCCESS;
}

static CVI_S32 sensor_flip_mirror(void)
{
	int flip;
	int mirror;
	int chnID;
	int pipeID;

	SAMPLE_PRT("chn(0~1): ");
	scanf("%d", &chnID);
	SAMPLE_PRT("Flip enable/disable(1/0): ");
	scanf("%d", &flip);
	SAMPLE_PRT("Mirror enable/disable(1/0): ");
	scanf("%d", &mirror);
	pipeID = chnID;
	CVI_VI_SetChnFlipMirror(pipeID, chnID, flip, mirror);

	return CVI_SUCCESS;
}

static CVI_S32 sensor_dump_raw(void)
{
	VIDEO_FRAME_INFO_S stVideoFrame[2];
	VI_DUMP_ATTR_S attr;
	struct timeval tv1;
	int frm_num = 1, j = 0;
	CVI_U32 dev = 0, loop = 0;
	struct timespec start, end;
	CVI_S32 s32Ret = CVI_SUCCESS;

	memset(stVideoFrame, 0, sizeof(stVideoFrame));

	stVideoFrame[0].stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_BAYER_12BPP;
	stVideoFrame[1].stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_BAYER_12BPP;

	SAMPLE_PRT("To get raw dump from dev(0~1): ");
	scanf("%d", &dev);

	attr.bEnable = 1;
	attr.u32Depth = 0;
	attr.enDumpType = VI_DUMP_TYPE_RAW;

	CVI_VI_SetPipeDumpAttr(dev, &attr);

	attr.bEnable = 0;
	attr.enDumpType = VI_DUMP_TYPE_IR;

	CVI_VI_GetPipeDumpAttr(dev, &attr);

	SAMPLE_PRT("Enable(%d), DumpType(%d):\n", attr.bEnable, attr.enDumpType);
	SAMPLE_PRT("how many loops to do (1~60): ");
	scanf("%d", &loop);

	if (loop > 60)
		return s32Ret;

	while (loop > 0) {
		clock_gettime(CLOCK_MONOTONIC, &start);
		frm_num = 1;

		CVI_VI_GetPipeFrame(dev, stVideoFrame, 1000);

		if (stVideoFrame[1].stVFrame.u64PhyAddr[0] != 0)
			frm_num = 2;

		gettimeofday(&tv1, NULL);

		for (j = 0; j < frm_num; j++) {
			size_t image_size = stVideoFrame[j].stVFrame.u32Length[0];
			unsigned char *ptr = calloc(1, image_size);
			FILE *output;
			char img_name[128] = {0,}, order_id[8] = {0,};

			if (attr.enDumpType == VI_DUMP_TYPE_RAW) {
				stVideoFrame[j].stVFrame.pu8VirAddr[0]
					= CVI_SYS_Mmap(stVideoFrame[j].stVFrame.u64PhyAddr[0]
					  , stVideoFrame[j].stVFrame.u32Length[0]);
				SAMPLE_PRT("paddr(%#"PRIx64") vaddr(%p)\n",
							stVideoFrame[j].stVFrame.u64PhyAddr[0],
							stVideoFrame[j].stVFrame.pu8VirAddr[0]);

				memcpy(ptr, (const void *)stVideoFrame[j].stVFrame.pu8VirAddr[0],
					stVideoFrame[j].stVFrame.u32Length[0]);
				CVI_SYS_Munmap((void *)stVideoFrame[j].stVFrame.pu8VirAddr[0],
						stVideoFrame[j].stVFrame.u32Length[0]);

				switch (stVideoFrame[j].stVFrame.enBayerFormat) {
				default:
				case BAYER_FORMAT_BG:
					snprintf(order_id, sizeof(order_id), "BG");
					break;
				case BAYER_FORMAT_GB:
					snprintf(order_id, sizeof(order_id), "GB");
					break;
				case BAYER_FORMAT_GR:
					snprintf(order_id, sizeof(order_id), "GR");
					break;
				case BAYER_FORMAT_RG:
					snprintf(order_id, sizeof(order_id), "RG");
					break;
				}

				snprintf(img_name, sizeof(img_name),
						"./vi_%d_%s_%s_w_%d_h_%d_x_%d_y_%d_tv_%ld_%ld.raw",
						dev, (j == 0) ? "LE" : "SE", order_id,
						stVideoFrame[j].stVFrame.u32Width,
						stVideoFrame[j].stVFrame.u32Height,
						stVideoFrame[j].stVFrame.s16OffsetLeft,
						stVideoFrame[j].stVFrame.s16OffsetTop,
						tv1.tv_sec, tv1.tv_usec);

				SAMPLE_PRT("dump image %s\n", img_name);

				output = fopen(img_name, "wb");

				fwrite(ptr, image_size, 1, output);
				fclose(output);
				free(ptr);
			}
		}

		CVI_VI_ReleasePipeFrame(dev, stVideoFrame);

		clock_gettime(CLOCK_MONOTONIC, &end);
		SAMPLE_PRT("ms consumed: %f\n",
					(CVI_FLOAT)diff_in_us(start, end) / 1000);

		loop--;
	}

	SAMPLE_PRT("Dump VI raw TEST-PASS\n");

	return s32Ret;
}

static CVI_S32 sensor_linear_wdr_switch(void)
{
	int tmp;
	CVI_U8 wdrMode = 0;
	CVI_S32 s32Ret = CVI_SUCCESS;

	SAMPLE_COMM_VI_DestroyIsp(&g_stViConfig);
	// Stop VI.
	SAMPLE_COMM_VI_DestroyVi(&g_stViConfig);
	// Close ISP device.
	s32Ret = SAMPLE_COMM_VI_CLOSE();
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi close failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}
	// select which mode want to switch.
	SAMPLE_PRT("Please select sensor input mode (0:linear/1:wdr): ");
	scanf("%d", &tmp);
	wdrMode = tmp;
	if (wdrMode == 0) {
		// Reset main sensor initial config to linear setting.
		g_stIniCfg.enSnsType[0] = SONY_IMX327_2L_MIPI_2M_30FPS_12BIT;
		g_stIniCfg.enWDRMode[0] = WDR_MODE_NONE;
		// Reset slave sensor initial config to linear setting.
		g_stIniCfg.enSnsType[1] = SONY_IMX327_SLAVE_MIPI_2M_30FPS_12BIT;
		g_stIniCfg.enWDRMode[1] = WDR_MODE_NONE;
	} else {
		// Reset main sensor initial config to wdr setting.
		g_stIniCfg.enSnsType[0] = SONY_IMX327_2L_MIPI_2M_30FPS_12BIT_WDR2TO1;
		g_stIniCfg.enWDRMode[0] = WDR_MODE_2To1_LINE;
		// Reset slave sensor initial config to wdr setting.
		g_stIniCfg.enSnsType[1] = SONY_IMX327_SLAVE_MIPI_2M_30FPS_12BIT_WDR2TO1;
		g_stIniCfg.enWDRMode[1] = WDR_MODE_2To1_LINE;
	}
	// Reconfig VI setting for different mode Re-initial correctly.
	ViConfigReInit(&g_stViConfig, &g_stIniCfg);
	// open Isp device.
	s32Ret = SAMPLE_COMM_VI_OPEN();
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi open failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}
	// Initial VI & ISP.
	s32Ret = SAMPLE_PLAT_VI_INIT(&g_stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi init failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}

	return s32Ret;
}

int sensor_dump(void)
{
	CVI_U64 addr = 0;
	CVI_U32 size = 0;
	FILE *output;
	char img_name[128] = {0, };

	SAMPLE_PRT("dump addr: ");
	scanf("%lx", &addr);
	SAMPLE_PRT("dump size(0\1): ");
	scanf("%x", &size);

	snprintf(img_name, sizeof(img_name), "register_%lx.bin", addr);

	output = fopen(img_name, "wb");
	if (output == NULL) {
		memset(img_name, 0x0, sizeof(img_name));
		snprintf(img_name, sizeof(img_name), "register_%lx.bin", addr);
		output = fopen(img_name, "wb");
		if (output == NULL) {
			SAMPLE_PRT("fopen fail\n");
			return CVI_FAILURE;
		}
	}

	void *vir_addr = CVI_SYS_Mmap(addr, size);
	if (vir_addr == NULL) {
		SAMPLE_PRT("CVI_SYS_Mmap fail\n");
		fclose(output);
		return CVI_FAILURE;
	}

	fwrite(vir_addr, size, 1, output);
	fflush(output);

	CVI_SYS_Munmap(vir_addr, size);

	fclose(output);

	return CVI_SUCCESS;
}

int sensor_proc(void)
{
	CVI_S32 s32Ret = CVI_SUCCESS;
	CVI_S32 op;

	SAMPLE_PRT("---debug_info------------------------------------------------\n");
	SAMPLE_PRT("1: /proc/cvitek/vi_dbg\n");
	SAMPLE_PRT("2: /proc/cvitek/vi\n");
	SAMPLE_PRT("3: /proc/mipi-rx\n");
	SAMPLE_PRT("input your choice: ");
	scanf("%d", &op);

	switch (op) {
	case 1:
		system("cat /proc/cvitek/vi_dbg");
		break;
	case 2:
		system("cat /proc/cvitek/vi");
		break;
	case 3:
		system("cat /proc/mipi-rx");
		break;
	default:
		break;
	}

	return s32Ret;
}

#ifdef ENABLE_LOAD_ISPD_SO

#include <dlfcn.h>

void load_ispd(void)
{
#define ISPD_LIBNAME "libcvi_ispd2.so"
#define ISPD_CONNECT_PORT 5566

	char *error = NULL;
	void *handle = NULL;

	dlerror();
	handle = dlopen(ISPD_LIBNAME, RTLD_NOW);

	if (handle) {
		void (*daemon_init)(unsigned int port);

		printf("Load dynamic library %s success\n", ISPD_LIBNAME);

		dlerror();
		daemon_init = dlsym(handle, "isp_daemon2_init");
		error = dlerror();

		if (error == NULL) {
			(*daemon_init)(ISPD_CONNECT_PORT);
		} else {
			printf("Run daemon initial fail, %s\n", error);
			dlclose(handle);
		}
	} else {
		error = dlerror();
		printf("dlopen: %s, error: %s\n", ISPD_LIBNAME, error);
	}
}
#endif

int main(int argc, char **argv)
{
	CVI_S32 s32Ret = CVI_SUCCESS;
	CVI_S32 op;

	UNUSED(argc);
	UNUSED(argv);

	setbuf(stdout, NULL);

	s32Ret = sys_vi_init();
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

#ifdef ENABLE_LOAD_ISPD_SO
	load_ispd();
#endif

	usleep(500 * 1000);

	system("stty erase ^H");

	do {
		SAMPLE_PRT("---Basic------------------------------------------------\n");
		SAMPLE_PRT("1: dump vi raw frame\n");
		SAMPLE_PRT("2: dump vi yuv frame\n");
		SAMPLE_PRT("3: set chn flip/mirror\n");
		SAMPLE_PRT("4: linear wdr switch\n");
		SAMPLE_PRT("5: AE debug\n");
		SAMPLE_PRT("6: sensor dump\n");
		SAMPLE_PRT("7: sensor proc\n");
		SAMPLE_PRT("255: exit\n");
	    SAMPLE_PRT("input your choice: ");
		scanf("%d", &op);

		switch (op) {
		case 1:
			s32Ret = sensor_dump_raw();
			break;
		case 2:
			s32Ret = sensor_dump_yuv();
			break;
		case 3:
			s32Ret = sensor_flip_mirror();
			break;
		case 4:
			s32Ret = sensor_linear_wdr_switch();
			break;
		case 5:
			s32Ret = sensor_ae_test();
			break;
		case 6:
			s32Ret = sensor_dump();
			break;
		case 7:
			s32Ret = sensor_proc();
			break;
		default:
			break;
		}
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("op(%d) failed with %#x!\n", op, s32Ret);
			break;
		}
	} while (op != 255);

	sys_vi_deinit();

	return s32Ret;
}