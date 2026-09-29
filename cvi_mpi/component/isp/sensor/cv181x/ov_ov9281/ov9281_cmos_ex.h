#ifndef __OV9281_CMOS_EX_H_
#define __OV9281_CMOS_EX_H_

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif


#include <linux/cvi_type.h>
#include "cvi_sns_ctrl.h"


enum ov9281_linear_regs_e {
	LINEAR_EXP_0 = 0,
	LINEAR_EXP_1,
	LINEAR_EXP_2,
	LINEAR_AGAIN0,
	LINEAR_AGAIN1,
	LINEAR_VTS_0,
	LINEAR_VTS_1,
	LINEAR_REGS_NUM
};

typedef enum _OV9281_MODE_E {
	OV9281_MODE_1280X800P120 = 0,
	OV9281_MODE_LINEAR_NUM,
	OV9281_MODE_NUM
} OV9281_MODE_E;

typedef struct _OV9281_STATE_S {
	CVI_U32		u32Sexp_MAX;
} OV9281_STATE_S;

typedef struct _OV9281_MODE_S {
	ISP_WDR_SIZE_S astImg[2];
	CVI_FLOAT f32MaxFps;
	CVI_FLOAT f32MinFps;
	CVI_U32 u32HtsDef;
	CVI_U32 u32VtsDef;
	CVI_U16 u16L2sOffset;
	CVI_U16 u16TopBoundary;
	CVI_U16 u16BotBoundary;
	SNS_ATTR_S stExp[2];
	SNS_ATTR_S stAgain[2];
	SNS_ATTR_S stDgain[2];
	CVI_U32 u32L2S_MAX;
	char name[64];
} OV9281_MODE_S;

/****************************************************************************
 * external variables and functions                                         *
 ****************************************************************************/

extern ISP_SNS_STATE_S *g_pastOv9281[VI_MAX_PIPE_NUM];
extern ISP_SNS_COMMBUS_U g_aunOv9281_BusInfo[];
extern CVI_U16 g_au16Ov9281_GainMode[];
extern CVI_U16 g_au16Ov9281_L2SMode[VI_MAX_PIPE_NUM];
extern CVI_U8 ov9281_i2c_addr;
extern const CVI_U32 ov9281_addr_byte;
extern const CVI_U32 ov9281_data_byte;
extern void ov9281_init(VI_PIPE ViPipe);
extern void ov9281_exit(VI_PIPE ViPipe);
extern void ov9281_standby(VI_PIPE ViPipe);
extern void ov9281_restart(VI_PIPE ViPipe);
extern int  ov9281_write_register(VI_PIPE ViPipe, int addr, int data);
extern int  ov9281_read_register(VI_PIPE ViPipe, int addr);
extern void ov9281_mirror_flip(VI_PIPE ViPipe, ISP_SNS_MIRRORFLIP_TYPE_E eSnsMirrorFlip);
extern int ov9281_probe(VI_PIPE ViPipe);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */


#endif /* __OV9281_CMOS_EX_H_ */
